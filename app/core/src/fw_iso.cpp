// Direct FireWire receive of the radio's stream. See fw_iso.h for what is written to the radio (three DICE registers, restored) and why.
#include "fireside/fw_iso.h"
#include "fireside/dsp.h"
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <linux/firewire-cdev.h>
#include <linux/firewire-constants.h>
#include <poll.h>
#include <stdexcept>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>
#include "fireside/pal.h"

namespace fireside {
namespace {
std::vector<uint8_t> be4(uint32_t v) { return {uint8_t(v >> 24), uint8_t(v >> 16), uint8_t(v >> 8), uint8_t(v)}; }
uint32_t rd_be(const uint8_t* p) { return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3]; }
void default_sleep(double s) { std::this_thread::sleep_for(std::chrono::duration<double>(s)); }
constexpr uint64_t kFlexOui = 0x001C2D;                   // FlexRadio Systems (the radio's GUID is 001c2d04007fffff)
}  // namespace

int dice_rate_index(unsigned rate) { return rate == 48000 ? 2 : rate == 96000 ? 4 : rate == 192000 ? 6 : -1; }
std::vector<unsigned> dice_supported_rates(uint32_t caps) {
    std::vector<unsigned> r;
    for (unsigned rate : {48000u, 96000u, 192000u}) if (caps & (1u << dice_rate_index(rate))) r.push_back(rate);
    return r;
}

// ---------------------------------------------------------------- DiceStreamControl (the only writer)
DiceStreamControl::DiceStreamControl(IBus& radio, Sleep sleep) : bus_(radio), sleep_(sleep ? std::move(sleep) : Sleep(default_sleep)) {}
uint32_t DiceStreamControl::rq(uint64_t a) { auto v = bus_.read(a, 4); if (v.size() < 4) throw FwError("DICE read: short reply"); return rd_be(v.data()); }
void DiceStreamControl::wq(uint64_t a, uint32_t v) { bus_.write(a, be4(v)); }

DiceStreamState DiceStreamControl::read_state() {
    DiceStreamState s;
    const uint32_t g = rq(kDiceBase) * 4, t = rq(kDiceBase + 8) * 4;          // section offsets are in quadlets
    if (g == 0 || g >= 0x1000 || t == 0 || t >= 0x1000) throw FwError("DICE: implausible section offsets (not a DICE device?)");
    s.global = kDiceBase + g; s.tx = kDiceBase + t;
    s.clock_select = rq(s.global + kDiceGlobalClockSelect); s.enable = rq(s.global + kDiceGlobalEnable); s.status = rq(s.global + kDiceGlobalStatus);
    s.sample_rate = rq(s.global + kDiceGlobalSampleRate); s.clock_caps = rq(s.global + kDiceGlobalClockCaps);
    s.tx_iso = rq(s.tx + kDiceTxStream0 + kDiceTxIso); s.tx_audio = rq(s.tx + kDiceTxStream0 + kDiceTxAudio); s.tx_midi = rq(s.tx + kDiceTxStream0 + kDiceTxMidi);
    return s;
}

void DiceStreamControl::start(unsigned rate, int ch, int rx_ch) {
    if (active_) throw std::logic_error("DiceStreamControl: already started");
    const int idx = dice_rate_index(rate);
    if (idx < 0) throw std::invalid_argument("direct stream: rate must be 48000, 96000 or 192000 Hz");
    if (ch < 0 || ch > 62) throw std::invalid_argument("direct stream: iso channel must be 0..62");
    DiceStreamState s = read_state();
    if (!(s.clock_caps & (1u << idx))) throw std::invalid_argument("direct stream: the radio does not offer " + std::to_string(rate) + " Hz");
    stale_note_.clear();
    if (s.enable != 0) {
        // A stream left running by a run that died without closing it (killed, crashed, a failed shutdown)? The kernel freed that run's channel when
        // it exited, and our receiver has just been given the first free one. If the radio is sending on exactly the channel WE now hold, nobody else
        // can own that stream (snd-dice or another program keeps its channel allocated while streaming): put the radio back to idle and go on.
        // Any other case is refused before any write, as before.
        uint64_t rxs = 0; uint32_t rx_iso = kDiceNoChannel;
        try { const uint32_t r = rq(kDiceBase + 16) * 4; if (r != 0 && r < 0x1000 && rq(kDiceBase + r) >= 1) { rxs = kDiceBase + r; rx_iso = rq(rxs + kDiceRxStream0 + kDiceRxIso); } }
        catch (const std::exception&) { rxs = 0; }
        const bool ours = s.tx_iso == uint32_t(ch) && (rx_iso == kDiceNoChannel || (rx_ch >= 0 && rx_iso == uint32_t(rx_ch)));
        if (!ours) throw IoError("the radio is already streaming (snd-dice/ALSA or another program has it): close that first" + std::string(s.tx_iso == uint32_t(ch) ? " (or open the two-way stream: an earlier two-way run may have left it running)" : ""));
        wq(s.global + kDiceGlobalEnable, 0);
        wq(s.tx + kDiceTxStream0 + kDiceTxIso, kDiceNoChannel);
        if (rxs && rx_iso != kDiceNoChannel) wq(rxs + kDiceRxStream0 + kDiceRxIso, kDiceNoChannel);
        stale_note_ = "cleared a stream left running on channel " + std::to_string(ch) + " by an earlier run that did not close it";
        s = read_state();
        if (s.enable != 0) throw IoError("the radio is still streaming after clearing a stale stream");
    }
    if (s.tx_audio < 2 || s.tx_audio > 32) throw IoError("the radio reports " + std::to_string(s.tx_audio) + " transmit audio channels (expected 8)");
    if (rx_ch >= 0) {                                                                  // duplex: the radio's RX[0] (host -> radio), read before any write
        if (rx_ch > 62) throw std::invalid_argument("direct stream: transmit iso channel must be 0..62");
        const uint32_t r = rq(kDiceBase + 16) * 4;
        if (r == 0 || r >= 0x1000) throw IoError("the radio reports no RX (host to radio) stream section");
        rx_ = kDiceBase + r;
        if (rq(rx_) < 1) throw IoError("the radio has no RX (host to radio) stream");
        rx_orig_iso_ = rq(rx_ + kDiceRxStream0 + kDiceRxIso); rx_audio_ = rq(rx_ + kDiceRxStream0 + kDiceRxAudio);
        if (rx_audio_ < 2 || rx_audio_ > 32) throw IoError("the radio reports " + std::to_string(rx_audio_) + " RX audio channels (expected 8)");
    }
    orig_ = s; channel_ = ch; rx_channel_ = rx_ch; wrote_ = true; stop_note_.clear();
    try {
        wq(s.global + kDiceGlobalClockSelect, (uint32_t(idx) << 8) | (s.clock_select & 0xFF));   // keep the clock source (internal)
        bool locked = false;
        for (int i = 0; i < 20 && !locked; ++i) { sleep_(0.05); DiceStreamState n = read_state(); locked = n.locked() && n.status_rate_index() == idx; }
        if (!locked) throw IoError("the radio's clock did not lock at " + std::to_string(rate) + " Hz within 1 s");
        wq(s.tx + kDiceTxStream0 + kDiceTxIso, uint32_t(ch));
        if (rx_channel_ >= 0) wq(rx_ + kDiceRxStream0 + kDiceRxIso, uint32_t(rx_channel_));   // duplex: before ENABLE, as snd-dice
        wq(s.global + kDiceGlobalEnable, 1);
        active_ = true;
    } catch (...) { stop(); throw; }
}

