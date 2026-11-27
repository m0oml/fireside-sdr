// WSPR receive: collects two-minute slots of receiver audio and has WSJT-X's wsprd decode each one (a separate program, run with posix_spawn).
// Kept out of decode.h on purpose: this file starts processes, and decode.h is on the transmit-isolation allowlist. Nothing here can transmit
// or touch the radio. The runner can be replaced (tests use a fake, so no test needs wsprd).
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "fireside/decode.h"
#include "fireside/wspr.h"

namespace fireside::decode {
// Runs the decoder on one slot file: true and wsprd's stdout in `out`, or false and the reason in `err`.
using WsprRunner = std::function<bool(const std::string& wav_path, double dial_mhz, const std::string& work_dir, std::string& out, std::string& err)>;
// The real one: `wsprd -f <dial MHz> -a <work_dir> <wav>` (found on PATH), killed after timeout_s.
WsprRunner wsprd_runner(double timeout_s = 90);
// wsprd's full path on PATH, "" if it is not installed.
std::string find_wsprd();

// A spot worth showing and uploading? wsprd's false decodes (from noise, at the bottom of its range) carry random callsigns. Dropped:
//  - a callsign with a prefix no country uses (any SNR; dxcc.h callsign_prefix_known);
//  - a compound call ("B4N/N85JIU", type 2) at -28 dB or weaker;
//  - at -25 dB or weaker, a locator impossible for the callsign's country (dxcc.h callsign_grid_plausible).
// A call wsprd could not resolve ("<...>") passes. `why`: the rule that dropped it.
bool wspr_spot_plausible(const wspr::Spot& s, std::string* why = nullptr);
struct WsprSlot { std::string date, hhmm; double dial_mhz = 0; std::vector<wspr::Spot> spots; bool ok = false; std::string error; std::vector<std::string> dropped; };   // dropped: implausible decodes ("CALL -31: why")
struct WsprOptions {
    double sample_rate = 48000;
    bool wall_clock = false;               // live audio: slots on even UTC minutes. False: the audio starts start_offset_s into a slot
    double start_offset_s = 0;
    double dial_mhz = 14.0956;             // the dial while the slot was heard (set_dial() follows the receiver)
    std::shared_ptr<std::atomic<double>> dial_now;   // optional: read when a slot is decoded (the GUI keeps it at the receiver's dial, MHz); overrides dial_mhz
    std::string work_dir;                  // where slot files go (created); "" = ~/.local/share/fireside/wspr
    WsprRunner runner;                     // empty: wsprd_runner()
    // Each decoded slot (also with no spots, or a failure), on the decode thread: the GUI uploads from here. Must not block for long.
    std::function<void(const WsprSlot&)> on_slot;
    std::function<double()> now;           // wall_clock: the UTC now (tests); empty = the system clock. The audio is timed by a SlotClock (slot_clock.h)
    std::string my_call;                   // our own beacon (heard while transmitting: the stream is duplex) is never listed or uploaded: any spot whose call, or a / part of it, is this
};
class WsprDecoder : public TextDecoder {
public:
    explicit WsprDecoder(WsprOptions o = {});
    ~WsprDecoder() override;                               // waits for a decode in progress
    void process(const float* audio, size_t n, std::string& out) override;   // a slot is decoded in the background after its 114 s are in; lines arrive in a later call
    std::string status() const override;
    void reset() override;
    void set_dial(double mhz);                             // thread-safe; takes effect for the slot being heard
    bool busy() const;
    bool decoder_found() const;                            // false: the default runner and no wsprd on PATH (nothing is decoded; status says so)
    static constexpr double kFileS = 114;                  // what wsprd is given of each slot (the signal ends at 111.6 s)
private:
    struct Impl; std::unique_ptr<Impl> p_;
};
}  // namespace fireside::decode
