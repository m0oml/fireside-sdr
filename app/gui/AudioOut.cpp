#include "AudioOut.h"
#ifdef FIRESIDE_QT_AUDIO
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#endif
#include <QDateTime>
#include <QElapsedTimer>
#include <deque>
#include <map>
#include <memory>
#include "fireside/fw_iso.h"
#include "fireside/playback.h"
#include <QSettings>
#include <QTimer>
#include <algorithm>
#include <cmath>

bool AudioOut::deviceExists(const QString& device) {
#ifdef FIRESIDE_QT_AUDIO
    for (const auto& d : QMediaDevices::audioOutputs()) if (d.description().contains(device, Qt::CaseInsensitive)) return true;
#else
    (void)device;
#endif
    return false;
}
namespace {
std::vector<AudioOut*>& live() { static std::vector<AudioOut*> v; return v; }        // GUI thread only
QString& pc_out() { static QString d; static bool loaded = false; if (!loaded) { loaded = true; d = QSettings("fireside", "audio").value("pc_output").toString(); } return d; }
}
// The radio's headphone jack: one mixer for every AudioOut that follows the PC output while it is set to kRadioPhones (GUI thread only).
class RadioPhonesMix : public QObject {
public:
    static RadioPhonesMix& get() { static RadioPhonesMix* m = new RadioPhonesMix; return *m; }
    void push(const AudioOut* src, const std::vector<float>& a, float vol) {
        auto& q = srcs_[src]; for (float v : a) q.buf.push_back(v * vol);
        while (q.buf.size() > kMaxQueue) q.buf.pop_front();                                     // a source far ahead: drop its oldest
        if (!t_.isActive()) { clock_.restart(); t_.start(); }
    }
    void forget(const AudioOut* src) { srcs_.erase(src); }
    float peak = 0; long blocks = 0;
private:
    static constexpr size_t kPrime = 48000 / 25, kMaxQueue = 48000 / 4;                         // 40 ms before a source joins; 250 ms at most
    struct Src { std::deque<float> buf; bool primed = false; };
    std::map<const AudioOut*, Src> srcs_; QTimer t_; QElapsedTimer clock_; double owed_ = 0; std::unique_ptr<fireside::RadioAudioOut> out_; qint64 retry_ms_ = 0;
    RadioPhonesMix() { t_.setInterval(10); QObject::connect(&t_, &QTimer::timeout, [this] { tick(); }); }
    void tick() {
        owed_ += clock_.restart() * 48.0; size_t n = size_t(std::min(owed_, 4800.0)); owed_ -= double(n); if (n == 0) return;
        std::vector<float> mix(n, 0.0f); bool any = false;
        for (auto& [k, q] : srcs_) {
            if (!q.primed) { if (q.buf.size() < kPrime) continue; q.primed = true; }
            const size_t m = std::min(n, q.buf.size()); for (size_t i = 0; i < m; ++i) mix[i] += q.buf[i];
            q.buf.erase(q.buf.begin(), q.buf.begin() + long(m)); if (q.buf.empty()) q.primed = false; any = any || m > 0;
        }
        if (!any) { bool empty = true; for (auto& [k, q] : srcs_) empty = empty && q.buf.empty(); if (empty) { t_.stop(); owed_ = 0; } }
        float pk = 0; for (auto& v : mix) { v = std::clamp(v, -1.0f, 1.0f); pk = std::max(pk, std::fabs(v)); }
        peak = pk; ++blocks;
        if (!out_) {
            const qint64 now = QDateTime::currentMSecsSinceEpoch(); if (now < retry_ms_) return; retry_ms_ = now + 500;
            if (!fireside::fw_iso_duplex_running()) return;
            try { out_ = std::make_unique<fireside::RadioAudioOut>(fireside::open_fw_iso_out(fireside::FwIsoPcm::OutKind::Audio), 1, 48000.0); out_->set_volume(1.0f); }   // pair 1 = ch 2/3, the headphone jack
            catch (const std::exception&) { out_.reset(); return; }
        }
        try { out_->push(mix.data(), mix.size()); } catch (const std::exception&) { out_.reset(); }   // the stream closed or restarted: reopen on a later tick
    }
};
float AudioOut::radioPhonesPeakForTests() { return RadioPhonesMix::get().peak; }
long AudioOut::radioPhonesBlocksForTests() { return RadioPhonesMix::get().blocks; }