void DiceStreamControl::stop() noexcept {
    if (!wrote_) return;
    wrote_ = false; active_ = false;
    try {
        const DiceStreamState now = read_state();
        if (now.tx_iso != uint32_t(channel_) && now.tx_iso != orig_.tx_iso) { stop_note_ = "another program took over the radio's stream; left it alone"; return; }
        if (now.enable != orig_.enable) wq(orig_.global + kDiceGlobalEnable, orig_.enable);
        if (now.tx_iso != orig_.tx_iso) wq(orig_.tx + kDiceTxStream0 + kDiceTxIso, orig_.tx_iso);
        if (rx_channel_ >= 0 && rq(rx_ + kDiceRxStream0 + kDiceRxIso) != rx_orig_iso_) wq(rx_ + kDiceRxStream0 + kDiceRxIso, rx_orig_iso_);
        if (now.clock_select != orig_.clock_select) {
            wq(orig_.global + kDiceGlobalClockSelect, orig_.clock_select);
            // Wait (up to 1 s) for the clock to settle back, so ALSA/snd-dice opening right after us finds it locked (measured: ~0.1 s).
            bool settled = false;
            for (int i = 0; i < 20 && !settled; ++i) { sleep_(0.05); const DiceStreamState n = read_state(); settled = n.locked() && n.status_rate_index() == int((orig_.clock_select >> 8) & 0xF); }
            if (!settled) stop_note_ = "the radio's clock had not re-locked at its original rate 1 s after restoring";
        }
    } catch (const std::exception& e) { stop_note_ = std::string("restoring the radio's stream registers failed: ") + e.what(); }
}

// ---------------------------------------------------------------- AmdtpDecoder
AmdtpDecoder::AmdtpDecoder(int channels, bool dual_wire) : ch_(channels), dual_(dual_wire) {
    if (channels < 1 || channels > 32) throw std::invalid_argument("AmdtpDecoder: channels 1..32");
}
void AmdtpDecoder::packet(const uint8_t* p, size_t len, int cycle, std::vector<int32_t>& out) {
    ++st_.packets;
    if (cycle >= 0) {
        if (last_cycle_ >= 0) { const int d = (cycle - last_cycle_ + 8000) % 8000; if (d != 1) { ++st_.cycle_gaps; st_.lost_cycles += uint64_t(d == 0 ? 0 : d - 1); } }
        last_cycle_ = cycle;
    }
    auto reject = [&](const std::string& why) { ++st_.rejected; st_.last_reject = why; next_dbc_ = -1; };
    if (len < 8) { reject("packet shorter than a CIP header"); return; }
    const uint32_t q0 = rd_be(p), q1 = rd_be(p + 4);
    if ((q0 >> 30) != 0 || (q1 >> 30) != 2) { reject("not a two-quadlet CIP header"); return; }
    if (((q1 >> 24) & 0x3F) != 0x10) { reject("not AM824 (FMT != 0x10)"); return; }
    const uint32_t dbs = (q0 >> 16) & 0xFF, dbc = q0 & 0xFF, fdf = (q1 >> 16) & 0xFF;
    if (len == 8 || fdf == 0xFF) {                                       // empty / NO-DATA packet: carries the DBC of the next data packet
        ++st_.empty;
        if (next_dbc_ >= 0 && int(dbc) != next_dbc_) { ++st_.dbc_breaks; next_dbc_ = int(dbc); }
        return;
    }
    if (dbs != expected_dbs()) { reject("DBS " + std::to_string(dbs) + ", expected " + std::to_string(expected_dbs())); return; }
    if ((len - 8) % (4 * dbs) != 0) { reject("payload is not a whole number of data blocks"); return; }
    st_.dbs = dbs; st_.fdf = fdf;
    const size_t nb = (len - 8) / (4 * dbs);
    if (next_dbc_ >= 0 && int(dbc) != next_dbc_) ++st_.dbc_breaks;
    next_dbc_ = int((dbc + nb) & 0xFF);
    const uint8_t* d = p + 8; const int per = dual_ ? 2 : 1;
    const size_t base = out.size(); out.resize(base + nb * size_t(per) * size_t(ch_));
    int32_t* o = out.data() + base;
    for (size_t b = 0; b < nb; ++b, d += 4 * dbs)
        for (int t = 0; t < per; ++t)
            for (int c = 0; c < ch_; ++c) {
                const uint8_t* w = d + 4 * (dual_ ? 2 * c + t : c);     // dual-wire: slot 2c = channel c at t, 2c+1 = at t+1
                *o++ = int32_t(uint32_t(w[1]) << 24 | uint32_t(w[2]) << 16 | uint32_t(w[3]) << 8);   // 24-bit sample -> S32, as snd-dice does
            }
    st_.blocks += nb; st_.frames += nb * uint64_t(per);
}

