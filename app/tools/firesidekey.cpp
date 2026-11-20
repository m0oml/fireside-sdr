// firesidekey: the supervised stage 3 transmit tests (analysis/transmit-stage2.md section 7). Every command is one step, run on purpose by the user.
//   firesidekey --fw /dev/fw1 --backup F.bin atu-report                   no RF: ask the tuner for its relays
//   firesidekey --fw /dev/fw1 --backup F.bin atu-relays IND CAP HIZ        no RF: set the tuner's relays directly, read them back
//   firesidekey --fw /dev/fw1 --backup F.bin bias                         no RF: radio start-up, PA bias pots from the EEPROM, shutdown
//   firesidekey --fw /dev/fw1 --backup F.bin key-silent --freq MHZ [--seconds S] [--rate 48000|96000|192000] [--alsa hw:CARD=MyProduct,DEV=0]
//        (--rate: what snd-dice offers now: /proc/asound/cardN/firewire/formation)
//        start-up, bias, carrier null, TX on ANT1 at MHZ (USB), I/Q stream of ZEROS on ch0/1, key for S s (<= 3) through the TxGate reading
//        the PA meter every 50 ms (unkeys above 5 W or on any error), unkey, shutdown. Expect ~0 W: anything else means carrier leakage or a fault.
//   firesidekey --fw /dev/fw1 --backup F.bin ramp --freq MHZ --confirm-clear [--target W] [--tone HZ] [--no-mirror-q (re-test only: mirrored is right)] [--rate HZ]
//        THE FIRST RF: as key-silent, then a single tone (USB, `tone` Hz above the TX DDS) starting at 1/10 of the EEPROM's 1 W drive, +2 dB every 200 ms,
//        PA meter read every 50 ms. Holds 0.5 s at the target and unkeys. Aborts (unkey at once) on: under 5 mW once the drive reaches the table's 1 W point,
//        SWR > 3 (above 0.5 W), forward power > 1.5 x target, a meter error, the 6 s gate deadline. Output clamped to the EEPROM table's 100 W drive (measured ~10-15 W on 20 m) whatever happens.
//        --confirm-clear is required: listen to the frequency first. --hold S (<= 10) keeps a steady tone at the target, e.g. to find it on another receiver.
//   firesidekey --fw /dev/fw1 --backup F.bin tune --freq MHZ --confirm-clear [--power W] [--swr X] [--k W_PER_DRIVE2] [--memory FILE]
//        ATU full tune from our app (PowerSDR's sequence): tuner init, TX mode CWU, key, carrier at the drive for --power (<= 15 W, as PowerSDR) from the MEASURED
//        calibration (~/.config/fireside/tx-drive.txt, written by `ramp` and the GUI's Measure drive; or --k), SWR threshold + full tune,
//        poll tuner and PA meter, unkey. The gate: 15 s, forward reading <= 30 W, reflected <= 20 W (fixed: the readings swing while the tuner searches); SWR is NOT a stop while the tuner searches (as PowerSDR:
//        atu_tuning skips its SWR protection), which is why the power is limited to 15 W (PowerSDR tunes at 15 W). On a pass the band's relays go to --memory
//        (default ~/.config/fireside/atu-memory.txt).
// Before any write the EEPROM is read and must equal --backup byte for byte (the calibration the steps use is the radio's own).
// The gate's limits for these tests: 3 s, 5 W. Never more.
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <iostream>
#include <fstream>
#include <thread>
#include "fireside/atu.h"
#include "fireside/atu_memory.h"
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>
#include "fireside/cli.h"
#include "fireside/eeprom.h"
#include "fireside/pal.h"
#include "fireside/startup.h"
#include "fireside/tuning.h"
#include "fireside/tx_gate.h"
#include "fireside/tx_out.h"
#include "fireside/tx_control.h"
#include "fireside/tx_setup.h"
#include "fireside/fw_iso.h"
#include "fireside/codec.h"

