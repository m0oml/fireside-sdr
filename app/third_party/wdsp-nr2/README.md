# wdsp-nr2: WDSP's NR2 (EMNR) noise reduction, vendored

What it is: the spectral noise-reduction module ("NR2", `emnr.c`) of **WDSP**, Warren Pratt NR0V's signal-processing library for SDR
(https://github.com/TAPR/OpenHPSDR-wdsp). Used by `core/src/nr2.cpp` (`fireside/nr2.h`) as an optional voice noise reduction.

- Source: TAPR/OpenHPSDR-wdsp, commit `e69de50d1d702216098019013899ad87534c64e0` ("Release Version 2.11 (NNR model 1)"), `wdsp 2.11/Source/`.
- Licence: **GNU GPL v2 or (at your option) any later version** (each file's header; the full text is `LICENSE-GPL-2.0.txt`).
  fireside is GPLv3, which the "or later" allows. Copyright (C) 2015, 2025, 2026 Warren Pratt, NR0V. Not affiliated with or endorsed by him or TAPR.
- Files, unchanged from upstream except where `PATCHES.md` says: `src/emnr.c` (patched, see below), `src/emnr.h`, `src/calculus.c` + `calculus.h`
  (the trained gain tables, 3 MB of numbers), `src/zetaHat.c` + `zetaHat.h` (artifact-elimination tables), `include/fftw3.h` (the FFTW 3 header,
  GPL, from the same upstream tree).
- Added by fireside: `src/wdsp_standalone.h` (what NR2 needs from the rest of WDSP), `src/fireside_emnr.c` (one setter), `PATCHES.md`, this file.
- Build: `core/CMakeLists.txt`, only if the system has the FFTW 3 runtime library (`libfftw3.so.3`); otherwise NR2 is not built and the app has no NR2 button.
- Why only this module: WDSP is a whole receive/transmit chain with its own threads and Windows-oriented code. We needed one algorithm, which turned out
  to be self-contained apart from its channel setters.
