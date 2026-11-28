// One radio session over the FireWire mailbox: connect (read-only, proves the byte order) -> identify (read-only) -> start (reviewed start-up writes, then the Tuner is armed)
// -> tune -> shutdown (reviewed shutdown). Used by the `firesidecli` CLI and the GUI so the write orchestration exists exactly once.
// Writes need allow_write; every write goes through run_startup/run_steps/Tuner, never anything else. State machine: Disconnected -> Connected -> Started.
#pragma once
#include <optional>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include "fireside/cli.h"            // BusHandle
#include "fireside/client.h"
#include "fireside/identify.h"
#include "fireside/pal.h"
#include "fireside/startup.h"
#include "fireside/preamp.h"
#include "fireside/rxant.h"
#include "fireside/txlines.h"
#include "fireside/tune.h"

namespace fireside { class IPcmOut; }
namespace fireside::txc { class TxControl; using PcmFactory = std::function<std::unique_ptr<fireside::IPcmOut>(double rate)>; }   // tx_control.h (not included here: it pulls in eeprom.h)

namespace fireside {
struct SessionOptions { std::string path; std::string byte_order = "auto"; bool allow_write = false; double boot_wait_s = 10.0; double if_offset_hz = kRadioIfOffsetHz; };   // if_offset_hz: DDS = dial - this (0 = DDS on the dial)

class RadioSession {
public:
    using Opener = std::function<std::unique_ptr<BusHandle>(const std::string& path)>;
    enum class State { Disconnected, Connected, Started };
    explicit RadioSession(Opener open, std::function<void(double)> sleep = {});
    // Read-only. Throws std::runtime_error (local controller / cannot open), StartupError (byte order not proven; nothing was written).
    void connect(const SessionOptions& opt);
    Identity identify();                                            // read-only; requires Connected or Started
    // Requires Connected + allow_write (SafetyError otherwise). freq_mhz is the DIAL: the reviewed start-up tunes the DDS to dial - if_offset_hz,
    // waits kSettleBeforeAudioS, then arms the Tuner (same offset) on the dial.
    Identity start(double freq_mhz, const std::string& rx_ant, const LogFn& log = {});
    TuneResult tune(double dial_hz);                                // NotArmed unless Started
    // Voice: the radio's mic input gain (0..-128 dB, -128 = muted; the one reviewed codec write, fireside/codec.h) and the PTT bits
    // (READ_PTT, read-only: bit 3 mic PTT, bit 2 rear RCA PTT). Both need a Started session with writes; false / nullopt otherwise or on a fault.
    bool set_mic_gain_db(int db, std::string* error = nullptr);
    std::optional<uint32_t> read_ptt();
    // The KEY jack and PTT contacts (bit 0 dot, 1 dash, 2 rear RCA PTT, 3 mic PTT), read-only. event=true: the PAL EVENT register (one quadlet read,
    // no firmware command: fast enough to poll every few ms for CW); false: the READ_PTT command (the same bitmap). Needs a Started session.
    std::optional<uint32_t> read_keys(bool event = true);
    std::optional<uint32_t> read_pa_adc(int channel);               // READ_PA_ADC (read-only; txm::kAdcSupply / kAdcTemp): needs a Started session; nullopt otherwise or on a fault
    bool pump();                                                    // sends a coalesced pending retune if due; call from a timer
    PreampResult set_preamp(bool on);                               // receive preamp on/off through RxPreamp (SET_TRX_PREAMP 0/1 only); NotArmed unless Started
    bool preamp_on() const;
    AntResult set_rx_antenna(RxAnt a);                              // switch the receive antenna through RxAntenna (SET_RX1_ANT 1..4 only); NotArmed unless Started; Unchanged writes nothing
    AntResult reassert_rx_antenna();                    // the current RX antenna written again (once the radio has settled after power-up); NotArmed without one
    std::optional<RxAnt> rx_antenna() const;                        // what the radio is set to now (the start-up's choice until changed)
    // The rear TX lines as band switches (txlines.h). The table is kept across restarts; while Started the lines follow apply_tx_lines(dial) and every
    // over sets them first. shutdown() switches every used line off; disconnect() writes nothing.
    void configure_tx_lines(const LineConfig& c);
    TxLines::Result apply_tx_lines(double dial_hz);                 // NotArmed unless Started
    std::array<int, kTxLines> tx_lines_state() const;               // -1 unknown, 0 off, 1 on
    // Reviewed shutdown (MOX off, standby, PA bias off, confirmed consumed). Requires allow_write; valid from Connected or Started. On failure the state is kept and the error rethrown.
    void shutdown(const LogFn& log = {});
    void disconnect();                                              // drops everything; does NOT write (call shutdown() first if the radio is started)
    // The transmit controller (tx_control.h), created on first use while Started; null otherwise. shutdown() / disconnect() disarm it first (a running tune is
    // aborted and waited for, so it unkeys before the radio goes to standby).
    std::shared_ptr<txc::TxControl> transmit(txc::PcmFactory pcm);
    std::shared_ptr<txc::TxControl> transmit_if_any() const { std::lock_guard<std::mutex> l(m_); return tx_; }
    State state() const { std::lock_guard<std::mutex> l(m_); return state_; }
    std::shared_ptr<Tuner> tuner() { std::lock_guard<std::mutex> l(m_); return tuner_; }      // shared: stays valid even if the session restarts/disconnects meanwhile
    bool allow_write() const { std::lock_guard<std::mutex> l(m_); return opt_.allow_write; }
    double if_offset_hz() const { std::lock_guard<std::mutex> l(m_); return opt_.if_offset_hz; }   // the receiver must use the same IF offset
    ByteOrder byte_order() const { std::lock_guard<std::mutex> l(m_); return order_; }
private:
    Opener open_; std::function<void(double)> sleep_;
    mutable std::mutex m_; State state_ = State::Disconnected; SessionOptions opt_; ByteOrder order_ = ByteOrder::Big;
    std::unique_ptr<BusHandle> handle_; std::unique_ptr<PalMailbox> mb_; std::unique_ptr<Flex> fx_; std::shared_ptr<Tuner> tuner_; std::shared_ptr<RxPreamp> preamp_; std::shared_ptr<RxAntenna> rxant_; std::shared_ptr<TxLines> lines_; LineConfig line_cfg_; std::shared_ptr<txc::TxControl> tx_;   // a Tuner handed out by tuner() may outlive the session's Flex: disconnect() disarms it first (under the Tuner's own lock, so an in-flight write finishes), and a disarmed Tuner never touches its Flex again
    IdentifyOptions ident_opts() const;
};
}  // namespace fireside
