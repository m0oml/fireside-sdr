// pal.dll logging proxy (x86, for PowerSDR 2.x). Interoperability tool: records every call PowerSDR makes into FlexRadio's
// pal.dll (the radio's command mailbox: opcode, data1, data2, result) and forwards it unchanged to the original, renamed to
// pal_real.dll in the same folder. It never makes a call of its own, never changes an argument or a result.
//
// Install: rename PowerSDR's pal.dll to pal_real.dll, copy this pal.dll beside it. Remove: delete it and rename pal_real.dll back.
// Log: pal_log_<local time>.txt beside the DLL, one line per call, flushed at once (a crash loses nothing).
//
// Signatures from PowerSDR's DllImport declarations (PowerSDRServer/PalInteropMethods.cs, Console/FWC/pal.cs): all cdecl.
// Float overloads of WriteOp / ReadOp pass the same 32 bits, so each value is logged both as hex and as a float.
#include <windows.h>
#include <cstdarg>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include "fireside/opcodes_gen.h"

namespace {
typedef BOOL (__cdecl* InitFn)();
typedef int (__cdecl* GetNumDevicesFn)();
typedef BOOL (__cdecl* GetDeviceInfoFn)(uint32_t, uint32_t*, uint32_t*);
typedef BOOL (__cdecl* SelectDeviceFn)(uint32_t);
typedef int (__cdecl* WriteOpFn)(uint32_t, uint32_t, uint32_t);
typedef int (__cdecl* ReadOpFn)(uint32_t, uint32_t, uint32_t, uint32_t*);
typedef void (__cdecl* ExitFn)();
typedef void (__cdecl* SetCallbackFn)(void*);
typedef void (__cdecl* SetBufferSizeFn)(uint32_t);

struct Real {
    HMODULE h = nullptr; bool tried = false;
    InitFn Init = nullptr; GetNumDevicesFn GetNumDevices = nullptr; GetDeviceInfoFn GetDeviceInfo = nullptr; SelectDeviceFn SelectDevice = nullptr;
    WriteOpFn WriteOp = nullptr; ReadOpFn ReadOp = nullptr; ExitFn Exit = nullptr; SetCallbackFn SetCallback = nullptr; SetBufferSizeFn SetBufferSize = nullptr;
} g;
CRITICAL_SECTION g_lock; FILE* g_log = nullptr; HMODULE g_self = nullptr; LARGE_INTEGER g_t0, g_freq;
wchar_t g_dir[MAX_PATH] = {0};

const char* op_name(uint32_t v) { for (const auto& o : fireside::kOps) if (o.value == v) return o.name; return "?"; }
float as_float(uint32_t v) { float f; std::memcpy(&f, &v, 4); return f; }
double now_ms() { LARGE_INTEGER t; QueryPerformanceCounter(&t); return 1000.0 * double(t.QuadPart - g_t0.QuadPart) / double(g_freq.QuadPart); }

void logf(const char* fmt, ...) {
    EnterCriticalSection(&g_lock);
    if (g_log) {
        std::fprintf(g_log, "%11.3f %5lu ", now_ms(), GetCurrentThreadId());
        va_list a; va_start(a, fmt); std::vfprintf(g_log, fmt, a); va_end(a);
        std::fputc('\n', g_log); std::fflush(g_log);
    }
    LeaveCriticalSection(&g_lock);
}

void open_log() {
    SYSTEMTIME st; GetLocalTime(&st);
    wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%lspal_log_%04u%02u%02u_%02u%02u%02u.txt", g_dir, st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
    g_log = _wfopen(p, L"w");
    SYSTEMTIME u; GetSystemTime(&u);
    if (g_log) std::fprintf(g_log, "# pal.dll logging proxy. Started %04u-%02u-%02uT%02u:%02u:%02u.%03uZ. Columns: ms since start, thread, call.\n"
                                   "# Values are hex with a float reading in brackets (PowerSDR passes some as float). Forwards to pal_real.dll unchanged.\n",
                            u.wYear, u.wMonth, u.wDay, u.wHour, u.wMinute, u.wSecond, u.wMilliseconds);
}

bool load_real() {                                       // lazily, on the first call (never inside DllMain)
    EnterCriticalSection(&g_lock);
    if (!g.tried) {
        g.tried = true;
        wchar_t p[MAX_PATH]; swprintf(p, MAX_PATH, L"%lspal_real.dll", g_dir);
        g.h = LoadLibraryW(p);
        if (g.h) {
            g.Init = (InitFn)GetProcAddress(g.h, "Init"); g.GetNumDevices = (GetNumDevicesFn)GetProcAddress(g.h, "GetNumDevices");
            g.GetDeviceInfo = (GetDeviceInfoFn)GetProcAddress(g.h, "GetDeviceInfo"); g.SelectDevice = (SelectDeviceFn)GetProcAddress(g.h, "SelectDevice");
            g.WriteOp = (WriteOpFn)GetProcAddress(g.h, "WriteOp"); g.ReadOp = (ReadOpFn)GetProcAddress(g.h, "ReadOp"); g.Exit = (ExitFn)GetProcAddress(g.h, "Exit");
            g.SetCallback = (SetCallbackFn)GetProcAddress(g.h, "SetCallback"); g.SetBufferSize = (SetBufferSizeFn)GetProcAddress(g.h, "SetBufferSize");
        }
        if (g_log) std::fprintf(g_log, "# pal_real.dll %s\n", g.h ? "loaded" : "NOT FOUND: every call fails (PowerSDR will report no radio)"), std::fflush(g_log);
    }
    const bool ok = g.h != nullptr; LeaveCriticalSection(&g_lock); return ok;
}
}  // namespace

