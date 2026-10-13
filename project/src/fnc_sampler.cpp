// Sampling profiler and crash reporter.
// Samples the busiest game threads ~1000x/s, unwinds each stack, and every 10 s
// logs [FNC-PROF] lines: which recompiled game function each thread is in, and
// when it is inside a DLL (runtime, kernel), which game function called it.
// The profiler only runs when a file named "profile_on" exists next to FNCReborn.exe.
// Crash reports (with the game function that crashed) go to FNCReborn_crash.txt.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <rex/logging.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#include <tlhelp32.h>
#pragma comment(lib, "winmm.lib")
extern "C" __declspec(dllimport) UINT WINAPI timeBeginPeriod(UINT uPeriod);
#endif

namespace fnc_prof {

#ifdef _WIN32
namespace {

struct Mapping {  // layout of PPCFuncMapping
  size_t guest;
  void* host;
};

struct HostFn {
  uintptr_t host;
  uint32_t guest;
};

std::vector<HostFn> g_fns;  // sorted by host address, exe-only
uintptr_t g_exe_lo = 0, g_exe_hi = 0;

uint32_t GuestOf(uintptr_t rip) {
  if (rip < g_exe_lo || rip >= g_exe_hi || g_fns.empty()) return 0;
  auto it = std::upper_bound(g_fns.begin(), g_fns.end(), rip,
                             [](uintptr_t v, const HostFn& f) { return v < f.host; });
  if (it == g_fns.begin()) return 0;
  // Past the last game function is our own host code (sampler, overlay...).
  if (it == g_fns.end() && rip - std::prev(it)->host > 0x4000) return 0;
  return std::prev(it)->guest;
}

std::string ModuleOfUncached(uintptr_t addr);
std::string ModuleOf(uintptr_t addr) {
  static std::unordered_map<uintptr_t, std::string> cache;
  auto it = cache.find(addr);
  if (it != cache.end()) return it->second;
  if (cache.size() > 200000) cache.clear();
  return cache[addr] = ModuleOfUncached(addr);
}
std::string ModuleOfUncached(uintptr_t addr) {
  HMODULE m = nullptr;
  if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                              GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(addr), &m) || !m) {
    return "?";
  }
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(m, buf, MAX_PATH);
  std::wstring w(buf, n);
  auto slash = w.find_last_of(L"\\/");
  if (slash != std::wstring::npos) w = w.substr(slash + 1);
  std::string s;
  for (wchar_t c : w) s.push_back(c < 128 ? char(c) : '?');
  // Nearest exported symbol at or below addr (names ntdll syscall stubs etc).
  auto base = reinterpret_cast<const uint8_t*>(m);
  uint32_t rva = uint32_t(addr - reinterpret_cast<uintptr_t>(m));
  auto dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
  auto nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
  const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
  const char* best_name = nullptr;
  uint32_t best_rva = 0;
  if (dir.VirtualAddress && dir.Size) {
    auto exp = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dir.VirtualAddress);
    auto funcs = reinterpret_cast<const uint32_t*>(base + exp->AddressOfFunctions);
    auto names = reinterpret_cast<const uint32_t*>(base + exp->AddressOfNames);
    auto ords = reinterpret_cast<const uint16_t*>(base + exp->AddressOfNameOrdinals);
    for (uint32_t i = 0; i < exp->NumberOfNames; ++i) {
      uint32_t f = funcs[ords[i]];
      if (f <= rva && f > best_rva) {
        best_rva = f;
        best_name = reinterpret_cast<const char*>(base + names[i]);
      }
    }
  }
  char off[160];
  if (best_name) {
    std::snprintf(off, sizeof(off), "!%.100s+0x%X", best_name, rva - best_rva);
  } else {
    std::snprintf(off, sizeof(off), "+0x%X", rva);
  }
  return s + off;
}

typedef HRESULT(WINAPI* GetThreadDescriptionFn)(HANDLE, PWSTR*);

std::string ThreadName(HANDLE h) {
  static auto fn = reinterpret_cast<GetThreadDescriptionFn>(
      GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "GetThreadDescription"));
  std::string out;
  PWSTR w = nullptr;
  if (fn && SUCCEEDED(fn(h, &w)) && w) {
    for (const wchar_t* p = w; *p; ++p) out.push_back(*p < 128 ? char(*p) : '?');
    LocalFree(w);
  }
  return out;
}

bool WantThread(const std::string& name) {
  return name.rfind("FGGameSim", 0) == 0 || name.rfind("FGGameRender", 0) == 0 ||
         name.rfind("Job Thread 0", 0) == 0;
}

struct Target {
  HANDLE h;
  std::string name;
  std::unordered_map<std::string, uint32_t> hist;
  std::unordered_map<uint32_t, uint32_t> incl;  // [FNC-PROFI] inclusive: any guest fn on the stack
  uint32_t samples = 0;
};

constexpr int kMaxFrames = 48;

