# FNC Reborn (TU2 + DLC) - build script.
#
# Builds a native Windows version of Fight Night Champion (Xbox 360, 2011) from
# YOUR OWN copy of the game: your disc image, your title update 2 package and
# (optionally) your DLC packages. Nothing from the game is included in this
# repository; the script extracts your files, recompiles the game's executable
# (with title update 2 applied) locally, places the result in .\dist and
# installs your DLC to Documents\FNCReborn-TU2.
#
# Usage (normally via setup.bat):
#   powershell -ExecutionPolicy Bypass -File scripts\setup.ps1 [-Iso <path>]
#       [-TitleUpdate <TU2 package or default.xexp>] [-Dlc <folder>] [-NoDlc] [-Clean]

param(
    [string]$Iso = "",
    [string]$TitleUpdate = "",
    [string]$Dlc = "",
    [switch]$NoDlc,
    [switch]$Clean,
    [switch]$NoPause
)

# Native tools report failure through exit codes, checked by Run below.
$ErrorActionPreference = "Continue"
$Root = Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path)
$Build = Join-Path $Root "build"
$Dist = Join-Path $Root "dist"
$GameDir = Join-Path $Dist "game"
$SdkSrc = Join-Path $Build "rexglue-sdk"
$SdkInstall = Join-Path $Build "sdk"
$GameBuild = Join-Path $Build "game"
$Manifest = Join-Path $Root "config\fnc_manifest.toml"
$ToolBuild = Join-Path $Build "fnc_content"
$ContentTool = Join-Path $ToolBuild "fnc_content.exe"
# Saves and DLC of this build (FNCReborn.exe redirects Documents\FNCReborn here).
$UserData = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "FNCReborn-TU2"
$OldUserData = Join-Path ([Environment]::GetFolderPath("MyDocuments")) "FNCReborn"
$TitleId = "45410915"
$LegendsPack = "E40F674A98DA244CD018C51ACA9815966328CED345"   # Fight Night Legends Pack
$KnownXexp = "F0B4EE711583F7D34C00F246EB71838A4BFC2783"        # title update 2 default.xexp (SHA-1)

$SdkRepo = "https://github.com/rexglue/rexglue-sdk.git"
$SdkCommit = "c94f5ebdcb3c9d1a460ca48e04f9758448f8d518"   # ReXGlue SDK v0.10.0

New-Item -ItemType Directory -Force -Path $Build, $Dist | Out-Null
Start-Transcript -Path (Join-Path $Build "setup.log") -Force | Out-Null

function Step($text) { Write-Host "`n== $text ==" -ForegroundColor Cyan }
function Fail($text) {
    Write-Host "`nERROR: $text" -ForegroundColor Red
    Write-Host "A full log is in build\setup.log"
    Stop-Transcript | Out-Null
    if (-not $NoPause) { Read-Host "Press Enter to close" | Out-Null }
    exit 1
}
function Run($exe, [string[]]$argList) {
    & $exe @argList
    if ($LASTEXITCODE -ne 0) { Fail "'$exe $($argList -join ' ')' failed (exit code $LASTEXITCODE)." }
}

if ($Clean) {
    Step "Removing previous build output"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $GameBuild, (Join-Path $Build "generated"), (Join-Path $Build "stamps")
}
$Stamps = Join-Path $Build "stamps"
New-Item -ItemType Directory -Force -Path $Stamps | Out-Null
# A step is redone when its inputs change (e.g. after pulling an update).
function Done($name, $key) {
    $f = Join-Path $Stamps $name
    (Test-Path $f) -and ((Get-Content -Raw $f).Trim() -eq $key)
}
function MarkDone($name, $key) { Set-Content -Path (Join-Path $Stamps $name) -Value $key }
function FileHash($path) { (Get-FileHash -Algorithm SHA1 $path).Hash }