// ---------------------------------------------------------------- AmdtpEncoder / ReplayScheduler (transmit direction)
AmdtpEncoder::AmdtpEncoder(int channels, bool dual_wire, uint8_t fdf, uint8_t source_node) : ch_(channels), dual_(dual_wire), fdf_(fdf), sid_(uint8_t(source_node & 0x3F)) {
    if (channels < 1 || channels > 32) throw std::invalid_argument("AmdtpEncoder: channels 1..32");
}
void AmdtpEncoder::packet(int blocks, uint16_t syt, const int32_t* frames, uint8_t h[8], std::vector<uint8_t>& payload) {
    const bool empty = blocks <= 0; const uint32_t dbs = this->dbs();
    const uint32_t q0 = uint32_t(sid_) << 24 | (dbs & 0xFF) << 16 | dbc_;
    const uint32_t q1 = 0x80000000u | 0x10u << 24 | uint32_t(empty ? 0xFF : fdf_) << 16 | (empty ? kSytNoInfo : syt);
    for (int i = 0; i < 4; ++i) { h[i] = uint8_t(q0 >> (24 - 8 * i)); h[4 + i] = uint8_t(q1 >> (24 - 8 * i)); }
    payload.clear(); if (empty) return;
    payload.resize(size_t(blocks) * dbs * 4);
    uint8_t* o = payload.data(); const int per = frames_per_block();
    for (int b = 0; b < blocks; ++b) {
        for (int t = 0; t < per; ++t) for (int c = 0; c < ch_; ++c) {
            const int32_t v = frames ? frames[(size_t(b) * size_t(per) + size_t(t)) * size_t(ch_) + size_t(c)] : 0;
            uint8_t* w = o + 4 * (dual_ ? 2 * c + t : c);
            const uint32_t q = 0x40000000u | (uint32_t(v) >> 8);
            w[0] = uint8_t(q >> 24); w[1] = uint8_t(q >> 16); w[2] = uint8_t(q >> 8); w[3] = uint8_t(q);
        }
        uint8_t* m = o + 4 * (dbs - 1); m[0] = 0x80; m[1] = m[2] = m[3] = 0;           // the MIDI quadlet: no data
        o += 4 * dbs;
    }
    dbc_ = uint8_t(dbc_ + blocks);
}
uint16_t syt_shift(uint16_t syt, int cycles) {
    if (syt == kSytNoInfo) return syt;
    const int cyc = ((int(syt >> 12) + cycles) % 16 + 16) % 16;
    return uint16_t(cyc << 12 | (syt & 0x0FFF));
}
void ReplayScheduler::received(int cycle, int blocks, uint16_t syt, std::vector<OutPacket>& out) {
    const int target = cycle_add(cycle, lead_);
    if (next_ >= 0) {
        const int ahead = cycle_diff(target, next_);
        if (ahead < 0) { ++dropped_; return; }                                     // a repeat or a jump back: ignore it
        if (ahead > kMaxGap) { broken_ = true; return; }                            // > 100 ms of receive missing: the stream is broken (the owner restarts it)
        for (int i = 0; i < ahead; ++i) { out.push_back({cycle_add(next_, i), 0, kSytNoInfo}); ++filled_; }   // missing receive cycles: NO-DATA, so every cycle has a packet
    }
    out.push_back({target, blocks, syt_shift(syt, lead_)}); next_ = cycle_add(target, 1);
}

size_t amdtp_max_payload(unsigned rate, int channels) {
    const bool dual = dice_dual_wire(rate); const unsigned wire = dual ? rate / 2 : rate;
    const size_t syt = wire <= 48000 ? 8 : wire <= 96000 ? 16 : 32, dbs = size_t(channels * (dual ? 2 : 1) + 1);
    return 8 + 4 * dbs * syt;
}
uint32_t iso_bandwidth_units(size_t max_payload) { return uint32_t(12 + ((max_payload + 3) & ~size_t(3))); }   // header + 2 CRCs, bytes at S400 = S1600 quadlet units

