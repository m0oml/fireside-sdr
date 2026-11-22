// Direct FireWire receive of the FLEX-5000's audio/I-Q stream (48, 96 or 192 kHz), bypassing snd-dice.
// Why: the Linux snd-dice driver exposes no channels in the radio's 96/192 kHz rate bands (its probe fails), although the radio
// itself streams 8 channels at 192 kHz (measured 2026-11-18, analysis/audio-stream-layout.md "Userspace isochronous receive spike").
//
// The one part that writes is DiceStreamControl, and it writes exactly four DICE sound-chip registers: global CLOCK_SELECT (sample
// rate, internal clock source kept), TX[0] ISOCHRONOUS (which channel the radio sends on), RX[0] ISOCHRONOUS (duplex only: which channel
// the radio listens on for our transmit stream) and global ENABLE. No PAL/RDAL radio opcode is sent from here (it can never key the
// transmitter: MOX only through the TxGate). stop() (also run by the destructor) restores them to what they were.
// Duplex (FwIsoOptions::duplex): the transmit I/Q goes to the radio on our own channel while receive keeps running (analysis/duplex-iso-tx.md).
// tools/tests/test_tune_guard.py checks statically that nothing else in this file writes to the bus.
//
// Stream format (IEC 61883-6 AM824, blocking mode), measured on the radio:
//   48 kHz: DBS 9 (8 audio + 1 MIDI), 8 blocks per data packet, 1 packet in 4 empty (CIP only), FDF 0x02.
//   192 kHz: "dual-wire": FDF 0x04 (96 kHz), DBS 17, 16 blocks per data packet; a block carries two consecutive samples:
//            slot 2c = channel c at time t, slot 2c+1 = channel c at t+1 (I/Q check: image 43 dB down this way, 3 dB the other).
// Samples are delivered as S32 (24-bit value << 8), exactly what snd-dice hands ALSA, so PcmIqSource works unchanged.
#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "fireside/capture.h"
#include "fireside/playback.h"
#include "fireside/firewire.h"