# Xbox 360 content packages start with "LIVE", "PIRS" or "CON ".
function IsPackage($path) {
    try {
        $s = [System.IO.File]::OpenRead($path)
        $b = New-Object byte[] 4
        $n = $s.Read($b, 0, 4)
        $s.Close()
        return ($n -eq 4) -and (@("LIVE", "PIRS", "CON ") -contains [System.Text.Encoding]::ASCII.GetString($b))
    } catch { return $false }
}
# Title updates and DLC are often downloaded as .zip (or .rar / .7z) files.
function IsArchive($path) { @(".zip", ".rar", ".7z") -contains [System.IO.Path]::GetExtension($path).ToLower() }
function Expand-ToFolder($archive, $dest) {
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $dest
    New-Item -ItemType Directory -Force -Path $dest | Out-Null
    if ([System.IO.Path]::GetExtension($archive).ToLower() -eq ".zip") {
        Expand-Archive -LiteralPath $archive -DestinationPath $dest -Force
        return
    }
    $7z = (Get-Command 7z -ErrorAction SilentlyContinue).Source
    if (-not $7z) { $7z = Join-Path $env:ProgramFiles "7-Zip\7z.exe" }
    if (-not (Test-Path $7z)) { Fail "$archive needs 7-Zip (https://www.7-zip.org) to unpack. Install it, or unpack the file yourself." }
    & $7z x -y "-o$dest" $archive | Out-Null
    if ($LASTEXITCODE -ne 0) { Fail "Could not unpack $archive" }
}

# Parallel compile jobs: one per logical CPU, but at most one per ~2 GB of RAM.
# The recompiled game files are huge; on an 8 GB laptop one job per thread
# runs out of memory (the build then crawls or clang crashes).
$LogicalCpus = [Environment]::ProcessorCount
$RamGB = [math]::Floor((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory / 1GB)
$BuildJobs = [math]::Max(1, [math]::Min($LogicalCpus, [math]::Floor($RamGB / 2)))
Write-Host "Build: $BuildJobs parallel jobs ($LogicalCpus CPU threads, $RamGB GB RAM)"

# ---------------------------------------------------------------------------
# 1. Toolchain: Visual Studio 2022 (with its Clang, CMake and Ninja) + Git
# ---------------------------------------------------------------------------
Step "Checking build tools"
if (-not (Get-Command git -ErrorAction SilentlyContinue)) {
    Fail "Git was not found. Install Git for Windows (https://git-scm.com/download/win) and run setup again."
}
# Git refuses folders it cannot prove belong to this Windows user ("detected
# dubious ownership": exFAT / FAT32 / network drives, folders made by another
# account). Setup only runs git in its own folders, so trust them all here.
$env:GIT_CONFIG_COUNT = "1"
$env:GIT_CONFIG_KEY_0 = "safe.directory"
$env:GIT_CONFIG_VALUE_0 = "*"
$vswhere = Join-Path ${env:ProgramFiles(x86)} "Microsoft Visual Studio\Installer\vswhere.exe"
if (-not (Test-Path $vswhere)) {
    Fail "Visual Studio 2022 was not found. See README.md (Requirements) for what to install."
}
$vs = & $vswhere -latest -products * -version "[17.0,)" `
    -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 Microsoft.VisualStudio.Component.VC.Llvm.Clang `
    -property installationPath
if (-not $vs) {
    Fail ("Visual Studio 2022 is installed but is missing components. In the Visual Studio Installer, " +
          "select 'Desktop development with C++' and tick 'C++ Clang Compiler for Windows'.")
}
# Load the x64 developer environment (compiler, Windows SDK, CMake, Ninja).
$vcvars = Join-Path $vs "VC\Auxiliary\Build\vcvars64.bat"
cmd /c "`"$vcvars`" >nul 2>&1 && set" | ForEach-Object {
    if ($_ -match '^([^=]+)=(.*)$') { Set-Item -Path "env:$($matches[1])" -Value $matches[2] }
}
$env:PATH = (Join-Path $vs "VC\Tools\Llvm\x64\bin") + ";" + $env:PATH
foreach ($tool in "clang", "clang++", "cmake", "ninja") {
    if (-not (Get-Command $tool -ErrorAction SilentlyContinue)) {
        Fail "'$tool' was not found even after loading the Visual Studio environment. See README.md."
    }
}
Write-Host "Visual Studio: $vs"

# ---------------------------------------------------------------------------
# 2. Extract the game from your disc image
# ---------------------------------------------------------------------------
if (-not (Test-Path (Join-Path $GameDir "default.xex"))) {
    if (-not $Iso) {
        Write-Host "`nSelect your Fight Night Champion (Xbox 360) disc image (.iso)." -ForegroundColor Yellow
        try {
            Add-Type -AssemblyName System.Windows.Forms
            $dlg = New-Object System.Windows.Forms.OpenFileDialog
            $dlg.Filter = "Xbox 360 disc image (*.iso)|*.iso|All files (*.*)|*.*"
            $dlg.Title = "Select your Fight Night Champion disc image"
            if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $Iso = $dlg.FileName }
        } catch { }
        if (-not $Iso) { $Iso = (Read-Host "Path to your Fight Night Champion .iso").Trim('"') }
    }
    if (-not (Test-Path $Iso)) { Fail "Disc image not found: $Iso" }

    Step "Building the disc image extractor"
    $extractor = Join-Path $Build "xiso_extract.exe"
    Run "clang++" @("-std=c++17", "-O2", "-o", $extractor, (Join-Path $Root "tools\xiso_extract\xiso_extract.cpp"))

    Step "Extracting the game from $Iso (about 6 GB)"
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $GameDir
    Run $extractor @($Iso, $GameDir,
        "--skip-name", "`$SystemUpdate", "--skip-name", "layer1filler.bin", "--skip-name", "layer2filler.bin")
    if (-not (Test-Path (Join-Path $GameDir "default.xex"))) {
        Fail "The disc image does not contain default.xex - is this an Xbox 360 game disc?"
    }
} else {
    Write-Host "Game files already extracted to dist\game - skipping extraction."
}