// ---------------------------------------------------------------- firewire-cdev receiver
namespace {
static_assert(sizeof(fw_cdev_create_iso_context) == 32 && sizeof(fw_cdev_queue_iso) == 24 && sizeof(fw_cdev_start_iso) == 16 && sizeof(fw_cdev_allocate_iso_resource) == 24, "cdev iso layouts");
class CdevIsoReceiver : public IIsoReceiver {
public:
    static constexpr size_t kPackets = 2048, kChunk = 32, kSlot = 2048, kHeader = 8;   // 256 ms of packets in the ring, an interrupt every 4 ms
    CdevIsoReceiver(const std::string& node, size_t max_payload) : node_(node) {
        if (max_payload > kSlot) throw std::invalid_argument("iso receiver: packet larger than a buffer slot");
        fd_ = ::open(node.c_str(), O_RDWR);
        if (fd_ < 0) throw FwError(node + ": " + std::strerror(errno) + (errno == EACCES ? " (install the udev rule: system/setup-system.sh)" : ""));
        try { setup(max_payload); } catch (...) { teardown(); throw; }
    }
    ~CdevIsoReceiver() override { stop(); teardown(); }
    int channel() const override { return channel_; }
    std::string describe() const override { return node_ + " iso channel " + std::to_string(channel_); }
    void start() override {
        for (size_t c = 0; c < kPackets / kChunk; ++c) queue_chunk(c);
        fw_cdev_start_iso s{}; s.cycle = -1; s.sync = 0; s.tags = FW_CDEV_ISO_CONTEXT_MATCH_ALL_TAGS; s.handle = ctx_;
        if (::ioctl(fd_, FW_CDEV_IOC_START_ISO, &s) != 0) throw FwError(node_ + ": START_ISO: " + std::strerror(errno));
        running_ = true;
    }
    size_t poll(double timeout_s, const IsoPacketFn& fn) override {
        pollfd p{fd_, POLLIN, 0};
        const int r = ::poll(&p, 1, int(timeout_s * 1000));
        if (r < 0) { if (errno == EINTR) return 0; throw FwError(node_ + ": poll: " + std::strerror(errno)); }
        if (r == 0) return 0;
        if (p.revents & (POLLHUP | POLLERR)) throw FwError(node_ + ": the FireWire controller went away");
        alignas(8) uint8_t ev[16384];
        const ssize_t n = ::read(fd_, ev, sizeof ev);
        if (n < 0) { if (errno == EAGAIN || errno == EINTR) return 0; throw FwError(node_ + ": read: " + std::strerror(errno)); }
        const auto* common = reinterpret_cast<const fw_cdev_event_common*>(ev);
        if (common->type == FW_CDEV_EVENT_BUS_RESET) throw FwError("FireWire bus reset: the direct stream stopped (start it again)");
        if (common->type == FW_CDEV_EVENT_ISO_RESOURCE_DEALLOCATED) throw FwError("the FireWire channel was taken away (bus reset?)");
        if (common->type != FW_CDEV_EVENT_ISO_INTERRUPT) return 0;
        const auto* e = reinterpret_cast<const fw_cdev_event_iso_interrupt*>(ev);
        const uint8_t* h = reinterpret_cast<const uint8_t*>(e->header);
        const size_t count = e->header_length / kHeader; size_t done = 0;
        for (size_t i = 0; i < count; ++i, h += kHeader) {
            const uint32_t iso = rd_be(h), ts = rd_be(h + 4);           // per packet: [iso header][timestamp] (measured byte order)
            const size_t len = std::min<size_t>(iso >> 16, kSlot);
            fn(buf_ + (done_ % kPackets) * kSlot, len, ts);
            ++done_; ++done;
            if (done_ % kChunk == 0) queue_chunk(((done_ / kChunk) - 1) % (kPackets / kChunk));
        }
        return done;
    }
    void stop() noexcept override {
        if (running_) { fw_cdev_stop_iso s{}; s.handle = ctx_; ::ioctl(fd_, FW_CDEV_IOC_STOP_ISO, &s); running_ = false; }
    }
private:
    std::string node_; int fd_ = -1; int channel_ = -1; uint32_t ctx_ = 0; uint8_t* buf_ = nullptr; bool running_ = false; uint64_t done_ = 0;
    void setup(size_t max_payload) {
        fw_cdev_event_bus_reset reset{}; fw_cdev_get_info info{}; info.version = 5; info.bus_reset = reinterpret_cast<uintptr_t>(&reset);
        if (::ioctl(fd_, FW_CDEV_IOC_GET_INFO, &info) != 0) throw FwError(node_ + ": GET_INFO: " + std::strerror(errno));
        if (reset.node_id != reset.local_node_id) throw FwError(node_ + " is not the local controller node");
        fw_cdev_allocate_iso_resource a{}; a.closure = 1; a.channels = ((1ULL << 63) - 1) & ~1ULL; a.bandwidth = iso_bandwidth_units(max_payload);
        if (::ioctl(fd_, FW_CDEV_IOC_ALLOCATE_ISO_RESOURCE, &a) != 0) throw FwError(node_ + ": ALLOCATE_ISO_RESOURCE: " + std::strerror(errno));
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (channel_ < 0) {
            if (std::chrono::steady_clock::now() > end) throw FwError("no answer from the FireWire resource manager");
            pollfd p{fd_, POLLIN, 0}; if (::poll(&p, 1, 100) <= 0) continue;
            alignas(8) uint8_t ev[4096]; if (::read(fd_, ev, sizeof ev) <= 0) continue;
            const auto* c = reinterpret_cast<const fw_cdev_event_common*>(ev);
            if (c->type != FW_CDEV_EVENT_ISO_RESOURCE_ALLOCATED) continue;
            const auto* r = reinterpret_cast<const fw_cdev_event_iso_resource*>(ev);
            if (r->handle != a.handle) continue;
            if (r->channel < 0 || r->bandwidth <= 0) throw FwError("the FireWire bus has no free isochronous channel/bandwidth");
            channel_ = r->channel;
        }
        fw_cdev_create_iso_context c{}; c.type = FW_CDEV_ISO_CONTEXT_RECEIVE; c.header_size = kHeader; c.channel = uint32_t(channel_); c.speed = SCODE_400; c.closure = 2;
        if (::ioctl(fd_, FW_CDEV_IOC_CREATE_ISO_CONTEXT, &c) != 0) throw FwError(node_ + ": CREATE_ISO_CONTEXT: " + std::strerror(errno));
        ctx_ = c.handle;
        void* m = ::mmap(nullptr, kPackets * kSlot, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (m == MAP_FAILED) throw FwError(node_ + ": mmap: " + std::strerror(errno));
        buf_ = static_cast<uint8_t*>(m);
    }
    void queue_chunk(size_t c) {
        uint32_t ctl[kChunk];
        for (size_t i = 0; i < kChunk; ++i) ctl[i] = FW_CDEV_ISO_PAYLOAD_LENGTH(kSlot) | FW_CDEV_ISO_HEADER_LENGTH(kHeader) | (i == kChunk - 1 ? FW_CDEV_ISO_INTERRUPT : 0);
        fw_cdev_queue_iso q{}; q.packets = reinterpret_cast<uintptr_t>(ctl); q.data = reinterpret_cast<uintptr_t>(buf_ + c * kChunk * kSlot); q.size = sizeof ctl; q.handle = ctx_;
        if (::ioctl(fd_, FW_CDEV_IOC_QUEUE_ISO, &q) < 0) throw FwError(node_ + ": QUEUE_ISO: " + std::strerror(errno));
    }
    void teardown() noexcept {
        if (buf_) { ::munmap(buf_, kPackets * kSlot); buf_ = nullptr; }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }                        // closing frees the context and the channel/bandwidth
    }
};
}  // namespace

