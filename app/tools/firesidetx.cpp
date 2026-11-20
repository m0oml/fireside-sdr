// firesidetx: renders transmit signals to a WAV file (48 kHz, float32, I = ch0, Q = ch1, dial at 0 Hz) so they can be opened in the receive app and
// seen on the waterfall. FILE OUTPUT ONLY: it has no connection to the radio. Output never exceeds -1 dBFS.
#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include "fireside/tx_render.h"
using namespace fireside::tx;

static void usage() {
    std::puts("usage: firesidetx ssb <in.wav> <out.wav> [--lsb] [--no-mic-chain] [--gain dB]\n"
              "       firesidetx ft8 \"CQ M0OML IO91\" <out.wav> [--freq 1000] [--lsb] [--level -6]      (one 15 s slot)\n"
              "       firesidetx ft4 \"CQ M0OML IO91\" <out.wav> [--freq 1000] [--lsb] [--level -6]      (one 7.5 s slot)\n"
              "       firesidetx cw  \"CQ DE M0OML\" <out.wav> [--wpm 20] [--edge 5] [--offset 0] [--level -6]\n"
              "       firesidetx psk31 \"CQ CQ DE M0OML\" <out.wav> [--freq 1000] [--lsb] [--level -6]\n"
              "       firesidetx rtty \"CQ CQ DE M0OML\" <out.wav> [--freq 2125] [--lsb] [--level -6]      (--freq is the mark tone)\n"
              "       firesidetx twotone <out.wav> [--seconds 4] [--peak -6] [--lsb]\n"
              "  Writes a complex baseband WAV only. Open it in fireside (Open file) or: fireside --decode-file out.wav ft8|ft4|cw.\n"
              "  CW puts the carrier on the dial (offset 0); the receiver's CW mode makes the beat note. Levels are dBFS (at most -1).");
}
int main(int argc, char** argv) {
    if (argc < 3) { usage(); return 2; }
    try {
        const std::string mode = argv[1]; Sideband sb = Sideband::USB; double freq = 1000, level = -6, wpm = 20, edge = 5, offset = 0, seconds = 4, peak = -6, gain = 0; bool mic = true;
        std::string a1, a2; int pos = 0;
        for (int i = 2; i < argc; ++i) {
            auto next = [&]() -> const char* { if (i + 1 >= argc) { usage(); std::exit(2); } return argv[++i]; };
            if (!std::strcmp(argv[i], "--lsb")) sb = Sideband::LSB; else if (!std::strcmp(argv[i], "--no-mic-chain")) mic = false;
            else if (!std::strcmp(argv[i], "--freq")) freq = std::atof(next()); else if (!std::strcmp(argv[i], "--level")) level = std::atof(next());
            else if (!std::strcmp(argv[i], "--wpm")) wpm = std::atof(next()); else if (!std::strcmp(argv[i], "--edge")) edge = std::atof(next());
            else if (!std::strcmp(argv[i], "--offset")) offset = std::atof(next()); else if (!std::strcmp(argv[i], "--seconds")) seconds = std::atof(next());
            else if (!std::strcmp(argv[i], "--peak")) peak = std::atof(next()); else if (!std::strcmp(argv[i], "--gain")) gain = std::atof(next());
            else if (argv[i][0] == '-' && argv[i][1] == '-') { std::fprintf(stderr, "unknown option %s\n", argv[i]); return 2; }
            else if (pos == 0) { a1 = argv[i]; ++pos; } else if (pos == 1) { a2 = argv[i]; ++pos; } else { usage(); return 2; }
        }
        std::vector<fireside::dsp::cf> iq; std::string out;
        if (mode == "ssb" && pos == 2) { iq = render_ssb(read_audio_wav(a1), sb, mic, gain); out = a2; }
        else if ((mode == "ft8" || mode == "ft4") && pos == 2) { iq = render_ft(a1, freq, mode == "ft4", sb, level); out = a2; }
        else if (mode == "cw" && pos == 2) { iq = render_cw(a1, wpm, edge, level, offset); out = a2; }
        else if (mode == "psk31" && pos == 2) { iq = render_psk31(a1, freq, sb, level); out = a2; }
        else if (mode == "rtty" && pos == 2) { iq = render_rtty(a1, freq == 1000 ? 2125 : freq, sb, level); out = a2; }
        else if (mode == "twotone" && pos == 1) { iq = render_two_tone(seconds, peak, sb); out = a1; }
        else { usage(); return 2; }
        write_iq_wav(out, iq);
        std::printf("wrote %s: %.2f s, %zu samples, peak %.2f dBFS (%s)\n", out.c_str(), double(iq.size()) / kRenderRate, iq.size(), 20 * std::log10(peak_magnitude(iq) + 1e-12), sb == Sideband::USB ? "USB" : "LSB");
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "firesidetx: %s\n", e.what()); return 1; }
}
