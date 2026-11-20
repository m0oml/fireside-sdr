// firesideiq: run the receive chain over a recorded I/Q WAV (e.g. `arecord -c 8 -f S32_LE` from the radio, ch0/ch1 = RX1 I/Q).
// Reads files only; never opens a device. Writes: demodulated audio (WAV), waterfall (PGM) and the last spectrum frame (CSV).
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iostream>
#include "fireside/dsp.h"
#include "fireside/iqsource.h"
using namespace fireside::dsp;

static void usage() {
    std::puts("usage: firesideiq <in.wav> [--mode usb|lsb|am|cw|fm] [--am env|sync|sync-lsb|sync-usb] [--agc off|fast|med|slow] [--nb] [--mirror] [--if-offset HZ] [--fft N] [--chans I,Q] [--width HZ (after --mode)]\n"
              "              [--audio out.wav] [--waterfall out.pgm] [--spectrum out.csv]\n"
              "       firesideiq --make-test <out.wav>   write a 2-ch 48 kHz demo: USB tone 1 kHz above the dial, 50 %-AM carrier, noise (DDS 9 kHz below dial)");
}
int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 2; }
    try {
        if (!std::strcmp(argv[1], "--make-test")) {
            if (argc != 3) { usage(); return 2; }
            auto x = synth_tones(48000, 48000 * 6, {{9000 + 1000, 0.3}, {9000 - 5000, 0.2}, {9000 - 4000, 0.05}, {9000 - 6000, 0.05}}, 0.003, 1);
            std::vector<float> il; for (auto& v : x) { il.push_back(v.real()); il.push_back(v.imag()); }
            write_wav_f32(argv[2], 48000, 2, il); std::printf("wrote %s\n", argv[2]); return 0;
        }
        std::string in = argv[1], audio_out, wf_out, csv_out; ReceiveConfig rc; DemodConfig dc; size_t fft = 4096; int ci = 0, cq = 1;
        for (int i = 2; i < argc; ++i) {
            const std::string a = argv[i]; auto next = [&]() -> const char* { if (i + 1 >= argc) throw std::runtime_error("missing value for " + a); return argv[++i]; };
            if (a == "--mode") { std::string m = next(); dc.mode = m == "usb" ? Mode::USB : m == "lsb" ? Mode::LSB : m == "am" ? Mode::AM : m == "cw" ? Mode::CW : m == "fm" ? Mode::FM : throw std::runtime_error("bad mode"); }
            else if (a == "--agc") { std::string m = next(); dc.agc = m == "off" ? AgcMode::Off : m == "fast" ? AgcMode::Fast : m == "med" ? AgcMode::Med : m == "slow" ? AgcMode::Slow : throw std::runtime_error("bad agc"); }
            else if (a == "--nb") dc.noise_blanker = true;
            else if (a == "--am") { std::string m = next(); dc.am_detector = m == "env" ? AmDetector::Envelope : AmDetector::Sync;
                                    dc.am_sideband = m == "sync-lsb" ? AmSideband::Lower : m == "sync-usb" ? AmSideband::Upper : AmSideband::Both; if (m != "env" && m != "sync" && m != "sync-lsb" && m != "sync-usb") throw std::runtime_error("bad --am"); }
            else if (a == "--mirror") rc.mirror = true;
            else if (a == "--if-offset") rc.if_offset_hz = std::atof(next());
            else if (a == "--fft") fft = size_t(std::atol(next()));
            else if (a == "--width") { const double w = std::atof(next()); const auto pb = fireside::dsp::passband_for(dc.mode, w); dc.lo_hz = pb.first; dc.hi_hz = pb.second; }
            else if (a == "--chans") { if (std::sscanf(next(), "%d,%d", &ci, &cq) != 2) throw std::runtime_error("--chans I,Q"); }
            else if (a == "--audio") audio_out = next(); else if (a == "--waterfall") wf_out = next(); else if (a == "--spectrum") csv_out = next();
            else { usage(); return 2; }
        }
        auto src = fireside::open_iq_file(in, ci, cq); rc.sample_rate = src->sample_rate(); dc.audio_rate = 48000;           // odd sample rates are resampled automatically
        std::printf("%s: %s, mode %d, mirror %s, IF offset %.0f Hz\n", in.c_str(), src->describe().c_str(), int(dc.mode), rc.mirror ? "on" : "off", rc.if_offset_hz);
        dc.input_rate = rc.sample_rate;
        ReceiveChain chain(rc, dc, fft);
        Waterfall wf(fft, 2000); chain.spectrum().on_frame = [&](const std::vector<float>& r) { wf.push(r); };
        std::vector<float> audio;
        { std::vector<fireside::dsp::cf> blk; for (;;) { blk.clear(); if (src->read(blk, 4096) == 0) break; chain.process(blk.data(), blk.size(), audio); } }
        auto& sp = chain.spectrum(); size_t pk = size_t(std::max_element(sp.db().begin(), sp.db().end()) - sp.db().begin());
        std::printf("spectrum: %zu frames, peak %.1f dBFS at %+.0f Hz from the dial; audio: %zu samples\n", sp.frames(), sp.db()[pk], sp.bin_hz(pk), audio.size());
        std::printf("signal in passband: %.1f dBFS (uncalibrated; needs the radio's full-scale dBm for S-units); AGC gain %.1f dB; blanked %zu sample(s)\n",
                    chain.signal_dbfs(), chain.demod().agc_gain_db(), chain.demod().blanked());
        if (dc.mode == Mode::AM && dc.am_detector == AmDetector::Sync) std::printf("sync AM: %s, carrier %+.1f Hz from the dial\n", chain.demod().am_locked() ? "locked" : "NOT locked", chain.demod().am_carrier_hz());
        if (!audio_out.empty()) write_wav_pcm16_mono(audio_out, dc.audio_rate, audio);
        if (!wf_out.empty() && wf.rows()) {
            auto g = wf.to_gray(-120.f, -20.f); std::ofstream o(wf_out, std::ios::binary); o << "P5\n" << wf.width() << " " << wf.rows() << "\n255\n";
            for (size_t r = wf.rows(); r-- > 0;) o.write(reinterpret_cast<const char*>(&g[r * wf.width()]), long(wf.width()));   // oldest at the top
        }
        if (!csv_out.empty()) { std::ofstream o(csv_out); o << "offset_hz,dbfs\n"; for (size_t k = 0; k < sp.size(); ++k) o << sp.bin_hz(k) << "," << sp.db()[k] << "\n"; }
        return 0;
    } catch (const std::exception& e) { std::fprintf(stderr, "firesideiq: %s\n", e.what()); return 1; }
}