std::unique_ptr<IIsoReceiver> open_cdev_iso_receiver(const std::string& node, size_t max_payload) { return std::make_unique<CdevIsoReceiver>(node, max_payload); }
// ---------------------------------------------------------------- firewire-cdev transmitter
namespace {
class CdevIsoTransmitter : public IIsoTransmitter {
public:
    static constexpr size_t kPackets = 512, kSlot = 2048;  // 64 ms of packets; each in its own slot of the mmap'ed buffer
    CdevIsoTransmitter(const std::string& node, size_t max_payload) : node_(node), max_(max_payload) {
        if (max_payload > kSlot) throw std::invalid_argument("iso transmitter: packet larger than a buffer slot");
        fd_ = ::open(node.c_str(), O_RDWR);
        if (fd_ < 0) throw FwError(node + ": " + std::strerror(errno));
        try { setup(); } catch (...) { teardown(); throw; }
    }
    ~CdevIsoTransmitter() override { stop(); teardown(); }
    int channel() const override { return channel_; }
    std::string describe() const override { return node_ + " iso transmit channel " + std::to_string(channel_); }
    size_t queued() const override { return in_flight_; }
    bool has_room() const override { return in_flight_ + 1 <= kPackets - 1; }
    bool queue(const uint8_t h[8], const uint8_t* payload, size_t len) override {
        if (len > max_ || in_flight_ + 1 > kPackets - 1) return false;
        const size_t slot = next_ % kPackets;
        if (len) std::memcpy(buf_ + slot * kSlot, payload, len);
        // one descriptor: control word + the 8 header bytes; the payload is read from this packet's slot
        struct { uint32_t control; uint32_t header[2]; } d;
        d.control = FW_CDEV_ISO_PAYLOAD_LENGTH(uint32_t(len)) | FW_CDEV_ISO_HEADER_LENGTH(8) | FW_CDEV_ISO_TAG(1) | FW_CDEV_ISO_SY(0) | ((next_ % 16) == 15 ? FW_CDEV_ISO_INTERRUPT : 0);
        std::memcpy(d.header, h, 8);
        fw_cdev_queue_iso q{}; q.packets = reinterpret_cast<uintptr_t>(&d); q.data = reinterpret_cast<uintptr_t>(buf_ + slot * kSlot); q.size = sizeof d; q.handle = ctx_;
        if (::ioctl(fd_, FW_CDEV_IOC_QUEUE_ISO, &q) < 0) throw FwError(node_ + ": QUEUE_ISO (transmit): " + std::strerror(errno));
        ++next_; ++in_flight_; return true;
    }
    void start(int cycle15) override {
        fw_cdev_start_iso s{}; s.cycle = cycle15 & 0x7FFF; s.sync = 0; s.tags = 0; s.handle = ctx_;
        if (::ioctl(fd_, FW_CDEV_IOC_START_ISO, &s) != 0) throw FwError(node_ + ": START_ISO (transmit): " + std::strerror(errno));
        running_ = true;
    }
    void reap() override {
        for (;;) {
            pollfd p{fd_, POLLIN, 0}; const int r = ::poll(&p, 1, 0);
            if (r <= 0) return;
            if (p.revents & (POLLHUP | POLLERR)) throw FwError(node_ + ": the FireWire controller went away");
            alignas(8) uint8_t ev[16384]; const ssize_t n = ::read(fd_, ev, sizeof ev);
            if (n <= 0) return;
            const auto* c = reinterpret_cast<const fw_cdev_event_common*>(ev);
            if (c->type == FW_CDEV_EVENT_BUS_RESET) throw FwError("FireWire bus reset: the transmit stream stopped");
            if (c->type == FW_CDEV_EVENT_ISO_RESOURCE_DEALLOCATED) throw FwError("the FireWire transmit channel was taken away (bus reset?)");
            if (c->type != FW_CDEV_EVENT_ISO_INTERRUPT) continue;
            const auto* e = reinterpret_cast<const fw_cdev_event_iso_interrupt*>(ev);
            const size_t done = e->header_length / 4;                       // a timestamp per packet sent since the last event
            in_flight_ -= std::min(in_flight_, done);
            if (done) last_cycle_ = int(rd_ts(reinterpret_cast<const uint8_t*>(e->header) + 4 * (done - 1)) & 0x1FFF);
        }
    }
    void stop() noexcept override { if (running_) { fw_cdev_stop_iso s{}; s.handle = ctx_; ::ioctl(fd_, FW_CDEV_IOC_STOP_ISO, &s); running_ = false; } }
    int last_sent_cycle() const { return last_cycle_; }
private:
    std::string node_; size_t max_; int fd_ = -1, channel_ = -1, last_cycle_ = -1; uint32_t ctx_ = 0; uint8_t* buf_ = nullptr; bool running_ = false; uint64_t next_ = 0; size_t in_flight_ = 0;
    static uint32_t rd_ts(const uint8_t* p) { uint32_t v; std::memcpy(&v, p, 4); return v; }   // the kernel writes timestamps in host order
    void setup() {
        fw_cdev_event_bus_reset reset{}; fw_cdev_get_info info{}; info.version = 5; info.bus_reset = reinterpret_cast<uintptr_t>(&reset);
        if (::ioctl(fd_, FW_CDEV_IOC_GET_INFO, &info) != 0) throw FwError(node_ + ": GET_INFO: " + std::strerror(errno));
        if (reset.node_id != reset.local_node_id) throw FwError(node_ + " is not the local controller node");
        fw_cdev_allocate_iso_resource a{}; a.closure = 1; a.channels = ((1ULL << 63) - 1) & ~1ULL; a.bandwidth = iso_bandwidth_units(max_ + 8);
        if (::ioctl(fd_, FW_CDEV_IOC_ALLOCATE_ISO_RESOURCE, &a) != 0) throw FwError(node_ + ": ALLOCATE_ISO_RESOURCE: " + std::strerror(errno));
        const auto end = std::chrono::steady_clock::now() + std::chrono::seconds(2);
        while (channel_ < 0) {
            if (std::chrono::steady_clock::now() > end) throw FwError("no answer from the FireWire resource manager");
            pollfd p{fd_, POLLIN, 0}; if (::poll(&p, 1, 100) <= 0) continue;
            alignas(8) uint8_t ev[4096]; if (::read(fd_, ev, sizeof ev) <= 0) continue;
            const auto* c = reinterpret_cast<const fw_cdev_event_common*>(ev);
            if (c->type != FW_CDEV_EVENT_ISO_RESOURCE_ALLOCATED) continue;
            const auto* r = reinterpret_cast<const fw_cdev_event_iso_resource*>(ev);
            if (r->handle != a.handle) continue;
            if (r->channel < 0 || r->bandwidth <= 0) throw FwError("the FireWire bus has no free isochronous channel/bandwidth for transmit");
            channel_ = r->channel;
        }
        fw_cdev_create_iso_context c{}; c.type = FW_CDEV_ISO_CONTEXT_TRANSMIT; c.header_size = 4; c.channel = uint32_t(channel_); c.speed = SCODE_400; c.closure = 3;
        if (::ioctl(fd_, FW_CDEV_IOC_CREATE_ISO_CONTEXT, &c) != 0) throw FwError(node_ + ": CREATE_ISO_CONTEXT (transmit): " + std::strerror(errno));
        ctx_ = c.handle;
        void* m = ::mmap(nullptr, kPackets * kSlot, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0);
        if (m == MAP_FAILED) throw FwError(node_ + ": mmap (transmit): " + std::strerror(errno));
        buf_ = static_cast<uint8_t*>(m);
    }
    void teardown() noexcept {
        if (buf_) { ::munmap(buf_, kPackets * kSlot); buf_ = nullptr; }
        if (fd_ >= 0) { ::close(fd_); fd_ = -1; }
    }
};
}  // namespace
std::unique_ptr<IIsoTransmitter> open_cdev_iso_transmitter(const std::string& node, size_t max_payload) { return std::make_unique<CdevIsoTransmitter>(node, max_payload); }


// ---------------------------------------------------------------- FwIsoPcm
namespace {
std::mutex g_duplex_m; FwIsoPcm* g_duplex = nullptr; uint64_t g_duplex_gen = 0; std::string g_close_note, g_open_note;     // the running duplex stream (at most one), for open_fw_iso_out()
}  // namespace
FwIsoPcm::FwIsoPcm(IBus& radio, std::unique_ptr<IIsoReceiver> rx, unsigned rate, FwIsoOptions opt, DiceStreamControl::Sleep sleep, std::unique_ptr<IIsoTransmitter> tx)
    : bus_(radio), rx_(std::move(rx)), rate_(rate), opt_(std::move(opt)), tx_(std::move(tx)) { begin(std::move(sleep)); }
FwIsoPcm::FwIsoPcm(std::unique_ptr<FwDevice> bus, std::unique_ptr<IIsoReceiver> rx, unsigned rate, FwIsoOptions opt, std::unique_ptr<IIsoTransmitter> tx)
    : owned_bus_(std::move(bus)), bus_(*owned_bus_), rx_(std::move(rx)), rate_(rate), opt_(std::move(opt)), tx_(std::move(tx)) { begin({}); }
FwIsoPcm::~FwIsoPcm() { close(); }