# The hooks in this project are tied to one specific build of the game.
$knownXex = "498796AF783F0D598BBDA166B6BC8D518EDF8428"   # Fight Night Champion (USA, Europe), disc version
$xexHash = (Get-FileHash -Algorithm SHA1 (Join-Path $GameDir "default.xex")).Hash
if ($xexHash -ne $knownXex) {
    Write-Host ("WARNING: your default.xex (SHA-1 $xexHash) is not the version this port was made " +
                "for. The build may fail or the game may not run correctly.") -ForegroundColor Yellow
}

# ---------------------------------------------------------------------------
# 3. ReXGlue SDK (runtime + recompiler), patched for Fight Night Champion
# ---------------------------------------------------------------------------
$sdkKey = "$SdkCommit " + (FileHash (Join-Path $Root "patches\rexglue-sdk.patch"))
if (-not (Done "sdk" $sdkKey)) {
    if (-not (Test-Path (Join-Path $SdkSrc ".git"))) {
        Step "Downloading the ReXGlue SDK"
        # autocrlf off so patches/rexglue-sdk.patch (LF) applies cleanly.
        Run "git" @("clone", "-c", "core.autocrlf=false", $SdkRepo, $SdkSrc)
    }
    Push-Location $SdkSrc
    try {
        Step "Checking out ReXGlue SDK $($SdkCommit.Substring(0, 7)) and its dependencies"
        Run "git" @("-c", "advice.detachedHead=false", "checkout", "--force", $SdkCommit)
        # Files an older version of the patch added (checkout --force leaves
        # them), or the new patch can't create them again.
        & git clean -fdq -- include src resources cmake
        # Shallow submodules save several GB; fall back to full history if a
        # server refuses to serve a pinned commit shallowly.
        & git submodule update --init --recursive --force --depth 1
        if ($LASTEXITCODE -ne 0) {
            Run "git" @("submodule", "update", "--init", "--recursive", "--force")
        }

        # Git on Windows checks libmspack's symlinked sources out as small text
        # files containing the link target; replace them with the real files.
        $mspack = Join-Path $SdkSrc "thirdparty\libmspack\cabextract\mspack"
        if (Test-Path $mspack) {
            Get-ChildItem -Path $mspack -File | Where-Object { $_.Length -lt 300 } | ForEach-Object {
                $text = (Get-Content -Raw $_.FullName).Trim()
                if ($text -match '^(\.\./)+[\w./\\-]+$') {
                    $target = [System.IO.Path]::GetFullPath((Join-Path $_.DirectoryName $text))
                    if (Test-Path $target) { Copy-Item -Force $target $_.FullName }
                }
            }
        }

        Step "Applying the Fight Night Champion patch to the SDK"
        $patch = Join-Path $Root "patches\rexglue-sdk.patch"
        & git apply --check $patch 2>$null
        if ($LASTEXITCODE -eq 0) {
            Run "git" @("apply", "--whitespace=nowarn", $patch)
        } else {
            & git apply --reverse --check $patch 2>$null
            if ($LASTEXITCODE -ne 0) { Fail "The SDK patch does not apply. Try deleting the build folder and running setup again." }
            Write-Host "Patch already applied."
        }

        Step "Building the ReXGlue SDK (this takes a while)"
        Run "cmake" @("--preset", "win-amd64", "-DCMAKE_INSTALL_PREFIX=$SdkInstall")
        Run "cmake" @("--build", "out/build/win-amd64", "--target", "install", "--config", "Release", "-j", "$BuildJobs")
    } finally {
        Pop-Location
    }
    MarkDone "sdk" $sdkKey
} else {
    Write-Host "ReXGlue SDK already built - skipping."
}

