// firesideplay: the output-channel test (audio-stream-layout.md section 7, step 4). Plays a QUIET 1 kHz tone on ONE output channel at a time, so you can listen
// (headphones/speaker on the radio, volume at minimum) and note which jack each channel comes out of. Audio only: no opcodes, no keying, never run during transmit.
// Refuses anything louder than -20 dBFS. Default -40 dBFS, 2 s per channel, 48 kHz.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#include "fireside/playback.h"
using namespace fireside;

static void usage() {
    std::puts("usage: firesideplay <card-id|device> --channel N [--channel M ...] [--level -40] [--seconds 2] [--rate 48000] [--channels 8]\n"
              "       firesideplay <card-id|device> --all        (channels 0..7, one after the other, announced on the terminal)\n"
              "  <card-id> becomes hw:CARD=<id>,DEV=0; anything with ':' (or 'null') is an ALSA device name.\n"
              "  --level in dBFS, default -40, refused above -20. Turn the radio's AF gain / headphone volume to MINIMUM first, then raise slowly.\n"
              "  Write down, for each channel, which jack (phones / speaker / line out / none) and which side you hear it on.");
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    try {
        std::string dev = argv[1]; double level = -40, seconds = 2, rate = 48000; int nch = 8; std::vector<int> chans; bool all = false;
        if (dev.find(':') == std::string::npos && dev != "null" && dev != "default") dev = "hw:CARD=" + dev + ",DEV=0";
        for (int i = 2; i < argc; ++i) {
            auto next = [&]() -> const char* { if (i + 1 >= argc) { usage(); std::exit(2); } return argv[++i]; };
            if (!std::strcmp(argv[i], "--channel")) chans.push_back(std::atoi(next()));
            else if (!std::strcmp(argv[i], "--all")) all = true;
            else if (!std::strcmp(argv[i], "--level")) level = std::atof(next());
            else if (!std::strcmp(argv[i], "--seconds")) seconds = std::atof(next());
            else if (!std::strcmp(argv[i], "--rate")) rate = std::atof(next());
            else if (!std::strcmp(argv[i], "--channels")) nch = std::atoi(next());
            else { usage(); return 2; }
        }
        if (all) { chans.clear(); for (int c = 0; c < nch; ++c) chans.push_back(c); }
        if (chans.empty()) { usage(); return 2; }
        if (!(level <= -20.0)) { std::fprintf(stderr, "refusing level %.1f dBFS: the limit is -20 dBFS (use something quieter)\n", level); return 2; }
        if (seconds <= 0 || seconds > 30) { std::fprintf(stderr, "--seconds must be 0..30\n"); return 2; }
        for (int c : chans) if (c < 0 || c >= nch) { std::fprintf(stderr, "channel %d is outside 0..%d\n", c, nch - 1); return 2; }
        auto pcm = open_alsa_play(dev, rate, nch, 1024);
        std::printf("playing on %s: %.0f dBFS 1 kHz, %.1f s per channel\n", pcm->describe().c_str(), level, seconds);
        const double amp = std::pow(10.0, level / 20.0) * 2147483647.0, w = 2.0 * M_PI * 1000.0 / rate;
        const size_t total = size_t(seconds * rate), fade = size_t(0.02 * rate); size_t phase = 0;
        for (int c : chans) {
            std::printf("channel %d ... ", c); std::fflush(stdout);
            std::vector<int32_t> buf(1024 * size_t(nch)); size_t done = 0;
            while (done < total) {
                const size_t n = std::min<size_t>(1024, total - done);
                std::fill(buf.begin(), buf.end(), 0);
                for (size_t k = 0; k < n; ++k) {
                    const size_t pos = done + k; double env = 1.0;                      // short fades: no click
                    if (pos < fade) env = double(pos) / fade; else if (total - pos < fade) env = double(total - pos) / fade;
                    buf[k * size_t(nch) + size_t(c)] = int32_t(amp * env * std::sin(w * double(phase++)));
                }
                size_t off = 0;
                while (off < n) { const size_t a = pcm->write(buf.data() + off * size_t(nch), n - off); off += a; if (!a) std::this_thread::sleep_for(std::chrono::milliseconds(2)); }
                done += n;
            }
            std::puts("done"); std::this_thread::sleep_for(std::chrono::milliseconds(700));     // gap between channels
        }
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "firesideplay: %s\n", e.what()); return 1; }
}
