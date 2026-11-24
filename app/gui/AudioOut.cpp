#include "AudioOut.h"
#ifdef FIRESIDE_QT_AUDIO
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#endif
#include <QDateTime>
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
QString AudioOut::pcOutput() { return pc_out(); }
void AudioOut::setPcOutput(const QString& description) {
    if (description == pc_out()) return;
    pc_out() = description; QSettings("fireside", "audio").setValue("pc_output", description);
    for (auto* o : live()) if (o->follows_pc_) o->open();
}
AudioOut::AudioOut(bool enable, QObject* parent, const QString& device) : QObject(parent), device_(device), follows_pc_(device.isEmpty()), enabled_(enable) {
    live().push_back(this); open();
}
AudioOut::~AudioOut() { auto& v = live(); v.erase(std::remove(v.begin(), v.end(), this), v.end()); }
void AudioOut::open() {
#ifdef FIRESIDE_QT_AUDIO
    if (!enabled_) return;
    if (sink_) { sink_->stop(); sink_->deleteLater(); sink_ = nullptr; io_ = nullptr; available_ = false; }
    const QString want = follows_pc_ ? pc_out() : device_;
    QAudioDevice dev = want.isEmpty() ? QMediaDevices::defaultAudioOutput() : QAudioDevice();
    if (!want.isEmpty()) for (const auto& d : QMediaDevices::audioOutputs()) if (d.description() == want || (!follows_pc_ && d.description().contains(want, Qt::CaseInsensitive))) { dev = d; break; }
    if (dev.isNull() && follows_pc_) dev = QMediaDevices::defaultAudioOutput();               // the chosen output is unplugged: the system default
    if (dev.isNull()) return;
    QAudioFormat fmt; fmt.setSampleRate(48000); fmt.setChannelCount(2); fmt.setSampleFormat(QAudioFormat::Int16);      // stereo: the same audio on left and right, so it does not depend on the sound server upmixing mono
    if (!dev.isFormatSupported(fmt)) { fmt.setChannelCount(1); if (!dev.isFormatSupported(fmt)) return; }          // a device that only does mono: the server handles it
    channels_ = fmt.channelCount();
    sink_ = new QAudioSink(dev, fmt, this); sink_->setBufferSize(48000 * 2 * channels_ / 4);   // 250 ms
    io_ = sink_->start(); available_ = io_ != nullptr;
    if (available_ && !keep_) { keep_ = new QTimer(this); keep_->setInterval(40); connect(keep_, &QTimer::timeout, this, [this] { keepAlive(); }); keep_->start(); }
#endif
}
void AudioOut::push(const std::vector<float>& audio, float volume) {
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
    if (!available_ || QDateTime::currentMSecsSinceEpoch() - last_push_ms_ < 120) return;       // real audio is flowing
    const qint64 frame = 2 * channels_, queued = sink_->bufferSize() - sink_->bytesFree(), want = 48000 / 10 * frame;   // keep ~100 ms of silence queued
    if (queued >= want) return;
    const qint64 n = std::min<qint64>(want - queued, sink_->bytesFree()) / frame * frame; if (n <= 0) return;
    const std::vector<char> zeros(size_t(n), 0); io_->write(zeros.data(), n);
#endif
}