void FwIsoPcm::begin(DiceStreamControl::Sleep sleep) {
    if (dice_rate_index(rate_) < 0) throw std::invalid_argument("direct stream: rate must be 48000, 96000 or 192000 Hz");
    if (!rx_) throw std::invalid_argument("direct stream: no receiver");
    dice_ = std::make_unique<DiceStreamControl>(bus_, std::move(sleep));
    rx_->start();
    try { dice_->start(rate_, rx_->channel(), tx_ ? tx_->channel() : -1); } catch (...) { rx_->stop(); dice_.reset(); throw; }
    { std::lock_guard<std::mutex> g(g_duplex_m); g_open_note = dice_->stale_note(); }
    channels_ = int(dice_->original().tx_audio);
    if (tx_) {
        const int out_ch = int(dice_->rx_audio()); if (out_ch != channels_) { dice_->stop(); rx_->stop(); throw IoError("duplex: the radio's RX stream has " + std::to_string(out_ch) + " channels, its TX stream " + std::to_string(channels_)); }
        uint8_t sid = 0; if (auto* fw = dynamic_cast<FwDevice*>(&bus_)) sid = uint8_t(fw->local_node_id() & 0x3F);
        enc_ = std::make_unique<AmdtpEncoder>(channels_, dice_dual_wire(rate_), uint8_t(dice_dual_wire(rate_) ? 0x04 : rate_ == 96000 ? 0x04 : 0x02), sid);
        sched_ = std::make_unique<ReplayScheduler>(opt_.tx_lead_cycles);
    }
    try {
        if (tx_) for (auto& r : out_) r.buf.assign(size_t(std::max(256.0, opt_.out_ring_seconds * rate_)) * size_t(channels_), 0);
        dec_ = std::make_unique<AmdtpDecoder>(channels_, dice_dual_wire(rate_));
        ring_.assign(size_t(std::max(0.05, opt_.ring_seconds) * rate_) * size_t(channels_), 0);
        th_ = std::thread([this] { worker(); });
    } catch (...) { dice_->stop(); rx_->stop(); throw; }                       // the radio's stream registers are restored; nothing was published
    if (tx_) { std::lock_guard<std::mutex> g(g_duplex_m); g_duplex = this; ++g_duplex_gen; }   // last: open_fw_iso_out()/input() only ever see a running stream
}

void FwIsoPcm::worker() {
    std::vector<int32_t> tmp; tmp.reserve(size_t(channels_) * 4096);
    const size_t cap = ring_.size() / size_t(channels_);
    auto last_data = std::chrono::steady_clock::now();
    while (!quit_) {
        tmp.clear();
        try {
            rx_->poll(0.05, [&](const uint8_t* p, size_t n, uint32_t ts) { dec_->packet(p, n, int(ts & 0x1FFF), tmp); if (tx_) send_for(p, n, ts); });
            if (tx_) { tx_->reap(); if (sched_->broken()) throw IoError("duplex: the receive stream had a gap of more than 100 ms (transmit timing lost)"); }
        } catch (const std::exception& e) { std::lock_guard<std::mutex> l(m_); error_ = e.what(); stats_ = dec_->stats(); cv_.notify_all(); tap_cv_.notify_all(); return; }
        const auto now = std::chrono::steady_clock::now();
        std::lock_guard<std::mutex> l(m_);
        stats_ = dec_->stats();
        if (tmp.empty()) {
            if (std::chrono::duration<double>(now - last_data).count() > opt_.stall_seconds) { error_ = "no data from the radio for " + std::to_string(int(opt_.stall_seconds)) + " s (stream stopped, or another program took it over)"; cv_.notify_all(); tap_cv_.notify_all(); return; }
            continue;
        }
        last_data = now;
        const size_t frames = tmp.size() / size_t(channels_);
        if (tap_ch_ >= 0 && tap_ch_ < channels_ && !tap_.empty()) {               // the input tap (e.g. the mic, ch7)
            const size_t cap = tap_.size();
            for (size_t f = 0; f < frames; ++f) { if (tap_count_ == cap) { tap_head_ = (tap_head_ + 1) % cap; --tap_count_; } tap_[(tap_head_ + tap_count_) % cap] = float(double(tmp[f * size_t(channels_) + size_t(tap_ch_)]) / 2147483648.0); ++tap_count_; }
            tap_cv_.notify_all();
        }
        for (size_t f = 0; f < frames; ++f) {
            if (count_ == cap) { head_ = (head_ + 1) % cap; --count_; ++overflows_; }      // reader too slow: drop the oldest
            const size_t at = (head_ + count_) % cap;
            std::copy_n(tmp.data() + f * size_t(channels_), channels_, ring_.data() + at * size_t(channels_));
            ++count_;
        }
        cv_.notify_all();
    }
}

size_t FwIsoPcm::read(int32_t* out, size_t frames, double timeout_s) {
    std::unique_lock<std::mutex> l(m_);
    cv_.wait_for(l, std::chrono::duration<double>(std::max(0.0, timeout_s)), [&] { return count_ > 0 || !error_.empty() || closed_; });
    if (count_ == 0) { if (!error_.empty()) throw IoError("direct FireWire stream: " + error_); return 0; }
    const size_t cap = ring_.size() / size_t(channels_), n = std::min(frames, count_);
    for (size_t f = 0; f < n; ++f) { std::copy_n(ring_.data() + ((head_ + f) % cap) * size_t(channels_), channels_, out + f * size_t(channels_)); }
    head_ = (head_ + n) % cap; count_ -= n;
    return n;
}

uint64_t FwIsoPcm::xruns() const { std::lock_guard<std::mutex> l(m_); return overflows_ + stats_.lost_cycles; }
AmdtpStats FwIsoPcm::stats() const { std::lock_guard<std::mutex> l(m_); return stats_; }
std::string FwIsoPcm::describe() const {
    return "FLEX-5000 direct FireWire stream, " + std::to_string(rate_) + " Hz, " + std::to_string(channels_) + " ch (" + (rx_ ? rx_->describe() : std::string("closed")) + ")";
}
void FwIsoPcm::close() {
    { std::lock_guard<std::mutex> l(m_); if (closed_) return; closed_ = true; }
    { std::lock_guard<std::mutex> g(g_duplex_m); if (g_duplex == this) { g_duplex = nullptr; ++g_duplex_gen; } }   // open_fw_iso_out() writers fail from now on
    { std::unique_lock<std::mutex> l(m_); cv_.notify_all(); tap_cv_.notify_all(); tap_cv_.wait(l, [&] { return tap_readers_ == 0; }); }   // tap readers wake, leave
    quit_ = true; if (th_.joinable()) th_.join();
    if (tx_) tx_->stop();                                    // our packets stop; the radio's RX input is pointed away from us by dice_->stop()
    if (rx_) rx_->stop();                                    // stop listening first, then tell the radio to stop sending and restore it
    if (dice_) dice_->stop();
    { std::lock_guard<std::mutex> g(g_duplex_m); g_close_note = dice_ ? dice_->last_stop_note() : std::string(); }
    cv_.notify_all(); tap_cv_.notify_all();
}