# ---------------------------------------------------------------------------
# 3b. fnc_content: reads your title update and DLC packages (uses the SDK)
# ---------------------------------------------------------------------------
$env:REXSDK = $SdkInstall
# fnc_content.exe needs the SDK's runtime DLLs.
$env:PATH = (Join-Path $SdkInstall "bin") + ";" + $env:PATH
$toolKey = "$sdkKey " + (FileHash (Join-Path $Root "tools\fnc_content\fnc_content.cpp"))
if (-not ((Done "fnc_content" $toolKey) -and (Test-Path $ContentTool))) {
    Step "Building the content installer (fnc_content)"
    Run "cmake" @("-S", (Join-Path $Root "tools\fnc_content"), "-B", $ToolBuild, "-G", "Ninja",
        "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++")
    Run "cmake" @("--build", $ToolBuild)
    MarkDone "fnc_content" $toolKey
}

# ---------------------------------------------------------------------------
# 3c. Title update 2 (default.xexp next to default.xex; applied when recompiling)
# ---------------------------------------------------------------------------
$xexp = Join-Path $GameDir "default.xexp"
if (-not ((Test-Path $xexp) -and ((FileHash $xexp) -eq $KnownXexp))) {
    if (-not $TitleUpdate) {
        Write-Host ("`nSelect your Fight Night Champion title update 2 package (a file named like " +
                    "TU_12K228L_000000C000000.00000000000G3) or a default.xexp extracted from it.") -ForegroundColor Yellow
        try {
            Add-Type -AssemblyName System.Windows.Forms
            $dlg = New-Object System.Windows.Forms.OpenFileDialog
            $dlg.Filter = "Title update package, default.xexp or an archive with either|*.*"
            $dlg.Title = "Select your Fight Night Champion title update 2"
            if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $TitleUpdate = $dlg.FileName }
        } catch { }
        if (-not $TitleUpdate) { $TitleUpdate = (Read-Host "Path to your title update 2 package or default.xexp").Trim('"') }
    }
    if (-not (Test-Path $TitleUpdate -PathType Leaf)) { Fail "Title update not found: $TitleUpdate" }

    Step "Installing title update 2"
    if (IsArchive $TitleUpdate) {
        # A .zip (or .rar / .7z) with the package in it, as downloaded.
        $unpacked = Join-Path $Build "tu_archive"
        Expand-ToFolder $TitleUpdate $unpacked
        $found = Get-ChildItem -Path $unpacked -Recurse -File | Where-Object {
            $_.Name -ieq "default.xexp" -or (IsPackage $_.FullName)
        } | Select-Object -First 1
        if (-not $found) { Fail "No title update package or default.xexp found in $TitleUpdate" }
        $TitleUpdate = $found.FullName
    }
    if ((Split-Path -Leaf $TitleUpdate) -ieq "default.xexp") {
        Copy-Item -Force $TitleUpdate $xexp
    } else {
        Run $ContentTool @("extract-file", $TitleUpdate, "default.xexp", $xexp)
    }
    if ((FileHash $xexp) -ne $KnownXexp) {
        Remove-Item -Force $xexp
        Fail ("This is not title update 2 for Fight Night Champion (default.xexp SHA-1 differs). " +
              "Title update 1 or other versions are not supported.")
    }
} else {
    Write-Host "Title update 2 already installed - skipping."
}