QString AudioOut::pcOutput() { return pc_out(); }
void AudioOut::setPcOutput(const QString& description) {
    if (description == pc_out()) return;
    pc_out() = description; if (!qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) QSettings("fireside", "audio").setValue("pc_output", description);
    for (auto* o : live()) if (o->follows_pc_) o->open();
}
AudioOut::AudioOut(bool enable, QObject* parent, const QString& device) : QObject(parent), device_(device), follows_pc_(device.isEmpty()), enabled_(enable) {
    live().push_back(this); open();
}
AudioOut::~AudioOut() { auto& v = live(); v.erase(std::remove(v.begin(), v.end(), this), v.end()); RadioPhonesMix::get().forget(this); }
void AudioOut::open() {
    if (!enabled_) return;
#ifdef FIRESIDE_QT_AUDIO
    if (sink_) { sink_->stop(); sink_->deleteLater(); sink_ = nullptr; io_ = nullptr; available_ = false; }
#endif
    to_radio_ = follows_pc_ && pc_out() == kRadioPhones; RadioPhonesMix::get().forget(this);
    if (to_radio_) { available_ = true; return; }                                         // the radio's headphone jack: no PC device
#ifdef FIRESIDE_QT_AUDIO
    const QString want = follows_pc_ ? pc_out() : device_;
    QAudioDevice dev = want.isEmpty() ? QMediaDevices::defaultAudioOutput() : QAudioDevice();
    if (!want.isEmpty()) for (const auto& d : QMediaDevices::audioOutputs()) if (d.description() == want || (!follows_pc_ && d.description().contains(want, Qt::CaseInsensitive))) { dev = d; break; }
    if (dev.isNull() && follows_pc_) dev = QMediaDevices::defaultAudioOutput();               // the chosen output is unplugged: the system default
    if (dev.isNull()) return;
    // Stereo, the same audio on left and right. The channel POSITIONS must be given: with only a count, Qt hands PipeWire "aux0, aux1" (no position)
    // and it plays them on the left only (2026-11-26, the AB13X headset). Front-left / front-right is what every output maps properly.
    QAudioFormat fmt; fmt.setSampleRate(48000); fmt.setChannelConfig(QAudioFormat::ChannelConfigStereo); fmt.setSampleFormat(QAudioFormat::Int16);
    if (!dev.isFormatSupported(fmt)) { fmt.setChannelConfig(QAudioFormat::ChannelConfigMono); if (!dev.isFormatSupported(fmt)) return; }   // a device that only does mono
    channels_ = fmt.channelCount();
    sink_ = new QAudioSink(dev, fmt, this); sink_->setBufferSize(48000 * 2 * channels_ / 4);   // 250 ms
    io_ = sink_->start(); available_ = io_ != nullptr;
    if (available_ && !keep_) { keep_ = new QTimer(this); keep_->setInterval(40); connect(keep_, &QTimer::timeout, this, [this] { keepAlive(); }); keep_->start(); }
#endif
}
void AudioOut::push(const std::vector<float>& audio, float volume) {
    if (to_radio_) { if (enabled_ && !audio.empty()) RadioPhonesMix::get().push(this, audio, volume); return; }
#ifdef FIRESIDE_QT_AUDIO
    if (!available_ || audio.empty()) return;
    const qint64 bytes = qint64(audio.size()) * 2 * channels_;
    if (sink_->bytesFree() < bytes) return;                                               // would grow latency: drop this block
    std::vector<int16_t> pcm(audio.size() * size_t(channels_));
    for (size_t i = 0; i < audio.size(); ++i) { const int16_t v = int16_t(std::lround(std::min(std::max(audio[i] * volume, -1.0f), 1.0f) * 32767.0f)); for (int c = 0; c < channels_; ++c) pcm[i * size_t(channels_) + size_t(c)] = v; }   // the same sample on every channel
    io_->write(reinterpret_cast<const char*>(pcm.data()), bytes); last_push_ms_ = QDateTime::currentMSecsSinceEpoch();
#else
    (void)audio; (void)volume;
#endif
}
void AudioOut::keepAlive() {
#ifdef FIRESIDE_QT_AUDIO
    if (to_radio_ || !available_ || QDateTime::currentMSecsSinceEpoch() - last_push_ms_ < 120) return;       // real audio is flowing
    const qint64 frame = 2 * channels_, queued = sink_->bufferSize() - sink_->bytesFree(), want = 48000 / 10 * frame;   // keep ~100 ms of silence queued
    if (queued >= want) return;
    const qint64 n = std::min<qint64>(want - queued, sink_->bytesFree()) / frame * frame; if (n <= 0) return;
    const std::vector<char> zeros(size_t(n), 0); io_->write(zeros.data(), n);
#endif
}