using namespace fireside;
namespace {
void usage() { std::fputs("usage: firesidekey --fw /dev/fwN --backup TRX.bin (mic-test [--freq MHZ] [--seconds S] (no RF: mic level + PTT) | atu-report | atu-relays IND CAP HIZ | bias | key-silent --freq MHZ [--seconds S] [--rate HZ] [--alsa DEV] |\n              ramp --freq MHZ --confirm-clear [--target W (<= 5, default 2)] [--hold S (<= 10, default 0.5)] [--tone HZ (default 1000)] [--no-mirror-q (re-test only: mirrored is right)] [--rate HZ] [--alsa DEV])\n", stderr); }
void say(const std::string& s) { std::printf("%s\n", s.c_str()); std::fflush(stdout); }
std::vector<uint8_t> read_file(const std::string& p) { std::ifstream in(p, std::ios::binary); if (!in) throw std::runtime_error("cannot read " + p); return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()}; }
void sleep_s(double s) { std::this_thread::sleep_for(std::chrono::duration<double>(s)); }
// Once start-up has run, the reviewed shutdown (MOX 0, standby, PA bias off) runs on every way out, errors included.
struct ShutdownGuard { Flex& fx; bool armed = false; ~ShutdownGuard() { if (armed) { try { run_steps(fx, shutdown_steps(), say); say("radio back in standby"); } catch (const std::exception& e) { std::fprintf(stderr, "SHUTDOWN FAILED: %s\n", e.what()); } } } };
void print_status(const atu::Status& s) {
    auto o = [](const std::optional<int>& v) { return v ? std::to_string(*v) : std::string("?"); };
    say("tuner: ind " + o(s.ind) + " cap " + o(s.cap) + " hiz " + o(s.hiz) + " antenna " + o(s.antenna));
}
}  // namespace