# ---------------------------------------------------------------------------
# 4. Recompile the game executable (PowerPC -> C++)
# ---------------------------------------------------------------------------
$codegenKey = "$sdkKey " + (FileHash $Manifest) + " " + (FileHash $xexp)
if (-not (Done "codegen" $codegenKey)) {
    Step "Recompiling default.xex (PowerPC -> C++)"
    $rexglue = Join-Path $SdkInstall "bin\rexglue.exe"
    Run $rexglue @("codegen", $Manifest, "--ignore-stamp")
    MarkDone "codegen" $codegenKey
} else {
    Write-Host "Recompiled code already generated - skipping."
}

# ---------------------------------------------------------------------------
# 5. Build FNCReborn.exe
# ---------------------------------------------------------------------------
Step "Building FNC Reborn (compiles ~70,000 functions; expect 15-60 minutes)"
$env:REXSDK = $SdkInstall
Run "cmake" @("-S", (Join-Path $Root "project"), "-B", $GameBuild, "-G", "Ninja",
    "-DCMAKE_BUILD_TYPE=Release", "-DCMAKE_C_COMPILER=clang", "-DCMAKE_CXX_COMPILER=clang++",
    "-DFNC_GENERATED_DIR=$(Join-Path $Build 'generated')")
Run "cmake" @("--build", $GameBuild, "-j", "$BuildJobs")

Step "Copying the game to dist"
try {
    Copy-Item -Force -ErrorAction Stop (Join-Path $GameBuild "FNCReborn.exe") $Dist
    Copy-Item -Force -ErrorAction Stop (Join-Path $SdkInstall "bin\*.dll") $Dist
    $config = Join-Path $Dist "FNCReborn.toml"
    if (-not (Test-Path $config)) {
        Copy-Item -ErrorAction Stop (Join-Path $Root "config\FNCReborn.toml") $config
    }
} catch {
    Fail "Could not copy the game to dist ($($_.Exception.Message)). If FNC Reborn is running, close it and run setup again."
}

# ---------------------------------------------------------------------------
# 6. Saves: start from your FNC Reborn (disc version) saves, if you have any
# ---------------------------------------------------------------------------
if (-not (Test-Path $UserData) -and (Test-Path $OldUserData)) {
    Step "Copying your FNC Reborn saves to Documents\FNCReborn-TU2"
    Copy-Item -Recurse -Force $OldUserData $UserData
    Write-Host "The original saves in Documents\FNCReborn are left as they are."
}
New-Item -ItemType Directory -Force -Path $UserData | Out-Null
$ContentDir = Join-Path $UserData "0000000000000000\$TitleId\00000002"
$HeaderDir = Join-Path $UserData "0000000000000000\$TitleId\Headers\00000002"

# ---------------------------------------------------------------------------
# 7. Your DLC: Xbox 360 packages (LIVE/PIRS/CON files) or folders Xenia installed
# ---------------------------------------------------------------------------
function Install-ExtractedDlc($dir) {
    # A package Xenia (or an earlier FNC Reborn) installed: <name>\ with the package's files,
    # and usually ..\Headers\00000002\<name>.header next to it.
    $name = Split-Path -Leaf $dir
    $dest = Join-Path $ContentDir $name
    if ((Resolve-Path $dir).Path -eq $dest) { return }
    Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $dest
    Copy-Item -Recurse -Force $dir $dest
    New-Item -ItemType Directory -Force -Path $HeaderDir | Out-Null
    # Header: XCONTENT_AGGREGATE_DATA (0x148 bytes, big-endian) + license mask (all licenses).
    $out = New-Object byte[] 0x14C
    $header = Join-Path (Split-Path -Parent (Split-Path -Parent $dir)) "Headers\00000002\$name.header"
    if (Test-Path $header) {
        $bytes = [System.IO.File]::ReadAllBytes($header)
        [Array]::Copy($bytes, $out, [Math]::Min($bytes.Length, 0x148))
    } else {
        # No header to copy: build one. Display name from the package's dimecontentinfo.xml.
        $display = $name
        $xml = Get-ChildItem -Path $dir -Recurse -Filter "dimecontentinfo.xml" -ErrorAction SilentlyContinue | Select-Object -First 1
        if ($xml -and ((Get-Content -Raw $xml.FullName) -match '<name>([^<]+)</name>')) { $display = $matches[1] }
        $out[3] = 1                                                   # device id: hard drive
        $out[7] = 2                                                   # content type: DLC
        $nameBytes = [System.Text.Encoding]::BigEndianUnicode.GetBytes($display)
        [Array]::Copy($nameBytes, 0, $out, 8, [Math]::Min($nameBytes.Length, 254))
        $fileBytes = [System.Text.Encoding]::ASCII.GetBytes($name)
        [Array]::Copy($fileBytes, 0, $out, 0x108, [Math]::Min($fileBytes.Length, 42))
        $tid = [Convert]::ToUInt32($TitleId, 16)
        $out[0x140] = ($tid -shr 24) -band 0xFF; $out[0x141] = ($tid -shr 16) -band 0xFF
        $out[0x142] = ($tid -shr 8) -band 0xFF;  $out[0x143] = $tid -band 0xFF
    }
    for ($i = 0x148; $i -lt 0x14C; $i++) { $out[$i] = 0xFF }
    [System.IO.File]::WriteAllBytes((Join-Path $HeaderDir "$name.header"), $out)
    Write-Host "installed $name (extracted folder)"
}

