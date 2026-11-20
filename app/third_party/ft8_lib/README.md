# ft8_lib (vendored)

FT8/FT4 decoding library by Karlis Goba (YL3JG), MIT licence (see LICENSE).
Source: https://github.com/kgoba/ft8_lib at commit `9fec6ca39886edbf96f4f5e71edc76da5074e871` (2025-08-24, "non-standard callsigns; special CQ; field type annotation (#47)").

Copied unmodified: `ft8/` (LDPC, CRC, message pack/unpack, sync search, encoder), `fft/` (KISS FFT), `common/monitor.*` and `common/common.h`.
Not copied: the demo programs, the audio/wave helpers (we have our own), the Fortran tables, the large test set.
`tests/ft8/` holds four of the upstream real on-air recordings (15 s, 12 kHz, 2019-11-11) with the messages WSJT-X decoded from them (the .txt files), used as ground truth.
Our wrapper is `core/src/decode_ft8.cpp`. To update: replace these directories from upstream and re-run the tests.