// ---------------------------------------------------------------- duplex: the transmit side
void FwIsoPcm::send_for(const uint8_t* p, size_t len, uint32_t ts) {
    if (len < 8) return;
    const uint32_t q0 = rd_be(p), q1 = rd_be(p + 4), dbs = (q0 >> 16) & 0xFF, fdf = (q1 >> 16) & 0xFF;
    const bool empty = len == 8 || fdf == 0xFF || dbs == 0;
    const int blocks = empty ? 0 : int((len - 8) / (4 * dbs)); const uint16_t syt = uint16_t(q1 & 0xFFFF);
    outq_.clear(); sched_->received(int(ts & 0x1FFF), blocks, syt, outq_);
    for (const OutPacket& o : outq_) {
        if (!tx_->has_room()) { std::lock_guard<std::mutex> l(m_); ++txs_.refused; continue; }   // before anything is consumed: the DBC and the rings stay continuous
        const size_t nf = size_t(o.blocks) * size_t(enc_->frames_per_block());
        frames_.assign(nf * size_t(channels_), 0);
        if (nf) {
            std::lock_guard<std::mutex> l(m_);
            for (int k = 0; k < 2; ++k) {                                              // [0] transmit: ch0/1 only; [1] audio: ch2..7 only
                OutRing& r = out_[k]; const size_t cap = r.buf.size() / size_t(channels_), take = std::min(nf, r.count);
                const int c0 = k == 0 ? 0 : 2, c1 = k == 0 ? 2 : channels_;
                for (size_t f = 0; f < take; ++f) { const int32_t* src = r.buf.data() + ((r.head + f) % cap) * size_t(channels_); int32_t* dst = frames_.data() + f * size_t(channels_); for (int c = c0; c < c1; ++c) dst[c] = src[c]; }
                r.head = (r.head + take) % cap; r.count -= take;
                if (k == 0 && take < nf && r.written > 0) txs_.underrun_frames += nf - take;   // the transmitter is feeding us but fell behind: silence for the rest
            }
            txs_.frames += nf; ++txs_.data_packets;
        }
        uint8_t h[8]; enc_->packet(o.blocks, o.syt, nf ? frames_.data() : nullptr, h, payload_);
        if (!tx_->queue(h, payload_.data(), payload_.size())) { std::lock_guard<std::mutex> l(m_); ++txs_.refused; continue; }
        { std::lock_guard<std::mutex> l(m_); ++txs_.packets; txs_.filled = sched_->filled(); txs_.dropped = sched_->dropped(); }
        if (!txs_.started) {                                                          // the first packet: start on its cycle (+ seconds from the timestamp)
            const int rx_cycle = int(ts & 0x1FFF), sec = int((ts >> 13) & 0x7);
            const int sec_out = (sec + (rx_cycle + sched_->lead()) / 8000) & 3;
            tx_->start(sec_out << 13 | o.cycle);
            std::lock_guard<std::mutex> l(m_); txs_.started = true;
        }
    }
}
size_t FwIsoPcm::write_out(const int32_t* x, size_t frames, OutKind kind) {
    std::lock_guard<std::mutex> l(m_);
    if (!tx_ || closed_) return 0;
    OutRing& r = out_[kind == OutKind::Tx ? 0 : 1];
    const size_t cap = r.buf.size() / size_t(channels_), n = std::min(frames, cap - r.count);
    for (size_t f = 0; f < n; ++f) std::copy_n(x + f * size_t(channels_), channels_, r.buf.data() + ((r.head + r.count + f) % cap) * size_t(channels_));
    r.count += n; r.written += n; return n;
}
double FwIsoPcm::out_latency_s() const {
    std::lock_guard<std::mutex> l(m_);
    const double ring = out_[0].buf.empty() ? 0.0 : double(out_[0].buf.size() / size_t(channels_)) / rate_;   // the writer keeps it about full
    return ring + double(opt_.tx_lead_cycles) / 8000.0 + 0.000479;                 // + the replay lead + the radio's transfer delay (~479 us)
}
void FwIsoPcm::set_input_tap(int ch, size_t ring) {
    std::lock_guard<std::mutex> l(m_); tap_ch_ = ch; tap_.assign(ch >= 0 ? std::max<size_t>(ring ? ring : size_t(rate_ * 0.5), 1024) : 0, 0.0f); tap_head_ = tap_count_ = 0;
}
void FwIsoPcm::tap_hold() { std::lock_guard<std::mutex> l(m_); ++tap_readers_; }
size_t FwIsoPcm::read_tap(float* x, size_t n, double timeout_s) {
    std::unique_lock<std::mutex> l(m_);
    struct Leave { FwIsoPcm* s; ~Leave() { if (s->tap_readers_ > 0) --s->tap_readers_; s->tap_cv_.notify_all(); } } leave{this};   // runs before l unlocks
    tap_cv_.wait_for(l, std::chrono::duration<double>(std::max(0.0, timeout_s)), [&] { return tap_count_ > 0 || !error_.empty() || closed_; });
    if (!error_.empty()) throw IoError("direct FireWire stream: " + error_);
    if (closed_) throw IoError("the direct FireWire stream was closed");
    const size_t k = std::min(n, tap_count_), cap = tap_.size();
    for (size_t i = 0; i < k; ++i) x[i] = tap_[(tap_head_ + i) % cap];
    tap_head_ = (tap_head_ + k) % cap; tap_count_ -= k; return k;
}
FwIsoTxStats FwIsoPcm::tx_stats() const { std::lock_guard<std::mutex> l(m_); return txs_; }