$installedDlc = @(Get-ChildItem -Path $ContentDir -Directory -ErrorAction SilentlyContinue).Count

if (-not $NoDlc) {
    if (-not $Dlc -and $installedDlc -gt 0) {
        Write-Host "$installedDlc DLC package(s) already installed - skipping (run setup with -Dlc <folder> to add more)."
    } elseif (-not $Dlc) {
        Write-Host ("`nSelect the folder with your Fight Night Champion DLC (Xbox 360 packages, or a Xenia " +
                    "content folder). Cancel to skip; you can run setup again later.") -ForegroundColor Yellow
        try {
            Add-Type -AssemblyName System.Windows.Forms
            $dlg = New-Object System.Windows.Forms.FolderBrowserDialog
            $dlg.Description = "Folder with your Fight Night Champion DLC (cancel to skip)"
            if ($dlg.ShowDialog() -eq [System.Windows.Forms.DialogResult]::OK) { $Dlc = $dlg.SelectedPath }
        } catch {
            $Dlc = (Read-Host "Folder with your DLC (leave empty to skip)").Trim('"')
        }
    }
    if ($Dlc) {
        if (-not (Test-Path $Dlc -PathType Container)) { Fail "DLC folder not found: $Dlc" }
        Step "Installing your DLC from $Dlc"
        New-Item -ItemType Directory -Force -Path $ContentDir | Out-Null
        $count = 0
        # Unpack downloaded archives (.zip, and .rar / .7z with 7-Zip) next to the build first.
        $unpackRoot = Join-Path $Build "dlc_archives"
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $unpackRoot
        $searchDirs = @($Dlc)
        $archives = @(Get-ChildItem -Path $Dlc -Recurse -File -ErrorAction SilentlyContinue | Where-Object { IsArchive $_.FullName })
        for ($i = 0; $i -lt $archives.Count; $i++) {
            Write-Host "unpacking $($archives[$i].Name)"
            Expand-ToFolder $archives[$i].FullName (Join-Path $unpackRoot $i)
        }
        if ($archives.Count) { $searchDirs += $unpackRoot }
        $seen = @{}
        foreach ($f in Get-ChildItem -Path $searchDirs -Recurse -File -ErrorAction SilentlyContinue) {
            if ($f.Length -lt 0xA000 -or $seen.ContainsKey($f.Name)) { continue }
            if (-not (IsPackage $f.FullName)) { continue }
            $info = (& $ContentTool info $f.FullName) -split "`t"
            if ($info.Count -lt 4 -or $info[1] -ne $TitleId) { continue }
            if ($info[2] -ne "00000002") {
                Write-Host "skipping $($f.Name): $($info[3]) (not DLC)"
                continue
            }
            Run $ContentTool @("install", $f.FullName, $UserData)
            $seen[$f.Name] = $true
            $count++
        }
        # Folders Xenia installed: 42-character package names containing the package's files.
        foreach ($d in Get-ChildItem -Path $searchDirs -Recurse -Directory -ErrorAction SilentlyContinue) {
            if ($d.Name -match '^[0-9A-F]{42}$' -and -not $seen.ContainsKey($d.Name) -and
                ((Test-Path (Join-Path $d.FullName "dlc.toc")) -or (Test-Path (Join-Path $d.FullName "info.dlc")))) {
                Install-ExtractedDlc $d.FullName
                $seen[$d.Name] = $true
                $count++
            }
        }
        Remove-Item -Recurse -Force -ErrorAction SilentlyContinue $unpackRoot
        Write-Host "$count DLC package(s) installed."
    }
}

