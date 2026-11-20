// ft8bench [--ft4]: decode 15 s FT8 (or 7.5 s FT4) slot files (12 kHz mono 16-bit WAV, as tools/compare_ft8.py writes for jt9) with our decoder, one slot per file
// with a fresh callsign table (as jt9 decodes each file alone). Prints "<file index> <snr> <dt> <freq> ~ <text>" per decode. For decoder tuning.
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <cstdlib>
#include <random>
#include <vector>
#include "fireside/decode.h"
using namespace fireside::decode;
int main(int argc, char** argv) {
    if (argc < 2) { std::puts("usage: ft8bench [--ft4] slot1.wav [slot2.wav ...]"); return 2; }
    double total_s = 0, worst_s = 0; int first = 1; bool ft4 = false; if (!std::strcmp(argv[1], "--ft4")) { ft4 = true; first = 2; }
    if (argc > first + 1 && !std::strcmp(argv[first], "--noise")) {           // --noise N: N slots of white Gaussian noise; every decode is false (false-decode rate)
        const int n = std::atoi(argv[first + 1]); std::mt19937 rng(std::getenv("FT8_NOISE_SEED") ? unsigned(std::atoi(std::getenv("FT8_NOISE_SEED"))) : 12345u); std::normal_distribution<float> g(0.0f, 0.1f); int falses = 0;
        for (int k = 0; k < n; ++k) { std::vector<float> a(size_t((ft4 ? 7.5 : 15.0) * 12000)); for (auto& v : a) v = g(rng);
            for (const auto& m : Ft8Decoder::decode_slot(a.data(), a.size(), "", ft4)) { ++falses; std::printf("NOISE slot %d: %d %.2f %.1f ~ %s\n", k, m.snr, m.dt, m.freq, m.text.c_str()); } }
        std::printf("noise: %d slots, %d false decodes\n", n, falses); return 0;
    }
    for (int i = first; i < argc; ++i) {
        std::ifstream f(argv[i], std::ios::binary); std::vector<char> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        if (b.size() < 44) { std::fprintf(stderr, "%s: too short\n", argv[i]); return 1; }
        std::vector<float> a((b.size() - 44) / 2); for (size_t k = 0; k < a.size(); ++k) { int16_t v; std::memcpy(&v, b.data() + 44 + 2 * k, 2); a[k] = float(v) / 32768.0f; }
        const auto t0 = std::chrono::steady_clock::now();
        const auto r = Ft8Decoder::decode_slot(a.data(), a.size(), std::to_string(i - first), ft4, std::getenv("FT8_MYCALL") ? std::getenv("FT8_MYCALL") : "");
        const double s = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(); total_s += s; worst_s = std::max(worst_s, s);
        for (const auto& m : r) std::printf("%d %d %.2f %.1f ~ %s\n", i - first, m.snr, m.dt, m.freq, m.text.c_str());
    }
    std::fprintf(stderr, "decode time: %.2f s total, %.2f s worst slot\n", total_s, worst_s);
}
