// Self-test fixture: device-level code in a transmit source.
#include <cstdio>
int f() { return std::fopen("/dev/snd/midiC1D0", "w") != nullptr; }