# ---------------------------------------------------------------------------
# 8. Legends Pack commentary: the game streams it from a decompressed copy
# ---------------------------------------------------------------------------
$legendsDir = Join-Path $ContentDir $LegendsPack
$legendsBig = Join-Path $legendsDir "dlc\data\xenon\dlc_main.big"
$legendsOrig = "$legendsBig.orig"   # the pack's own archive, if the Settings tabs were added to it
$speechDir = Join-Path $GameDir "datadlc\dlc_pack1\dlc\data\xenon\loc\eng\speech"
if (Test-Path $legendsBig) {
    $source = if (Test-Path $legendsOrig) { $legendsOrig } else { $legendsBig }
    $speechKey = (FileHash $source)
    if (-not ((Done "legends_speech" $speechKey) -and (Test-Path (Join-Path $speechDir "speech_eng_dlc_samples.sbs")))) {
        Step "Unpacking the Legends Pack commentary"
        New-Item -ItemType Directory -Force -Path $speechDir | Out-Null
        foreach ($ext in "sbr", "sbs") {
            $packed = Join-Path $Build "speech_eng_dlc_samples.$ext.lzx"
            Run $ContentTool @("big-extract", $source, "speech_eng_dlc_samples.$ext", $packed)
            Run $ContentTool @("unlzx", $packed, (Join-Path $speechDir "speech_eng_dlc_samples.$ext"))
            Remove-Item -Force $packed
        }
        MarkDone "legends_speech" $speechKey
    }
}

# ---------------------------------------------------------------------------
# 9. Settings > DISPLAY and EXTRAS tabs (patches your own fe.big and, when the
#    Legends Pack is installed, its copy of the Settings screen; needs Python 3)
# ---------------------------------------------------------------------------
$feBig = Join-Path $GameDir "databigs\xenon\fe.big"
$tabScript = Join-Path $Root "tools\display_tab\aptpatch_display.py"
$targets = @(@{ Name = "display_tab"; Args = @($feBig) })
if (Test-Path $legendsBig) {
    $targets += @{ Name = "display_tab_legends"; Args = @($legendsBig, "--toc", (Join-Path $legendsDir "dlc.toc")) }
}
$python = $null
foreach ($candidate in "py", "python", "python3") {
    $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
    # Skip the Microsoft Store placeholder, which only opens the Store.
    if ($cmd -and $cmd.Source -notmatch "WindowsApps") { $python = $cmd.Source; break }
}
# Keyed on the original file and the current one, so a fresh copy (e.g. the DLC installed again)
# is patched again.
function TabKey($file) {
    $orig = "$file.orig"
    (FileHash $tabScript) + " " + (FileHash $(if (Test-Path $orig) { $orig } else { $file })) + " " + (FileHash $file)
}
foreach ($t in $targets) {
    $file = $t.Args[0]
    if (Done $t.Name (TabKey $file)) { continue }
    Step "Adding the DISPLAY and EXTRAS tabs to the Settings screen ($(Split-Path -Leaf $file))"
    if (-not $python) {
        Write-Host ("Python 3 was not found, so the DISPLAY and EXTRAS tabs were not added. The game works " +
                    "without them; change the same settings in dist\FNCReborn.toml, or install Python and " +
                    "run setup again.") -ForegroundColor Yellow
        break
    }
    $pyArgs = @($tabScript) + $t.Args
    if ((Split-Path -Leaf $python) -eq "py.exe") { $pyArgs = @("-3") + $pyArgs }
    & $python @pyArgs
    if ($LASTEXITCODE -eq 0) {
        MarkDone $t.Name (TabKey $file)
    } else {
        Write-Host "Warning: the tabs could not be added to $file. The game works without them." -ForegroundColor Yellow
    }
}

Stop-Transcript | Out-Null
Write-Host "`nDone! Run dist\FNCReborn.exe to play." -ForegroundColor Green
Write-Host "F11 toggles fullscreen, F1 shows the frame rate, Back (or F2) switches the camera. See README.md for options."
Write-Host "Saves and DLC are in Documents\FNCReborn-TU2. Run setup again with -Dlc <folder> to add more DLC."
if (-not $NoPause) { Read-Host "Press Enter to close" | Out-Null }