int main(int argc, char** argv) {
    std::string fw, backup, cmd, alsa = "hw:CARD=MyProduct,DEV=0"; double freq = 0, seconds = 1.0, rate = 48000, target_w = 2.0, tone_hz = 1000, hold_s = 0.5, tune_w = 3.0, threshold = 1.5, kcal = 0; std::string memfile; bool confirm = false, mirror_q = true, reset_relays = false; std::vector<std::string> rest;
    for (int i = 1; i < argc; ++i) {
        std::string a = argv[i]; auto next = [&] { if (i + 1 >= argc) throw std::invalid_argument("missing value after " + a); return std::string(argv[++i]); };
        try {
            if (a == "--fw") fw = next(); else if (a == "--backup") backup = next(); else if (a == "--alsa") alsa = next();
            else if (a == "--freq") freq = std::stod(next()); else if (a == "--rate") rate = std::stod(next()); else if (a == "--target") target_w = std::stod(next()); else if (a == "--tone") tone_hz = std::stod(next()); else if (a == "--hold") hold_s = std::stod(next());
            else if (a == "--power") tune_w = std::stod(next()); else if (a == "--swr") threshold = std::stod(next()); else if (a == "--k") kcal = std::stod(next());
            else if (a == "--memory") memfile = next();
            else if (a == "--confirm-clear") confirm = true; else if (a == "--reset-relays") reset_relays = true; else if (a == "--mirror-q") mirror_q = true; else if (a == "--no-mirror-q") mirror_q = false; else if (a == "--seconds") seconds = std::stod(next());
            else if (cmd.empty()) cmd = a; else rest.push_back(a);
        } catch (const std::exception& e) { std::fprintf(stderr, "%s\n", e.what()); usage(); return 2; }
    }
    if (fw.empty() || backup.empty() || cmd.empty()) { usage(); return 2; }
    try {
        const auto want = read_file(backup);
        auto env = real_env(); auto h = env.open_bus(fw);
        if (h->is_local()) throw std::runtime_error(fw + " is the local controller");
        auto pr = probe_order(h->bus()); if (!pr.order) throw std::runtime_error("mailbox byte order not proven; nothing written");
        PalMailbox mb(h->bus(), *pr.order);
        if (!mb.verify(10.0)) throw std::runtime_error("firmware revision implausible; nothing written");
        Safety sf; sf.allow_write = true; Flex fx(mb, sf);

        say("reading the EEPROM (read-only) and comparing with " + backup + " ...");
        const auto img = read_image(fx, "TRX", kTrxLen);               // one pass is enough: any misread byte fails the comparison with the backup below
        if (img != want) throw std::runtime_error("the radio's EEPROM differs from the backup: refusing (run eeprom-dump and compare first)");
        const auto cal = txs::from_image(img); if (!cal.valid) throw std::runtime_error("transmit calibration: " + cal.problem);
        say("EEPROM identical to the backup; PA bias from row 0, " + std::to_string(cal.carrier.size()) + " carrier-null entries");

        if (cmd == "atu-report" || cmd == "atu-relays") {
            atu::Atu a(fx); a.drain();
            if (cmd == "atu-report") { a.request_update(); print_status(a.status()); return 0; }
            if (rest.size() != 3) { usage(); return 2; }
            const int ind = std::stoi(rest[0]), cap = std::stoi(rest[1]); const bool hiz = std::stoi(rest[2]) != 0;
            a.request_update(); say("before:"); print_status(a.status());
            const auto& b = a.status();
            if (b.ind && b.cap && b.hiz && *b.ind == ind && *b.cap == cap && (*b.hiz != 0) == hiz) { say("already set: nothing sent"); return 0; }
            bool ok = a.set_relays(ind, cap, hiz);
            if (!ok && !a.status().ind) { say("no report yet: asking again"); a.request_update(); const auto& t = a.status();
                ok = t.ind && t.cap && t.hiz && *t.ind == ind && *t.cap == cap && (*t.hiz != 0) == hiz; }
            say("after:"); print_status(a.status());
            say(ok ? "OK: the tuner reports the requested relays" : "MISMATCH: the tuner does not report what was set (the direct-set encoding needs another look)");
            return ok ? 0 : 1;
        }
        ShutdownGuard down{fx};
        if (cmd == "mic-test") {                                      // NO RF: the mic input level and the PTT bits (analysis/voice-tx.md stage A)
            run_startup(fx, freq > 0 ? freq : 7.1, "ANT1", say); down.armed = true; sleep_s(kSettleBeforeAudioS);
            auto rd = [&](uint32_t reg) { return uint32_t(codec::read_reg(fx, reg)); };
            std::string regs; for (uint32_t r : {0x07u, 0x0Au, 0x0Bu, 0x0Cu, 0x0Du, 0x0Eu, 0x0Fu, 0x13u, 0x14u, 0x15u, 0x16u, 0x1Bu}) { char b[24]; std::snprintf(b, sizeof b, " %02X=%02X", r, rd(r)); regs += b; }
            say("codec registers (read):" + regs);
            const uint32_t orig = rd(codec::kMicGainReg);
            struct MicRestore { Flex& fx; uint32_t v; ~MicRestore() { try { codec::set_mic_gain_reg(fx, uint8_t((v == 0 || v >= 0x80) ? v : 0x80)); say("mic gain register put back"); } catch (...) {} } } restore{fx, orig};
            codec::set_mic_gain_db(fx, 0);                             // 0 dB, PowerSDR's FLEX-5000 default (the one reviewed codec write: core/src/codec.cpp)
            say("mic gain 0x16: " + std::to_string(orig) + " -> 0 (0 dB). Speak into the mic, press its PTT. Nothing transmits.");
            FwIsoOptions fo; fo.duplex = true; auto pcm = open_fw_iso_pcm(96000, fo);
            const int ch = pcm->channels(); std::vector<int32_t> buf(size_t(ch) * 4096); std::vector<double> sq(size_t(ch), 0.0); size_t n = 0;
            const auto t0 = std::chrono::steady_clock::now(); auto next = t0 + std::chrono::milliseconds(500);
            uint32_t last_ptt = 0xFFFFFFFF;
            while (std::chrono::steady_clock::now() - t0 < std::chrono::duration<double>(seconds > 1 ? seconds : 20)) {
                const size_t got = pcm->read(buf.data(), 4096, 0.1);
                for (size_t f = 0; f < got; ++f) for (int c = 0; c < ch; ++c) { const double v = buf[f * size_t(ch) + size_t(c)] / 2147483648.0; sq[size_t(c)] += v * v; }
                n += got;
                if (std::chrono::steady_clock::now() >= next) {
                    const uint32_t ptt = uint32_t(fx.get("RDAL_OP_READ_PTT", 0)) & 0xF;
                    std::string line = "levels dBFS:"; for (int c = 2; c < ch; ++c) { char b[24]; std::snprintf(b, sizeof b, "  ch%d %6.1f", c, n ? 10 * std::log10(sq[size_t(c)] / double(n) + 1e-20) : -200.0); line += b; }
                    char p[64]; std::snprintf(p, sizeof p, "   PTT bits %X%s", ptt, (ptt & 8) ? " (MIC PTT)" : (ptt & 4) ? " (RCA PTT)" : ""); line += p;
                    say(line); if (ptt != last_ptt) last_ptt = ptt;
                    std::fill(sq.begin(), sq.end(), 0.0); n = 0; next += std::chrono::milliseconds(500);
                }
            }
            pcm.reset(); return 0;                                    // stream closed; the mic register and the shutdown follow (destructors)
        }
        if (cmd == "bias") {
            run_startup(fx, 14.336, "ANT1", say); down.armed = true; sleep_s(kSettleBeforeAudioS);
            txs::write_bias(fx, cal, say);
            uint32_t rb = fx.get("RDAL_OP_PA_POT_GET_RDAC"); char m[64]; std::snprintf(m, sizeof m, "PA_POT_GET_RDAC (read-only) -> 0x%08X", rb); say(m);
            say("done: bias written"); return 0;
        }
        if (cmd == "key-silent") {
            const int band = txm::band_of(freq * 1e6);
            if (band < 0) throw std::invalid_argument("--freq must be inside an amateur band");
            if (!(seconds > 0) || seconds > 3) throw std::invalid_argument("--seconds must be 0..3");
            run_startup(fx, freq, "ANT1", say); down.armed = true; sleep_s(kSettleBeforeAudioS);
            txs::write_bias(fx, cal, say);
            fx.set("RDAL_OP_SET_TX_ANT", 1, 0);
            fx.set("RDAL_OP_SET_TX_DSP_MODE", 1, 0);                    // USB
            fx.set("RDAL_OP_SET_TX_FREQ_TW", double(freq_to_tw(freq)), freq);
            txs::write_carrier_null(fx, cal, freq); fx.sync();
            if (rate != 48000 && rate != 96000 && rate != 192000) throw std::invalid_argument("--rate 48000, 96000 or 192000");
            txo::ToneSource src(rate, 8); src.set_cap(txs::drive_for_watts(cal, band, 1.0)); src.set_target(0.0);
            int rc = 0;
            {
                txo::Stream st(open_alsa_play(alsa, rate, 8, 1024), src);
                sleep_s(0.5);
                if (st.failed()) throw std::runtime_error("the I/Q playback stream failed to start");
                GateConfig gc; gc.max_key_s = seconds + 0.2; gc.max_fwd_w = 5.0; gc.max_swr = 5.0;
                TxGate gate(fx, gc); gate.arm(); std::string why;
                double max_fwd = 0, max_rev = 0;
                if (!gate.key("silent key-down test", seconds, &why)) throw std::runtime_error("gate refused: " + why);
                src.set_keyed(true);                                          // target 0: ch0/1 stay exactly zero
                const auto t0 = std::chrono::steady_clock::now();
                while (gate.keyed()) {
                    try {
                        const int f = int(fx.get("RDAL_OP_READ_PA_ADC", txm::kAdcFwd)), r = int(fx.get("RDAL_OP_READ_PA_ADC", txm::kAdcRev));
                        const auto rd = txm::read(cal.meter, band, f, r); max_fwd = std::max(max_fwd, rd.fwd_w); max_rev = std::max(max_rev, rd.rev_w);
                        char m[120]; std::snprintf(m, sizeof m, "  t=%.2f s  fwd ADC %4d = %6.3f W   rev ADC %4d = %6.3f W", std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count(), f, rd.fwd_w, r, rd.rev_w); say(m);
                        gate.report(rd);
                    } catch (const std::exception& e) { gate.unkey(std::string("meter read failed: ") + e.what()); rc = 1; }
                    gate.tick(); sleep_s(0.05);
                }
                src.set_keyed(false);
                char m[160]; std::snprintf(m, sizeof m, "unkeyed (%s). max forward %.3f W, max reflected %.3f W; stream %llu frames, %llu underruns", gate.last_unkey_reason().c_str(),
                                           max_fwd, max_rev, (unsigned long long)st.frames(), (unsigned long long)st.underruns()); say(m);
                for (auto& l : gate.log()) say("  gate: " + l);
            }
            return rc;
        }
        if (cmd == "ramp" || cmd == "tune") {                         // both through the shared transmit controller (fireside/tx_control.h), as the GUI
            const int band = txm::band_of(freq * 1e6);
            if (band < 0) throw std::invalid_argument("--freq must be inside an amateur band");
            if (!confirm) throw std::invalid_argument("listen to the frequency first, then add --confirm-clear");
            if (rate != 48000 && rate != 96000 && rate != 192000) throw std::invalid_argument("--rate 48000, 96000 or 192000");
            txc::Options o; o.rate = rate; o.tone_hz = tone_hz; o.mirror_q = mirror_q;
            run_startup(fx, freq, "ANT1", say); down.armed = true; sleep_s(kSettleBeforeAudioS);
            txc::TxControl tx(fx, [&](double r) { return open_alsa_play(alsa, r, 8, 1024); });
            tx.arm(img, want, say);
            const char* home = std::getenv("HOME"); const std::string cfg = std::string(home ? home : ".") + "/.config/fireside";
            ::mkdir((std::string(home ? home : ".") + "/.config").c_str(), 0755); ::mkdir(cfg.c_str(), 0755);
            const std::string drive_file = cfg + "/tx-drive.txt";       // this radio's measured drive levels (shared with the GUI); no built-in default
            { std::ifstream in(drive_file); std::string text; if (in) text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()); tx.set_all_k(txc::parse_drive_text(text)); }
            char m[220];
            auto prog = [&](const txc::Progress& p) {
                std::snprintf(m, sizeof m, "  t=%.2f s  drive %.5f (%5.1f dBFS)  fwd %6.3f W  rev %6.3f W  SWR %s", p.t, p.drive, p.drive > 0 ? 20 * std::log10(p.drive) : -999.0,
                              p.rd.fwd_w, p.rd.rev_w, p.rd.swr ? std::to_string(*p.rd.swr).substr(0, 4).c_str() : "-"); say(m); };
            if (cmd == "ramp") {
                auto r = tx.ramp(freq, target_w, o, prog, say, hold_s);
                if (r.reached && r.k > 0) { std::ofstream(drive_file) << txc::drive_text(tx.all_k()); say("saved the " + std::string(txm::band_name(band)) + " drive level to " + drive_file); }
                if (r.unkey_failed) say("!!! THE UNKEY DID NOT GET THROUGH: THE RADIO MAY STILL BE TRANSMITTING. POWER IT OFF NOW.");
                std::snprintf(m, sizeof m, "%s: %s; max forward %.2f W after %.1f s", r.reached ? "target reached" : "aborted", r.stop.c_str(), r.max_fwd_w, r.seconds); say(m);
                if (r.k > 0) { std::snprintf(m, sizeof m, "measured: %.0f W per drive^2  ->  1 W at %.1f dBFS, 5 W at %.1f dBFS", r.k, 20 * std::log10(std::sqrt(1 / r.k)), 20 * std::log10(std::sqrt(5 / r.k))); say(m); }
                return r.reached ? 0 : 1;
            }
            // tune
            std::string path = memfile;
            if (path.empty()) { const char* home = std::getenv("HOME"); const std::string base = std::string(home ? home : ".") + "/.config"; ::mkdir(base.c_str(), 0755); ::mkdir((base + "/fireside").c_str(), 0755); path = base + "/fireside/atu-memory.txt"; }
            { std::ifstream in(path); std::string text; if (in) text.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()); tx.memory() = atu::Memory::from_text(text); }
            if (kcal > 0) tx.set_k(band, kcal);
            if (reset_relays) { atu::Atu a(fx); a.drain(); const bool ok = a.set_relays(0, 0, false); say(ok ? "relays cleared to 0 / 0 / Lo-Z (no RF)" : "relays NOT cleared as asked (continuing)"); }
            auto r = tx.tune(freq, tune_w, threshold, o, [&](const txc::Progress& p) { std::snprintf(m, sizeof m, "  t=%.2f s  fwd %5.1f W  rev %5.1f W", p.t, p.rd.fwd_w, p.rd.rev_w); say(m); }, say);
            if (r.unkey_failed) say("!!! THE UNKEY DID NOT GET THROUGH: THE RADIO MAY STILL BE TRANSMITTING. POWER IT OFF NOW.");
            if (!r.outcome) throw std::runtime_error("tune refused: " + r.why_not);
            const auto& s2 = r.outcome->status; auto oi = [](const std::optional<int>& v) { return v ? std::to_string(*v) : std::string("?"); };
            auto od = [](const std::optional<double>& v, int pr) { if (!v) return std::string("?"); char b2[32]; std::snprintf(b2, sizeof b2, "%.*f", pr, *v); return std::string(b2); };
            const char* res = r.outcome->result == atu::Atu::TuneResult::Pass ? "PASS" : r.outcome->result == atu::Atu::TuneResult::Fail ? "FAIL" : "STOPPED";
            std::snprintf(m, sizeof m, "tune %s after %.1f s%s%s (%s); our meter: max %.1f W, last %.1f W", res, r.outcome->seconds, r.outcome->result == atu::Atu::TuneResult::Fail ? ": " : "",
                          r.outcome->result == atu::Atu::TuneResult::Fail ? atu::fail_text(r.outcome->fail_reason) : "", r.stop.c_str(), r.max_fwd_w, r.last_fwd_w); say(m);
            say("tuner reports: ind " + oi(s2.ind) + " cap " + oi(s2.cap) + " hiz " + oi(s2.hiz) + ", fwd " + od(s2.fwd_w, 2) + " W, rev " + od(s2.rev_w, 2) + " W, SWR " + od(s2.swr, 2) +
                ", threshold " + od(s2.threshold, 1) + ", TX freq " + od(s2.tx_freq_mhz, 3) + " MHz");
            if (r.saved) { std::ofstream(path) << tx.memory().to_text(); say("saved the " + std::string(txm::band_name(band)) + " tune to " + path); }
            if (auto rep = tx.atu_report()) say("relays now (read back, no RF): ind " + oi(rep->ind) + " cap " + oi(rep->cap) + " hiz " + oi(rep->hiz));
            return r.saved ? 0 : 1;
        }
        usage(); return 2;
    } catch (const std::exception& e) {
        std::fprintf(stderr, "firesidekey: %s\n", e.what()); return 1;
    }
}