// Walks the suspended thread's stack. No heap allocation in here.
int Unwind(CONTEXT ctx, uintptr_t* out) {
  int n = 0;
  for (int i = 0; i < kMaxFrames; ++i) {
    uintptr_t rip = ctx.Rip;
    if (!rip) break;
    out[n++] = rip;
    DWORD64 image_base = 0;
    PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(rip, &image_base, nullptr);
    if (!fe) {
      if (!ctx.Rsp) break;
      ctx.Rip = *reinterpret_cast<DWORD64*>(ctx.Rsp);
      ctx.Rsp += 8;
    } else {
      PVOID handler_data = nullptr;
      DWORD64 establisher = 0;
      RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, rip, fe, &ctx, &handler_data,
                       &establisher, nullptr);
    }
  }
  return n;
}

// Stack walk under an SEH guard: a thread caught mid-prologue (or with a
// half-built frame) can make RtlVirtualUnwind read garbage. That crashed the
// game once; now the sample is just dropped.
int SafeUnwind(const CONTEXT& ctx, uintptr_t* out) {
  __try {
    return Unwind(ctx, out);
  } __except (EXCEPTION_EXECUTE_HANDLER) {
    return 0;
  }
}

std::string Describe(const uintptr_t* frames, int n) {
  // innermost guest function, the module we were in if not the exe, and the
  // guest caller of that function.
  int gi = -1;
  for (int i = 0; i < n; ++i) {
    if (GuestOf(frames[i])) { gi = i; break; }
  }
  char buf[160];
  if (gi < 0) {
    return n ? "[no game frame] " + ModuleOf(frames[0]) : "[empty]";
  }
  uint32_t g = GuestOf(frames[gi]);
  uint32_t caller = 0;
  for (int i = gi + 1; i < n; ++i) {
    uint32_t c = GuestOf(frames[i]);
    if (c && c != g) { caller = c; break; }
  }
  std::string where = gi == 0 ? "" : " in " + ModuleOf(frames[0]);
  std::snprintf(buf, sizeof(buf), "sub_%08X%s <- sub_%08X", g, where.c_str(), caller);
  return buf;
}

void BuildTable(const Mapping* maps) {
  // Build the host->guest table for functions living in FNCReborn.exe.
  MODULEINFO mi{};
  GetModuleInformation(GetCurrentProcess(), GetModuleHandleW(nullptr), &mi, sizeof(mi));
  g_exe_lo = reinterpret_cast<uintptr_t>(mi.lpBaseOfDll);
  g_exe_hi = g_exe_lo + mi.SizeOfImage;
  for (const Mapping* m = maps; m && m->host; ++m) {
    uintptr_t h = reinterpret_cast<uintptr_t>(m->host);
    if (h >= g_exe_lo && h < g_exe_hi) g_fns.push_back({h, uint32_t(m->guest)});
  }
  std::sort(g_fns.begin(), g_fns.end(),
            [](const HostFn& a, const HostFn& b) { return a.host < b.host; });
}

std::wstring g_crash_path;

// Crash report: written straight to fnc_crash.txt (no logger), then the normal
// crash handling continues.
LONG WINAPI CrashFilter(EXCEPTION_POINTERS* ep) {
  static std::atomic<bool> once{false};
  if (once.exchange(true)) return EXCEPTION_CONTINUE_SEARCH;
  FILE* f = _wfopen(g_crash_path.c_str(), L"w");
  if (!f) return EXCEPTION_CONTINUE_SEARCH;
  auto* er = ep->ExceptionRecord;
  uintptr_t pc = reinterpret_cast<uintptr_t>(er->ExceptionAddress);
  std::fprintf(f, "Fight Night crash report\n");
  std::fprintf(f, "exception 0x%08lX at %s", er->ExceptionCode, ModuleOf(pc).c_str());
  if (uint32_t g = GuestOf(pc)) std::fprintf(f, "  (game function sub_%08X)", g);
  std::fprintf(f, "\nthread %lu\n", GetCurrentThreadId());
  if (er->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && er->NumberParameters >= 2) {
    std::fprintf(f, "access violation: %s address 0x%llX\n",
                 er->ExceptionInformation[0] == 0   ? "read"
                 : er->ExceptionInformation[0] == 1 ? "write"
                                                    : "execute",
                 (unsigned long long)er->ExceptionInformation[1]);
  }
  auto* c = ep->ContextRecord;
  std::fprintf(f, "rax=%llX rbx=%llX rcx=%llX rdx=%llX rsi=%llX rdi=%llX\n", c->Rax, c->Rbx, c->Rcx,
               c->Rdx, c->Rsi, c->Rdi);
  std::fprintf(f, "r8=%llX r9=%llX r10=%llX r11=%llX rsp=%llX rbp=%llX\n", c->R8, c->R9, c->R10,
               c->R11, c->Rsp, c->Rbp);
  std::fprintf(f, "\ncall stack (innermost first):\n");
  uintptr_t frames[kMaxFrames];
  int n = Unwind(*c, frames);
  for (int i = 0; i < n; ++i) {
    uint32_t g = GuestOf(frames[i]);
    if (g) {
      std::fprintf(f, "  %2d  sub_%08X\n", i, g);
    } else {
      std::fprintf(f, "  %2d  %s\n", i, ModuleOf(frames[i]).c_str());
    }
  }
  std::fclose(f);
  return EXCEPTION_CONTINUE_SEARCH;
}

