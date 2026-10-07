# FNC Reborn - build script.
#
# Builds a native Windows version of Fight Night Champion (Xbox 360, 2011) from
# YOUR OWN copy of the game. Nothing from the game is included in this
# repository; the script extracts your disc image, recompiles the game's
# executable locally and places the result in .\dist.
#
# Usage (normally via setup.bat):
#   powershell -ExecutionPolicy Bypass -File scripts\setup.ps1 [-Iso <path>] [-Clean]

param(
    [string]$Iso = "",
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
# 4. Recompile the game executable (PowerPC -> C++)
# ---------------------------------------------------------------------------
$codegenKey = "$sdkKey " + (FileHash $Manifest)
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
# 6. Settings > DISPLAY tab (patches your own fe.big; needs Python 3)
# ---------------------------------------------------------------------------
$feBig = Join-Path $GameDir "databigs\xenon\fe.big"
$tabScript = Join-Path $Root "tools\display_tab\aptpatch_display.py"
$tabKey = (FileHash $tabScript) + " " + (FileHash (Join-Path $GameDir "default.xex"))
if (-not (Done "display_tab" $tabKey)) {
    Step "Adding the DISPLAY tab to the game's Settings screen"
    $python = $null
    foreach ($candidate in "py", "python", "python3") {
        $cmd = Get-Command $candidate -ErrorAction SilentlyContinue
        # Skip the Microsoft Store placeholder, which only opens the Store.
        if ($cmd -and $cmd.Source -notmatch "WindowsApps") { $python = $cmd.Source; break }
    }
    if (-not $python) {
        Write-Host ("Python 3 was not found, so the DISPLAY tab was not added. The game works without it; " +
                    "change the same settings in dist\FNCReborn.toml, or install Python and run setup again.") -ForegroundColor Yellow
    } else {
        $pyArgs = @($tabScript, $feBig)
        if ((Split-Path -Leaf $python) -eq "py.exe") { $pyArgs = @("-3") + $pyArgs }
        & $python @pyArgs
        if ($LASTEXITCODE -eq 0) {
            MarkDone "display_tab" $tabKey
        } else {
            Write-Host "Warning: the DISPLAY tab could not be added. The game works without it." -ForegroundColor Yellow
        }
    }
} else {
    Write-Host "DISPLAY tab already added - skipping."
}

Stop-Transcript | Out-Null
Write-Host "`nDone! Run dist\FNCReborn.exe to play." -ForegroundColor Green
Write-Host "F11 toggles fullscreen, F1 shows the frame rate, Back (or F2) switches the camera. See README.md for options."
if (-not $NoPause) { Read-Host "Press Enter to close" | Out-Null }