namespace fireside {
// DICE register offsets (linux sound/firewire/dice/dice-interface.h). Section offsets are read from the radio.
inline constexpr uint32_t kDiceGlobalClockSelect = 0x4C, kDiceGlobalEnable = 0x50, kDiceGlobalStatus = 0x54, kDiceGlobalSampleRate = 0x5C, kDiceGlobalClockCaps = 0x64;
inline constexpr uint32_t kDiceTxStream0 = 0x08, kDiceTxIso = 0x00, kDiceTxAudio = 0x04, kDiceTxMidi = 0x08;   // TX section: count, size, then stream 0
inline constexpr uint32_t kDiceNoChannel = 0xFFFFFFFFu;
inline constexpr uint32_t kDiceRxStream0 = 0x08, kDiceRxIso = 0x00, kDiceRxAudio = 0x08, kDiceRxMidi = 0x0C;   // RX section (host -> radio): count, size, then stream 0

int dice_rate_index(unsigned rate);                       // 48000 -> 2, 96000 -> 4, 192000 -> 6; -1 for anything else we do not support
std::vector<unsigned> dice_supported_rates(uint32_t clock_caps);   // of 48/96/192 kHz, the ones the CLOCK_CAPS bits offer
inline bool dice_dual_wire(unsigned rate) { return rate > 96000; }

struct DiceStreamState {
    uint64_t global = 0, tx = 0;                          // absolute section addresses
    uint32_t clock_select = 0, enable = 0, status = 0, sample_rate = 0, clock_caps = 0, tx_iso = 0, tx_audio = 0, tx_midi = 0;
    bool locked() const { return status & 1; }
    int status_rate_index() const { return int((status >> 8) & 0xF); }
};

// THE ONLY WRITER in the direct-stream path. Not thread-safe; owned by FwIsoPcm.
class DiceStreamControl {
public:
    using Sleep = std::function<void(double)>;
    explicit DiceStreamControl(IBus& radio, Sleep sleep = {});
    ~DiceStreamControl() { stop(); }
    DiceStreamControl(const DiceStreamControl&) = delete; DiceStreamControl& operator=(const DiceStreamControl&) = delete;
    DiceStreamState read_state();
    // Sets the clock rate, points TX[0] at iso_channel and enables streaming, then waits up to 1 s for the clock to lock at that rate.
    // Refuses BEFORE any write (std::invalid_argument / IoError) if: the rate is not 48/96/192 kHz or not in the radio's caps, the channel is
    // not 0..62, the radio is already streaming (ENABLE != 0: snd-dice or another program owns it), or TX[0] has fewer than 2 audio channels.
    // Exception: a stream on exactly iso_channel (and RX[0] idle or on rx_iso_channel) is a leftover of a dead run, since the caller holds that
    // channel at the bus manager: it is put back to idle (ENABLE 0, ISOCHRONOUS none) and the start goes on (stale_note()).
    // If anything fails after the first write, it restores before throwing.
    void start(unsigned rate, int iso_channel, int rx_iso_channel = -1);
    // rx_iso_channel >= 0 (duplex): also points RX[0] (host -> radio) at that channel, before ENABLE, as snd-dice does; refused before any
    // write if the radio has no RX stream with 2..32 audio channels. stop() puts RX[0] back too.
    // Restores ENABLE, TX[0] ISOCHRONOUS and CLOCK_SELECT to the values read at start. Idempotent, never throws. If TX[0] no longer
    // points at our channel (another program took the stream over meanwhile), it leaves the radio alone.
    void stop() noexcept;
    bool active() const { return active_; }
    uint32_t rx_audio() const { return rx_audio_; }          // duplex: the radio's RX[0] audio channels (read at start)
    const DiceStreamState& original() const { return orig_; }
    std::string last_stop_note() const { return stop_note_; }
    std::string stale_note() const { return stale_note_; }  // non-empty: start() found and cleared a stream an earlier run left running
private:
    IBus& bus_; Sleep sleep_; bool active_ = false, wrote_ = false; int channel_ = -1, rx_channel_ = -1; DiceStreamState orig_; std::string stop_note_, stale_note_;
    uint64_t rx_ = 0; uint32_t rx_orig_iso_ = kDiceNoChannel, rx_audio_ = 0;
    uint32_t rq(uint64_t addr);
    void wq(uint64_t addr, uint32_t value);               // the single bus write in this file
};

// Pure decoder: CIP/AM824 packet payloads -> interleaved S32 frames of `channels` channels.
struct AmdtpStats {
    uint64_t packets = 0, empty = 0, blocks = 0, frames = 0, cycle_gaps = 0, lost_cycles = 0, dbc_breaks = 0, rejected = 0;
    uint32_t dbs = 0, fdf = 0;
    std::string last_reject;
};
class AmdtpDecoder {
public:
    AmdtpDecoder(int channels, bool dual_wire);           // channels = audio channels per sample time (8 on the radio)
    // payload = the packet's data (CIP header first); cycle = 13-bit bus cycle (timestamp & 0x1FFF), or -1 if unknown. Appends frames.
    void packet(const uint8_t* payload, size_t len, int cycle, std::vector<int32_t>& out);
    const AmdtpStats& stats() const { return st_; }
    int channels() const { return ch_; }
    uint32_t expected_dbs() const { return uint32_t(ch_ * (dual_ ? 2 : 1) + 1); }
private:
    int ch_; bool dual_; AmdtpStats st_; int last_cycle_ = -1; int next_dbc_ = -1;
};

// ---------------------------------------------------------------- transmit direction (host -> radio), analysis/duplex-iso-tx.md
// Pure encoder: S32 frames of `channels` channels -> CIP/AM824 blocking-mode packets for the radio's RX[0] stream.
// Audio quadlet = 0x40 | 24-bit sample (S32 >> 8); one MIDI quadlet per block = 0x80000000 (no data). Dual-wire (192 kHz): two
// consecutive frames per block, channel c at slots 2c and 2c+1. NO-DATA packet (0 blocks): CIP only, FDF 0xFF, SYT 0xFFFF.
inline constexpr uint16_t kSytNoInfo = 0xFFFF;
class AmdtpEncoder {
public:
    AmdtpEncoder(int channels, bool dual_wire, uint8_t fdf, uint8_t source_node);   // fdf: 0x02 (48 kHz) or 0x04 (96 kHz, and 192 kHz dual-wire)
    // Builds one packet of `blocks` data blocks from frames (blocks * frames_per_block() interleaved frames; may be null = silence).
    // header: the 8 CIP bytes; payload: the data blocks (empty for a NO-DATA packet). The data block counter advances by `blocks`.
    void packet(int blocks, uint16_t syt, const int32_t* frames, uint8_t header[8], std::vector<uint8_t>& payload);
    int frames_per_block() const { return dual_ ? 2 : 1; }
    uint32_t dbs() const { return uint32_t(ch_ * frames_per_block() + 1); }
    uint8_t dbc() const { return dbc_; }
private:
    int ch_; bool dual_; uint8_t fdf_, sid_, dbc_ = 0;
};
// SYT presentation time moved by `cycles` bus cycles (its 4-bit cycle field wraps; NO-INFO stays NO-INFO).
uint16_t syt_shift(uint16_t syt, int cycles);
// Media-clock replay (as snd-dice): every packet received at cycle r (N blocks, SYT s) gives one to send at cycle r + lead with N
// blocks and SYT s + lead. Cycles are 13-bit (0..7999) with wrap. Missing receive cycles are filled with NO-DATA packets; a receive
// cycle that is not after the last one scheduled (a repeat, or a jump backwards) is dropped and counted. The packets come out for
// consecutive cycles (the transmitter sends one per cycle); a gap of more than 800 cycles (100 ms) sets broken().
struct OutPacket { int cycle = 0; int blocks = 0; uint16_t syt = kSytNoInfo; };
class ReplayScheduler {
public:
    explicit ReplayScheduler(int lead_cycles) : lead_(lead_cycles) {}
    // A received packet: appends the packets to send (usually one; gap fill first) to `out`.
    void received(int cycle, int blocks, uint16_t syt, std::vector<OutPacket>& out);
    int lead() const { return lead_; }
    int next_cycle() const { return next_; }                // the cycle of the next packet to send (-1 before the first)
    uint64_t filled() const { return filled_; } uint64_t dropped() const { return dropped_; }
    bool broken() const { return broken_; }
    static constexpr int kMaxGap = 800;
private:
    int lead_, next_ = -1; uint64_t filled_ = 0, dropped_ = 0; bool broken_ = false;
};
inline int cycle_add(int c, int d) { return ((c + d) % 8000 + 8000) % 8000; }
inline int cycle_diff(int a, int b) { int d = ((a - b) % 8000 + 8000) % 8000; return d >= 4000 ? d - 8000 : d; }   // a - b in -4000..3999

// Isochronous receive of one channel on the local controller node. The real one uses firewire-cdev on /dev/fwN (is_local).
using IsoPacketFn = std::function<void(const uint8_t* payload, size_t len, uint32_t timestamp)>;
class IIsoReceiver {
public:
    virtual ~IIsoReceiver() = default;
    virtual int channel() const = 0;                       // the channel allocated at the IRM
    virtual void start() = 0;                              // begin listening (before the radio is told to send)
    virtual size_t poll(double timeout_s, const IsoPacketFn& fn) = 0;   // completed packets to fn; 0 if none within timeout. Throws IoError (bus reset, device gone).
    virtual void stop() noexcept = 0;
    virtual std::string describe() const = 0;
};
// Allocates a channel and bandwidth for packets of up to max_payload bytes (freed when the receiver is destroyed). Throws FwError.
std::unique_ptr<IIsoReceiver> open_cdev_iso_receiver(const std::string& local_node, size_t max_payload);
size_t amdtp_max_payload(unsigned rate, int channels);    // CIP header + the largest data packet at this rate
uint32_t iso_bandwidth_units(size_t max_payload);         // IRM allocation units (one quadlet at S1600) for S400

// Isochronous transmit on one channel from the local controller node (the real one: firewire-cdev). One packet per bus cycle, in
// the order queued: the first goes out at the cycle given to start(), each next one a cycle later.
class IIsoTransmitter {
public:
    virtual ~IIsoTransmitter() = default;
    virtual int channel() const = 0;
    // Queues one packet (8 CIP header bytes + payload, payload_len 0 for a NO-DATA packet). false = no room (nothing queued).
    virtual bool queue(const uint8_t header[8], const uint8_t* payload, size_t payload_len) = 0;
    virtual void start(int cycle15) = 0;                   // after the first packets are queued: 2 bits of seconds << 13 | the 13-bit cycle
    virtual void reap() = 0;                               // frees the room of packets sent (non-blocking). Throws IoError (bus reset, device gone)
    virtual size_t queued() const = 0;                     // packets waiting to go out
    virtual bool has_room() const { return true; }        // queue() would accept one more packet (checked before a packet is encoded)
    virtual void stop() noexcept = 0;
    virtual std::string describe() const = 0;
};
std::unique_ptr<IIsoTransmitter> open_cdev_iso_transmitter(const std::string& local_node, size_t max_payload);

// The direct stream as an IPcm: S32 frames of 8 channels at 48/96/192 kHz, filled by a worker thread. Plug into PcmIqSource.
// duplex: also send to the radio (its RX[0] stream) on our own channel, clocked by replaying the radio's packet sequence tx_lead
// cycles later (analysis/duplex-iso-tx.md); the samples come from open_fw_iso_out() (silence when nothing is written).
struct FwIsoOptions { std::string local_node, radio_node; double ring_seconds = 1.0; double stall_seconds = 1.0; bool duplex = false; int tx_lead_cycles = 128; double out_ring_seconds = 0.02; };
struct FwIsoTxStats { uint64_t packets = 0, data_packets = 0, frames = 0, underrun_frames = 0, refused = 0, filled = 0, dropped = 0; bool started = false; };
class FwIsoPcm : public IPcm {
public:
    // Test seam: any bus and receiver. Starts the receiver, then the radio's stream, then the worker. Throws if the radio refuses.
    FwIsoPcm(IBus& radio, std::unique_ptr<IIsoReceiver> rx, unsigned rate, FwIsoOptions opt = {}, DiceStreamControl::Sleep sleep = {}, std::unique_ptr<IIsoTransmitter> tx = {});
    ~FwIsoPcm() override;
    int channels() const override { return channels_; }
    double rate() const override { return double(rate_); }
    size_t read(int32_t* interleaved, size_t frames, double timeout_s) override;   // throws IoError once the stream has failed
    uint64_t xruns() const override;                      // ring overflows + bus cycles lost
    std::string describe() const override;
    AmdtpStats stats() const;
    void close();                                         // stops the worker and restores the radio (idempotent; also the destructor)
    // duplex: transmit samples (interleaved S32, channels() channels). Never blocks: returns how many frames fit (0 = full, try later).
    // kind: Tx = the transmit I/Q (only channels 0/1 of what is written are sent), Audio = speaker / line audio (only channels 2..7 are
    // sent). The two have their own rings and are mixed per frame, so audio can never reach the transmitter's channels.
    enum class OutKind { Tx, Audio };
    size_t write_out(const int32_t* interleaved, size_t frames, OutKind kind = OutKind::Tx);
    double out_latency_s() const;                         // a written frame goes out about this much later
    // Input tap: one input channel (7 = the radio's mic) copied as float at the stream rate into a ring for a reader (voice TX). -1 = off.
    void set_input_tap(int channel, size_t ring_frames = 0);
    size_t read_tap(float* x, size_t n, double timeout_s);   // up to n samples (stream rate); 0 if none within the timeout; throws IoError once failed
    void tap_hold();                                          // a reader announces itself (under the published-stream lock) before read_tap, which releases it;
                                                              // close() waits until no reader is left, so a reader never touches a destroyed stream
    bool duplex() const { return tx_ != nullptr; }
    FwIsoTxStats tx_stats() const;
private:
    std::unique_ptr<FwDevice> owned_bus_;                 // only when opened by open_fw_iso_pcm
    friend std::unique_ptr<IPcm> open_fw_iso_pcm(unsigned, const FwIsoOptions&);
    FwIsoPcm(std::unique_ptr<FwDevice> bus, std::unique_ptr<IIsoReceiver> rx, unsigned rate, FwIsoOptions opt, std::unique_ptr<IIsoTransmitter> tx);
    void send_for(const uint8_t* p, size_t len, uint32_t ts);   // worker: the outgoing packet(s) for one received packet
    void begin(DiceStreamControl::Sleep sleep);
    void worker();
    IBus& bus_; std::unique_ptr<IIsoReceiver> rx_; unsigned rate_; int channels_ = 8; FwIsoOptions opt_;
    std::unique_ptr<DiceStreamControl> dice_; std::unique_ptr<AmdtpDecoder> dec_;
    mutable std::mutex m_; std::condition_variable cv_; std::vector<int32_t> ring_; size_t head_ = 0, count_ = 0; uint64_t overflows_ = 0;
    std::string error_; AmdtpStats stats_; std::thread th_; std::atomic<bool> quit_{false}; bool closed_ = false;
    std::unique_ptr<IIsoTransmitter> tx_; std::unique_ptr<AmdtpEncoder> enc_; std::unique_ptr<ReplayScheduler> sched_; std::vector<OutPacket> outq_; std::vector<uint8_t> payload_; std::vector<int32_t> frames_;
    struct OutRing { std::vector<int32_t> buf; size_t head = 0, count = 0; uint64_t written = 0; };
    OutRing out_[2]; FwIsoTxStats txs_;
    int tap_ch_ = -1; std::vector<float> tap_; size_t tap_head_ = 0, tap_count_ = 0; std::condition_variable tap_cv_; int tap_readers_ = 0;   // guarded by m_                    // [0] transmit I/Q (ch0/1), [1] audio (ch2..7): guarded by m_
};
// duplex: the transmit side of the running direct stream as an IPcmOut (8 channels at the stream's rate), for TxControl. Throws IoError if
// no duplex stream runs; its write() throws IoError once that stream is closed. Writes nothing to the radio itself: the stream's worker
// sends what was written, in time with the radio's own packets.
std::unique_ptr<IPcmOut> open_fw_iso_out(FwIsoPcm::OutKind kind = FwIsoPcm::OutKind::Tx);
// The running duplex stream's input channel `ch` (7 = the radio's mic socket) as 48 kHz mono float, for voice transmit. Same clock as the
// radio's output: no drift. Throws IoError if no duplex stream runs. read() throws IoError once that stream is closed.
class AudioTap {
public:
    virtual ~AudioTap() = default;
    virtual size_t read(float* x, size_t n, double timeout_s) = 0;
    virtual std::string describe() const = 0;
};
std::unique_ptr<AudioTap> open_fw_iso_input(int channel);
bool fw_iso_duplex_running(double* rate = nullptr);
std::string fw_iso_last_open_note();                      // what the last open found: "", or that it cleared a stream an earlier run left running
std::string fw_iso_last_close_note();                     // what the last FwIsoPcm::close() said about restoring the radio ("" = all restored)
// Finds the local node and the FLEX node (vendor 0x001C2D) unless given, then streams at `rate`. Throws FwError / IoError / std::invalid_argument.
std::unique_ptr<IPcm> open_fw_iso_pcm(unsigned rate, const FwIsoOptions& opt = {});
// What the direct path can offer right now, without writing anything: nodes found, radio rates from CLOCK_CAPS, whether it is streaming.
struct FwIsoProbe { bool ok = false; std::string error, local_node, radio_node; std::vector<unsigned> rates; bool busy = false; };
FwIsoProbe probe_fw_iso(const std::string& dev_dir = "/dev");
}  // namespace fireside