extern "C" {
BOOL __cdecl Init() { if (!load_real() || !g.Init) { logf("Init -> FAIL (no pal_real.dll)"); return FALSE; } const BOOL r = g.Init(); logf("Init -> %d", r); return r; }
int __cdecl GetNumDevices() { if (!load_real() || !g.GetNumDevices) return 0; const int r = g.GetNumDevices(); logf("GetNumDevices -> %d", r); return r; }
BOOL __cdecl GetDeviceInfo(uint32_t i, uint32_t* model, uint32_t* sn) {
    if (!load_real() || !g.GetDeviceInfo) return FALSE; const BOOL r = g.GetDeviceInfo(i, model, sn);
    logf("GetDeviceInfo %u -> %d model=%u sn=0x%08X", i, r, model ? *model : 0u, sn ? *sn : 0u); return r;
}
BOOL __cdecl SelectDevice(uint32_t i) { if (!load_real() || !g.SelectDevice) return FALSE; const BOOL r = g.SelectDevice(i); logf("SelectDevice %u -> %d", i, r); return r; }
int __cdecl WriteOp(uint32_t op, uint32_t d1, uint32_t d2) {
    if (!load_real() || !g.WriteOp) return 0; const double t = now_ms(); const int r = g.WriteOp(op, d1, d2);
    logf("W %4u %-34s d1=0x%08X (%g) d2=0x%08X (%g) -> %d  [%.2f ms]", op, op_name(op), d1, as_float(d1), d2, as_float(d2), r, now_ms() - t); return r;
}
int __cdecl ReadOp(uint32_t op, uint32_t d1, uint32_t d2, uint32_t* out) {
    if (!load_real() || !g.ReadOp) return 0; const double t = now_ms(); const int r = g.ReadOp(op, d1, d2, out); const uint32_t v = out ? *out : 0u;
    logf("R %4u %-34s d1=0x%08X (%g) d2=0x%08X (%g) -> %d out=0x%08X (%g)  [%.2f ms]", op, op_name(op), d1, as_float(d1), d2, as_float(d2), r, v, as_float(v), now_ms() - t); return r;
}
void __cdecl Exit() { if (!load_real() || !g.Exit) return; logf("Exit"); g.Exit(); }
void __cdecl SetCallback(void* cb) { if (!load_real() || !g.SetCallback) return; logf("SetCallback %p (passed through; events are not logged)", cb); g.SetCallback(cb); }
void __cdecl SetBufferSize(uint32_t v) { if (!load_real() || !g.SetBufferSize) return; logf("SetBufferSize %u", v); g.SetBufferSize(v); }
}

BOOL WINAPI DllMain(HINSTANCE h, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h; DisableThreadLibraryCalls(h); InitializeCriticalSection(&g_lock); QueryPerformanceFrequency(&g_freq); QueryPerformanceCounter(&g_t0);
        GetModuleFileNameW(h, g_dir, MAX_PATH); wchar_t* s = wcsrchr(g_dir, L'\\'); if (s) s[1] = 0; else g_dir[0] = 0;
        open_log();
    } else if (reason == DLL_PROCESS_DETACH) {
        if (g_log) { std::fprintf(g_log, "# unloaded\n"); std::fclose(g_log); g_log = nullptr; }   // pal_real.dll is not freed here (loader lock)
    }
    return TRUE;
}