// REX_FATAL (e.g. a call to an unregistered function) ends with abort(), which is not an
// exception: report it the same way, from the aborting thread's own stack.
void AbortHandler(int) {
  CONTEXT ctx{};
  RtlCaptureContext(&ctx);
  EXCEPTION_RECORD er{};
  er.ExceptionCode = 0xE0000ABE;  // marker: abort()
  er.ExceptionAddress = reinterpret_cast<void*>(ctx.Rip);
  EXCEPTION_POINTERS ep{&er, &ctx};
  CrashFilter(&ep);
}

void SamplerMain() {
  REXLOG_INFO("[FNC-PROF] sampler started, {} game functions indexed", g_fns.size());

  timeBeginPeriod(1);
  std::vector<Target> targets;
  const DWORD pid = GetCurrentProcessId();
  const DWORD self = GetCurrentThreadId();
  auto last_scan = std::chrono::steady_clock::time_point{};
  auto last_report = std::chrono::steady_clock::now();
  uintptr_t frames[kMaxFrames];

  for (;;) {
    auto now = std::chrono::steady_clock::now();
    if (now - last_scan > std::chrono::seconds(3)) {
      last_scan = now;
      HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
      if (snap != INVALID_HANDLE_VALUE) {
        THREADENTRY32 te{};
        te.dwSize = sizeof(te);
        if (Thread32First(snap, &te)) {
          do {
            if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
            HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT |
                                      THREAD_QUERY_LIMITED_INFORMATION,
                                  FALSE, te.th32ThreadID);
            if (!h) continue;
            std::string name = ThreadName(h);
            bool known = false;
            for (auto& t : targets) {
              if (t.name == name) { known = true; break; }
            }
            if (!known && WantThread(name)) {
              targets.push_back({h, name, {}, {}, 0});
            } else {
              CloseHandle(h);
            }
          } while (Thread32Next(snap, &te));
        }
        CloseHandle(snap);
      }
    }

    for (auto& t : targets) {
      if (SuspendThread(t.h) == DWORD(-1)) continue;
      CONTEXT ctx{};
      ctx.ContextFlags = CONTEXT_CONTROL | CONTEXT_INTEGER;
      int n = 0;
      if (GetThreadContext(t.h, &ctx)) n = SafeUnwind(ctx, frames);
      ResumeThread(t.h);
      if (n) {
        t.hist[Describe(frames, n)]++;
        t.samples++;
        uint32_t seen[kMaxFrames];
        int ns = 0;
        for (int i = 0; i < n; ++i) {
          uint32_t g = GuestOf(frames[i]);
          if (!g) continue;
          bool dup = false;
          for (int j = 0; j < ns; ++j) if (seen[j] == g) { dup = true; break; }
          if (dup) continue;
          seen[ns++] = g;
          t.incl[g]++;
        }
      }
    }

    if (now - last_report > std::chrono::seconds(5)) {
      last_report = now;
      for (auto& t : targets) {
        if (!t.samples) continue;
        std::vector<std::pair<std::string, uint32_t>> v(t.hist.begin(), t.hist.end());
        std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.second > b.second; });
        REXLOG_INFO("[FNC-PROF] thread {} ({} samples):", t.name, t.samples);
        for (size_t i = 0; i < v.size() && i < 15; ++i) {
          REXLOG_INFO("[FNC-PROF]   {:5.1f}%  {}", 100.0 * v[i].second / t.samples, v[i].first);
        }
        {
          std::vector<std::pair<uint32_t, uint32_t>> iv(t.incl.begin(), t.incl.end());
          std::sort(iv.begin(), iv.end());
          std::string line;
          for (auto& e : iv) {
            double pct = 100.0 * e.second / t.samples;
            if (pct >= 0.2) line += fmt::format(" {:08X}:{:.1f}", e.first, pct);
          }
          REXLOG_INFO("[FNC-PROFI] {} n={}{}", t.name, t.samples, line);
        }
        t.incl.clear();
        t.hist.clear();
        t.samples = 0;
      }
    }
    Sleep(1);
  }
}

}  // namespace
#endif

void Start(const void* func_mappings) {
#ifdef _WIN32
  wchar_t buf[MAX_PATH];
  DWORD n = GetModuleFileNameW(nullptr, buf, MAX_PATH);
  auto dir = std::filesystem::path(std::wstring(buf, n)).parent_path();
  BuildTable(static_cast<const Mapping*>(func_mappings));
  g_crash_path = (dir / "FNCReborn_crash.txt").wstring();
  SetUnhandledExceptionFilter(CrashFilter);
  std::signal(SIGABRT, AbortHandler);
  REXLOG_INFO("[FNC-CRASH] crash reports go to FNCReborn_crash.txt ({} game functions indexed)",
              g_fns.size());
  std::error_code ec;
  if (!std::filesystem::exists(dir / "profile_on", ec)) return;
  std::thread(SamplerMain).detach();
#else
  (void)func_mappings;
#endif
}

}  // namespace fnc_prof