namespace {
class FwIsoOut : public IPcmOut {
public:
    FwIsoOut(FwIsoPcm* s, uint64_t gen, FwIsoPcm::OutKind k) : s_(s), gen_(gen), kind_(k), ch_(s->channels()), rate_(s->rate()), lat_(s->out_latency_s()) {}
    int channels() const override { return ch_; }
    double rate() const override { return rate_; }
    double latency_s() const override { return lat_; }
    std::string describe() const override { return kind_ == FwIsoPcm::OutKind::Tx ? "direct FireWire transmit (duplex)" : "direct FireWire audio out (duplex)"; }
    size_t write(const int32_t* x, size_t frames) override {
        std::lock_guard<std::mutex> g(g_duplex_m);
        if (g_duplex != s_ || g_duplex_gen != gen_) throw IoError("the direct FireWire stream was closed");
        return s_->write_out(x, frames, kind_);
    }
private:
    FwIsoPcm* s_; uint64_t gen_; FwIsoPcm::OutKind kind_; int ch_; double rate_, lat_;
};
}  // namespace
std::unique_ptr<IPcmOut> open_fw_iso_out(FwIsoPcm::OutKind kind) {
    std::lock_guard<std::mutex> g(g_duplex_m);
    if (!g_duplex) throw IoError("no duplex direct FireWire stream is running");
    return std::make_unique<FwIsoOut>(g_duplex, g_duplex_gen, kind);
}
namespace {
class FwIsoInput : public AudioTap {                       // the tap at the stream rate -> 48 kHz
public:
    FwIsoInput(FwIsoPcm* s, uint64_t gen, int ch) : s_(s), gen_(gen), ch_(ch) { if (s->rate() != 48000) rs_ = std::make_unique<dsp::RationalResampler>(s->rate(), 48000.0); }
    ~FwIsoInput() override { std::lock_guard<std::mutex> g(g_duplex_m); if (g_duplex == s_ && g_duplex_gen == gen_) s_->set_input_tap(-1); }
    std::string describe() const override { return "the radio's input ch" + std::to_string(ch_) + (ch_ == 7 ? " (mic socket)" : ""); }
    size_t read(float* x, size_t n, double timeout_s) override {
        while (pend_.size() < n) {
            const size_t want = rs_ ? std::max<size_t>(64, size_t(double(n - pend_.size()) * s_rate() / 48000.0) + 8) : n - pend_.size();
            raw_.resize(want); size_t got;
            { std::lock_guard<std::mutex> g(g_duplex_m); if (g_duplex != s_ || g_duplex_gen != gen_) throw IoError("the direct FireWire stream was closed"); s_->tap_hold(); }
            got = s_->read_tap(raw_.data(), want, timeout_s);                                    // releases the hold; close() waits for it
            if (got == 0) break;
            if (!rs_) { pend_.insert(pend_.end(), raw_.begin(), raw_.begin() + long(got)); continue; }
            in_.resize(got); for (size_t i = 0; i < got; ++i) in_[i] = dsp::cf(raw_[i], 0.0f);
            out_.clear(); rs_->process(in_.data(), got, out_); for (auto& v : out_) pend_.push_back(v.real());
        }
        const size_t k = std::min(n, pend_.size()); std::copy_n(pend_.begin(), k, x); pend_.erase(pend_.begin(), pend_.begin() + long(k)); return k;
    }
private:
    double s_rate() const { return s_->rate(); }
    FwIsoPcm* s_; uint64_t gen_; int ch_; std::unique_ptr<dsp::RationalResampler> rs_; std::vector<float> raw_, pend_; std::vector<dsp::cf> in_, out_;
};
}  // namespace
std::unique_ptr<AudioTap> open_fw_iso_input(int ch) {
    std::lock_guard<std::mutex> g(g_duplex_m);
    if (!g_duplex) throw IoError("no duplex direct FireWire stream is running (start receiving on 48 / 96 / 192)");
    if (ch < 0 || ch >= g_duplex->channels()) throw std::invalid_argument("no such input channel");
    g_duplex->set_input_tap(ch);
    return std::make_unique<FwIsoInput>(g_duplex, g_duplex_gen, ch);
}
std::string fw_iso_last_open_note() { std::lock_guard<std::mutex> g(g_duplex_m); return g_open_note; }
std::string fw_iso_last_close_note() { std::lock_guard<std::mutex> g(g_duplex_m); return g_close_note; }
bool fw_iso_duplex_running(double* rate) { std::lock_guard<std::mutex> g(g_duplex_m); if (g_duplex && rate) *rate = g_duplex->rate(); return g_duplex != nullptr; }

// ---------------------------------------------------------------- discovery
namespace {
struct Nodes { std::string local, radio, error; };
Nodes find_nodes(const std::string& dev_dir, const FwIsoOptions& opt) {
    Nodes n{opt.local_node, opt.radio_node, ""};
    if (!n.local.empty() && !n.radio.empty()) return n;
    for (const auto& d : list_fw_devices(dev_dir)) {
        if (d.is_local && n.local.empty()) n.local = d.path;
        else if (!d.is_local && d.guid && (*d.guid >> 40) == kFlexOui && n.radio.empty()) n.radio = d.path;
    }
    // The local node is root-only until the udev rule is installed, so list_fw_devices cannot open it: guess the controller's node.
    if (n.local.empty()) for (int i = 0; i < 8; ++i) { const std::string p = dev_dir + "/fw" + std::to_string(i); if (p != n.radio && ::access(p.c_str(), F_OK) == 0) { n.local = p; break; } }
    if (n.radio.empty()) n.error = "no FLEX radio node found on the FireWire bus (radio off, or the udev rule not installed?)";
    else if (n.local.empty()) n.error = "no local FireWire controller node found";
    return n;
}
}  // namespace

std::unique_ptr<IPcm> open_fw_iso_pcm(unsigned rate, const FwIsoOptions& opt) {
    if (dice_rate_index(rate) < 0) throw std::invalid_argument("direct stream: rate must be 48000, 96000 or 192000 Hz");
    const Nodes n = find_nodes("/dev", opt);
    if (!n.error.empty()) throw FwError(n.error);
    auto bus = std::make_unique<FwDevice>(n.radio);
    const DiceStreamState s = DiceStreamControl(*bus).read_state();          // read-only: size the channel allocation
    auto rx = open_cdev_iso_receiver(n.local, amdtp_max_payload(rate, int(s.tx_audio)));
    std::unique_ptr<IIsoTransmitter> tx; if (opt.duplex) tx = open_cdev_iso_transmitter(n.local, amdtp_max_payload(rate, int(s.tx_audio)) - 8);   // payload only (the CIP header is separate)
    FwIsoOptions o = opt; o.local_node = n.local; o.radio_node = n.radio;
    return std::unique_ptr<IPcm>(new FwIsoPcm(std::move(bus), std::move(rx), rate, o, std::move(tx)));
}

FwIsoProbe probe_fw_iso(const std::string& dev_dir) {
    FwIsoProbe p;
    try {
        const Nodes n = find_nodes(dev_dir, {});
        p.local_node = n.local; p.radio_node = n.radio;
        if (!n.error.empty()) { p.error = n.error; return p; }
        if (::access(n.local.c_str(), R_OK | W_OK) != 0) { p.error = n.local + ": no permission (install the udev rule: system/setup-system.sh)"; return p; }
        FwDevice bus(n.radio);
        const DiceStreamState s = DiceStreamControl(bus).read_state();
        p.rates = dice_supported_rates(s.clock_caps); p.busy = s.enable != 0; p.ok = !p.rates.empty();
        if (p.rates.empty()) p.error = "the radio offers none of 48/96/192 kHz";
    } catch (const std::exception& e) { p.error = e.what(); }
    return p;
}
}  // namespace fireside
