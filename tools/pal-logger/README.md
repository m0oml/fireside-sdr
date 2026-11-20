# pal.dll logging proxy (interoperability tool)

Records every call PowerSDR 2.x makes into FlexRadio's `pal.dll` (the radio's command mailbox: opcode, data1, data2,
result) and forwards it unchanged to the original. It never makes a call of its own and never changes a value.

- `out/` (build output, not committed) is the **32-bit build for PowerSDR** (PowerSDR 2.x is x86): `pal.dll` (the proxy), plus a test: `pal_real.dll`
  (a FAKE stand-in, test only) and `test_logger.exe`.
- `out64/` (not committed either) is a 64-bit build of the same source, used only to run the test under 64-bit Wine on Linux.

## 1. Test first (on Windows, no radio needed)
In a copy of `out\` (keep the fake pal_real.dll THERE only): run `test_logger.exe`. It must print `ALL TESTS PASSED`.

## 2. Install into PowerSDR (PowerSDR closed)
1. In PowerSDR's install folder (e.g. `C:\Program Files (x86)\FlexRadio Systems\PowerSDR v2.7.2\`), rename `pal.dll` to
   `pal_real.dll`. Keep a separate backup copy of the original too.
2. Copy `out\pal.dll` (the proxy) into that folder. Do NOT copy the fake `pal_real.dll`.
3. Start PowerSDR. A `pal_log_<date>_<time>.txt` appears beside the DLL (the folder must be writable: run PowerSDR as
   administrator if no log appears). If the log says `pal_real.dll NOT FOUND`, step 1 was missed: PowerSDR sees no radio.

## 3. Remove
Close PowerSDR, delete the proxy `pal.dll`, rename `pal_real.dll` back to `pal.dll`.

Build (Linux): `i686-w64-mingw32-g++ -O2 -std=c++17 -static -static-libgcc -static-libstdc++ -shared -I../../app/core/include pal_logger.cpp pal.def -o out/pal.dll`
The fake (`fake_pal.cpp` + `fake_pal.def`, output `out/pal_real.dll`) and the test (`test_logger.cpp`, output `out/test_logger.exe`) build the same way; `build.bat` shows the Windows flags. Opcode names come from `app/core/include/fireside/opcodes_gen.h`.
Not logged: radio events through the callback (passed straight through), and the audio stream (that goes through
FlexRadio's ASIO driver, not pal.dll).
