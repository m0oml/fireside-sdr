// firesidecapture: READ-ONLY first-contact audio capture from the radio's ALSA card (audio-stream-layout.md section 7, experiments 1-3).
// Opens a capture stream only. Never plays audio, never touches mixer/controls/MIDI, never writes to the radio. Run it after `firesidectl startup` has put the radio in receive.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <thread>
#include <ctime>
#include <fstream>
#include <sys/time.h>
#include "fireside/capture.h"
#include "fireside/fw_iso.h"
#include <atomic>
using namespace fireside;

static void usage() {
    std::puts("usage: firesidecapture --list\n"
              "       firesidecapture <card-id|device|direct> [--rate 48000|96000|192000] [--channels 8] [--seconds 5] [--out capture.wav] [--duplex]\n"
              "  direct = the radio's FireWire stream received by this program, bypassing snd-dice (48/96/192 kHz). It sets the radio's\n"
              "  stream clock/channel/enable (three DICE registers, restored on exit); no radio commands. Close ALSA users of the radio first.\n"
              "  <card-id> becomes hw:CARD=<id>,DEV=0; anything containing ':' (or 'null') is used as an ALSA device name.\n"
              "  Records capture only. --out writes every channel as float32 WAV (analyse with: firesideiq capture.wav [--chans 0,1]).\n"
              "  --stream iq.wav writes RX1 I/Q (ch0/ch1) as 32-bit PCM straight to disk (any length, up to 3600 s);\n"
              "  --align-ft8 starts recording exactly on the next 15 s UTC boundary (prints the epoch) for FT8 comparisons.");
}
static volatile std::sig_atomic_t g_stop = 0;                       // SIGINT / SIGTERM: finish the recording cleanly (header written, radio's stream registers restored)
static void on_signal(int) { g_stop = 1; }
static double dbfs(double v) { return v > 0 ? 20.0 * std::log10(v) : -200.0; }

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    std::signal(SIGINT, on_signal); std::signal(SIGTERM, on_signal);
    try {
        if (!std::strcmp(argv[1], "--list")) {
            auto cards = list_capture_cards();
            if (cards.empty()) std::puts("no ALSA capture cards found");
            for (const auto& c : cards) {
                const auto caps = probe_alsa_pcm(c.device());
                std::printf("%s%-10s %s   [%s]\n", c.flex_like ? "* " : "  ", c.id.c_str(), c.name.c_str(), c.device().c_str());
                if (!caps.ok) { std::printf("      probe: %s\n", caps.error.c_str()); continue; }
                std::printf("      channels %d..%d, S32_LE %s, rates:", caps.ch_min, caps.ch_max, caps.s32le ? "yes" : "NO");
                for (unsigned r : caps.rates_ok) std::printf(" %u", r);
                std::printf("   (offers %u..%u Hz)\n", caps.rate_min, caps.rate_max);
            }
            std::puts("(* = looks like the FLEX/DICE card)");
            const auto fp = probe_fw_iso();
            if (fp.ok) { std::printf("* direct     FireWire stream from %s via %s, rates:", fp.radio_node.c_str(), fp.local_node.c_str()); for (unsigned r : fp.rates) std::printf(" %u", r); std::puts(fp.busy ? "   (BUSY: something is streaming)" : ""); }
            else std::printf("  direct     not available: %s\n", fp.error.c_str());
            return 0;
        }
        std::string dev = argv[1]; double rate = 48000, seconds = 5; int channels = 8; std::string out, stream; bool align = false, duplex = false; int beep_ch = -1;
        const bool direct = dev == "direct";
        if (!direct && dev.find(':') == std::string::npos && dev != "null" && dev != "default") dev = "hw:CARD=" + dev + ",DEV=0";
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i]; auto next = [&]() -> const char* { if (i + 1 >= argc) throw std::runtime_error("missing value for " + a); return argv[++i]; };
            if (a == "--rate") rate = std::atof(next()); else if (a == "--channels") channels = std::atoi(next()); else if (a == "--seconds") seconds = std::atof(next()); else if (a == "--out") out = next(); else if (a == "--stream") stream = next(); else if (a == "--align-ft8") align = true; else if (a == "--duplex") duplex = true; else if (a == "--beep") beep_ch = std::atoi(next());
            else { usage(); return 2; }
        }
        if (!(seconds > 0 && seconds <= (stream.empty() ? 120 : 3600)) || channels < 2 || channels > 32) throw std::runtime_error("--seconds must be in (0,120] (with --stream up to 3600), --channels in 2..32");
        if (!stream.empty() && !out.empty()) throw std::runtime_error("use --out or --stream, not both");
        const size_t total = size_t(seconds * rate);
        if (!out.empty() && double(total) * channels > 120e6) throw std::runtime_error("recording too large for memory (frames x channels > 120 M)");
        if (direct && channels != 8) throw std::runtime_error("direct: the radio streams 8 channels (--channels 8)");
        if (duplex && !direct) throw std::runtime_error("--duplex needs direct");
        FwIsoPcm* fwp = nullptr; std::unique_ptr<IPcm> pcm;
        if (direct) { FwIsoOptions fo; fo.duplex = duplex; pcm = open_fw_iso_pcm(unsigned(rate), fo); fwp = dynamic_cast<FwIsoPcm*>(pcm.get()); }   // duplex: also sends SILENCE to the radio (this program never keys)
        else pcm = open_alsa_pcm(dev, rate, channels, 1024);
        PcmIqSource src(std::move(pcm), 0, 1, 1024);
        // --beep N (with --duplex): a 1 kHz tone at -40 dBFS on output channels N and N+1 (4 = the radio's external speaker), through the duplex
        // stream: proves the radio plays what we send, with NO RF (channels 0/1, the transmitter's I/Q, stay exactly zero; nothing keys).
        std::atomic<bool> beep_stop{false}; std::thread beep;
        if (beep_ch >= 0) {
            if (!duplex || beep_ch < 2 || beep_ch > 6) throw std::runtime_error("--beep needs --duplex and a channel 2..6 (never 0/1: the transmit I/Q)");
            auto outp = open_fw_iso_out(FwIsoPcm::OutKind::Audio);   // the audio ring: channels 2..7 only
            beep = std::thread([&beep_stop, beep_ch, rate, o = std::move(outp)]() mutable {
                std::vector<int32_t> b(8 * 256, 0); double ph = 0; const double w = 2 * M_PI * 1000.0 / rate;
                try { while (!beep_stop) {
                    for (size_t f = 0; f < 256; ++f) { const int32_t v = int32_t(std::lround(0.01 * std::sin(ph) * 2147483647.0)); ph += w; b[f * 8 + size_t(beep_ch)] = v; b[f * 8 + size_t(beep_ch) + 1] = v; }
                    size_t done = 0; while (done < 256 && !beep_stop) { const size_t n = o->write(b.data() + done * 8, 256 - done); if (n == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1)); done += n; }
                } } catch (...) {}
            });
            std::printf("beep: 1 kHz at -40 dBFS on output channels %d/%d (no RF: ch0/1 stay zero)\n", beep_ch, beep_ch + 1);
        }
        std::printf("capturing %.1f s from %s\n", seconds, src.describe().c_str());
        std::vector<float> rec; std::vector<dsp::cf> iq; std::vector<int32_t> raw; size_t got = 0; int idle = 0;
        auto now_s = [] { timeval tv{}; gettimeofday(&tv, nullptr); return double(tv.tv_sec) + double(tv.tv_usec) / 1e6; };
        std::ofstream sf; std::vector<int32_t> iqbuf;
        auto wav_header = [&](uint64_t frames) {                                    // 2-ch 32-bit PCM (S32_LE), as arecord writes
            const uint32_t data = uint32_t(std::min<uint64_t>(frames * 8, 0xFFFFFFF0u)), r = uint32_t(rate);
            auto u32 = [&](uint32_t v) { sf.write(reinterpret_cast<const char*>(&v), 4); }; auto u16 = [&](uint16_t v) { sf.write(reinterpret_cast<const char*>(&v), 2); };
            sf.seekp(0); sf.write("RIFF", 4); u32(36 + data); sf.write("WAVEfmt ", 8); u32(16); u16(1); u16(2); u32(r); u32(r * 8); u16(8); u16(32); sf.write("data", 4); u32(data);
        };
        if (!stream.empty()) { sf.open(stream, std::ios::binary | std::ios::trunc); if (!sf) throw std::runtime_error("cannot write " + stream); wav_header(0); }
        if (align) {                                                                // discard until the next 15 s UTC boundary, then record from it
            const double b = std::ceil((now_s() + 0.3) / 15.0) * 15.0;
            while (now_s() < b && !g_stop) { raw.clear(); if (src.read_raw(raw, 512, 0.05) == 0 && ++idle > 100) throw std::runtime_error("no data from the device for 5 s"); }
            idle = 0; std::printf("aligned: recording starts at epoch %.3f (slot boundary %.0f)\n", now_s(), b); std::fflush(stdout);
        }
        const auto t0 = std::chrono::steady_clock::now();
        while (got < total && !g_stop) {
            raw.clear(); const size_t n = src.read_raw(raw, std::min<size_t>(4096, total - got), 0.25);
            if (n == 0) { if (++idle > 20) throw std::runtime_error("no data from the device for 5 s"); continue; }
            idle = 0; got += n;
            if (sf.is_open()) { iqbuf.resize(2 * n); for (size_t f = 0; f < n; ++f) { iqbuf[2 * f] = raw[f * size_t(channels)]; iqbuf[2 * f + 1] = raw[f * size_t(channels) + 1]; } sf.write(reinterpret_cast<const char*>(iqbuf.data()), std::streamsize(iqbuf.size() * 4)); }
            for (size_t f = 0; f < n && iq.size() < (1u << 18); ++f) iq.emplace_back(s32_to_float(raw[f * size_t(channels)]), s32_to_float(raw[f * size_t(channels) + 1]));
            if (!out.empty()) for (int32_t v : raw) rec.push_back(s32_to_float(v));
        }
        beep_stop = true; if (beep.joinable()) beep.join();
        const double wall = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
        const auto& st = src.stats();
        std::printf("%llu frames in %.2f s wall (%.0f frames/s), xruns %llu, empty reads %llu, stream %s\n", (unsigned long long)st.frames, wall, double(st.frames) / wall,
                    (unsigned long long)st.xruns, (unsigned long long)st.empty_reads, src.stream_dead() ? "DEAD (all zero > 1 s)" : "alive");
        if (fwp) { const auto rs = fwp->stats(); std::printf("receive: %llu packets (%llu empty), dbc breaks %llu, lost cycles %llu, rejected %llu\n", (unsigned long long)rs.packets, (unsigned long long)rs.empty, (unsigned long long)rs.dbc_breaks, (unsigned long long)rs.lost_cycles, (unsigned long long)rs.rejected);
                   if (fwp->duplex()) { const auto t = fwp->tx_stats(); std::printf("duplex transmit: %llu packets (%llu with data, %llu frames), refused %llu, gap-filled %llu, dropped %llu, started %s\n", (unsigned long long)t.packets, (unsigned long long)t.data_packets, (unsigned long long)t.frames, (unsigned long long)t.refused, (unsigned long long)t.filled, (unsigned long long)t.dropped, t.started ? "yes" : "NO"); } }
        std::puts("ch   rms dBFS   peak dBFS");
        for (int c = 0; c < channels; ++c) std::printf("%2d   %8.1f   %8.1f%s\n", c, dbfs(st.rms(size_t(c))), dbfs(st.peak[size_t(c)]), c == 0 ? "   (RX1 I)" : c == 1 ? "   (RX1 Q)" : c < 4 ? "   (RX2 I/Q or mic: expected silent without RX2)" : "");
        double rest = -200; for (int c = 2; c < channels; ++c) rest = std::max(rest, dbfs(st.rms(size_t(c))));
        std::printf("\ncheck: channels 2..%d %s (loudest %.1f dBFS; expect near silent while receiving)\n", channels - 1, rest < -80 ? "silent" : "NOT silent", rest);
        const auto a = analyze_iq(iq, rate);
        if (a.peak_dbfs < -90) std::puts("check: no signal on ch0/ch1 (silent). Is the radio out of standby, tuned, with an antenna or generator on ANT1?");
        else if (a.image_valid) std::printf("check: ch0/ch1 strongest line at %+.0f Hz (%.1f dBFS), mirror image %.1f dBFS, image rejection %.1f dB -> %s\n", a.peak_hz, a.peak_dbfs, a.image_dbfs, a.image_rejection_db,
                                       a.image_rejection_db > 30 ? "true quadrature pair" : "NOT a clean quadrature pair (Q missing, duplicated, or no single carrier)");
        else std::puts("check: no off-centre carrier to judge quadrature (tune a known carrier a few kHz from the VFO and repeat)");
        std::puts("note: the sign of the line (above/below the VFO) settles the I/Q mirror question: compare with firesideiq --mirror on the saved file.");
        if (!out.empty()) { dsp::write_wav_f32(out, rate, channels, rec); std::printf("wrote %s\n", out.c_str()); }
        if (sf.is_open()) { wav_header(got); sf.close(); if (!sf) throw std::runtime_error("writing " + stream + " failed"); std::printf("wrote %s (%zu frames of I/Q)\n", stream.c_str(), got); }
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "firesidecapture: %s\n", e.what()); return 1; }
}
