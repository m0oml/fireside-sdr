// Loads the proxy pal.dll (which loads the fake pal_real.dll), calls every export, checks each was forwarded unchanged and logged.
#include <windows.h>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <string>
#include <fstream>
#include <sstream>
static int fails = 0;
#define CHECK(c) do { if (!(c)) { std::printf("FAIL line %d: %s\n", __LINE__, #c); ++fails; } } while (0)
int main() {
    HMODULE p = LoadLibraryW(L"pal.dll"); CHECK(p != nullptr); if (!p) return 1;
    auto Init = (BOOL(__cdecl*)())GetProcAddress(p, "Init"); auto Num = (int(__cdecl*)())GetProcAddress(p, "GetNumDevices");
    auto Info = (BOOL(__cdecl*)(uint32_t, uint32_t*, uint32_t*))GetProcAddress(p, "GetDeviceInfo"); auto Sel = (BOOL(__cdecl*)(uint32_t))GetProcAddress(p, "SelectDevice");
    auto W = (int(__cdecl*)(uint32_t, uint32_t, uint32_t))GetProcAddress(p, "WriteOp"); auto R = (int(__cdecl*)(uint32_t, uint32_t, uint32_t, uint32_t*))GetProcAddress(p, "ReadOp");
    auto Ex = (void(__cdecl*)())GetProcAddress(p, "Exit"); auto Cb = (void(__cdecl*)(void*))GetProcAddress(p, "SetCallback"); auto Bs = (void(__cdecl*)(uint32_t))GetProcAddress(p, "SetBufferSize");
    CHECK(Init && Num && Info && Sel && W && R && Ex && Cb && Bs);
    HMODULE f = GetModuleHandleW(L"pal_real.dll");
    CHECK(Init() == TRUE); f = GetModuleHandleW(L"pal_real.dll"); CHECK(f != nullptr);
    auto St = f ? (void(__cdecl*)(uint32_t*, void**, uint32_t*))GetProcAddress(f, "FakeState") : nullptr; CHECK(St != nullptr); if (!St) { std::printf("TESTS FAILED (pal_real.dll not loaded)\n"); return 1; }
    uint32_t l[4]; void* c; uint32_t b;
    CHECK(Num() == 1); uint32_t m = 0, s = 0; CHECK(Info(0, &m, &s) == TRUE && m == 5000 && s == 0x03011234u); CHECK(Sel(0) == TRUE && Sel(1) == FALSE);
    float fv = 3.566f; uint32_t fb; std::memcpy(&fb, &fv, 4);
    CHECK(W(1292, 0, 0) == 1); St(l, &c, &b); CHECK(l[0] == 1292 && l[1] == 0 && l[2] == 0);
    CHECK(W(1240, 0x01D3671Au, fb) == 1); St(l, &c, &b); CHECK(l[0] == 1240 && l[1] == 0x01D3671Au && l[2] == fb);
    uint32_t out = 0; CHECK(R(1221, 7, 9, &out) == 1 && out == (1221u ^ 7u ^ 9u)); St(l, &c, &b); CHECK(l[0] == 1221 && l[1] == 7 && l[2] == 9);
    Cb((void*)0x1234); Bs(512); St(l, &c, &b); CHECK(c == (void*)0x1234 && b == 512);
    Ex(); St(l, &c, &b); CHECK(l[3] == 0xE717u);
    FreeLibrary(p);
    // the log
    WIN32_FIND_DATAW fd; HANDLE h = FindFirstFileW(L"pal_log_*.txt", &fd); CHECK(h != INVALID_HANDLE_VALUE);
    std::wstring newest; FILETIME best = {}; if (h != INVALID_HANDLE_VALUE) { do { if (CompareFileTime(&fd.ftLastWriteTime, &best) >= 0) { best = fd.ftLastWriteTime; newest = fd.cFileName; } } while (FindNextFileW(h, &fd)); FindClose(h); }
    std::ifstream in(newest.c_str()); std::stringstream ss; ss << in.rdbuf(); const std::string t = ss.str();
    for (const char* want : {"pal_real.dll loaded", "Init -> 1", "GetDeviceInfo 0 -> 1 model=5000 sn=0x03011234", "RDAL_OP_SET_MOX", "W 1292", "d1=0x01D3671A", "(3.566)", "R 1221", "out=0x", "SetBufferSize 512", "Exit", "# unloaded"})
        if (t.find(want) == std::string::npos) { std::printf("FAIL: log lacks '%s'\n", want); ++fails; }
    std::printf("%s\n", fails ? "TESTS FAILED" : "ALL TESTS PASSED"); std::printf("---- log ----\n%s", t.c_str());
    return fails ? 1 : 0;
}
