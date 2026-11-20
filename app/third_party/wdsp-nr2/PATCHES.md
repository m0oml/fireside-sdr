# Changes to the upstream files

Only `src/emnr.c` differs from upstream, by preprocessor lines only (`diff` shows nothing removed):

1. `#include "comm.h"` (which pulls in all of WDSP and the Windows API) became
   `#ifdef WDSP_STANDALONE / #include "wdsp_standalone.h" / #else / #include "comm.h" / #endif`.
2. The setter functions that reach into a WDSP channel (`rxa[ch]`, `ch[ch].csDSP`: `SetRXAEMNRpost2*`, `SetRXAEMNRRun`, `...gainMethod`,
   `...npeMethod`, `...aeRun`, `...Position`, `...aeZetaThresh`, `...aePsi`, `...trainZetaThresh`, `...trainT2`) are inside `#ifndef WDSP_STANDALONE`.
   The algorithm itself (`create_emnr`, `xemnr`, `flush_emnr`, `destroy_emnr`, `setBuffers_emnr`, `setSamplerate_emnr`, `setSize_emnr`) is untouched.

`wdsp_standalone.h` replaces `malloc0` / `_aligned_free` (posix_memalign, zeroed), `mlog10` (WDSP's table-driven log10 is replaced by `log10`,
a speed-up that is not needed here), `PI`, `TWOPI`, `min`, `max`, `PORT`, the `complex` typedef (only `sizeof` is used).

`src/fireside_emnr.c` (ours, not upstream) holds `fireside_emnr_post2()`, the equivalent of upstream's `SetRXAEMNRpost2Run/Factor/Nlevel` for the standalone build.

Settings used (WDSP's defaults for NR2): FFT 4096, overlap 4, Hann window, output gain 1.0, trained gain method (2), OSMS noise estimate (0),
artifact elimination on; and post-processing ON (factor 0.15, level 0.15: upstream leaves it off). The gain method, noise estimator and artifact
elimination were compared on speech in real band noise and made no real difference; post-processing added about 3-4 dB of output SNR. Block size 128 samples. FFTW's planner is not thread-safe, so `nr2.cpp` serialises create / destroy.
