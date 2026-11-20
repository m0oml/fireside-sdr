// Test stand-in for pal_real.dll: same exports; records what it was called with so the test can check forwarding.
#include <windows.h>
#include <cstdint>
static uint32_t last[4]; static void* cb_seen; static uint32_t bufsz;
extern "C" {
BOOL __cdecl Init() { return TRUE; }
int __cdecl GetNumDevices() { return 1; }
BOOL __cdecl GetDeviceInfo(uint32_t i, uint32_t* m, uint32_t* s) { *m = 5000; *s = 0x03011234u + i; return TRUE; }
BOOL __cdecl SelectDevice(uint32_t i) { return i == 0; }
int __cdecl WriteOp(uint32_t op, uint32_t a, uint32_t b) { last[0] = op; last[1] = a; last[2] = b; return 1; }
int __cdecl ReadOp(uint32_t op, uint32_t a, uint32_t b, uint32_t* out) { last[0] = op; last[1] = a; last[2] = b; *out = op ^ a ^ b; return 1; }
void __cdecl Exit() { last[3] = 0xE717u; }
void __cdecl SetCallback(void* c) { cb_seen = c; }
void __cdecl SetBufferSize(uint32_t v) { bufsz = v; }
__declspec(dllexport) void __cdecl FakeState(uint32_t* l, void** c, uint32_t* b) { for (int i = 0; i < 4; ++i) l[i] = last[i]; *c = cb_seen; *b = bufsz; }
}
