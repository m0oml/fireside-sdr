#include "AudioOut.h"
#ifdef FIRESIDE_QT_AUDIO
#include <QAudioFormat>
#include <QAudioSink>
#include <QMediaDevices>
#endif
#include <QDateTime>
#include <QTimer>
#include <algorithm>
#include <cmath>

AudioOut::AudioOut(bool enable, QObject* parent) : QObject(parent) {
#ifdef FIRESIDE_QT_AUDIO
    if (!enable) return;
    const QAudioDevice dev = QMediaDevices::defaultAudioOutput();
    if (dev.isNull()) return;
    QAudioFormat fmt; fmt.setSampleRate(48000); fmt.setChannelCount(2); fmt.setSampleFormat(QAudioFormat::Int16);      // stereo: the same audio on left and right, so it does not depend on the sound server upmixing mono
    if (!dev.isFormatSupported(fmt)) { fmt.setChannelCount(1); if (!dev.isFormatSupported(fmt)) return; }          // a device that only does mono: the server handles it
    channels_ = fmt.channelCount();
    sink_ = new QAudioSink(dev, fmt, this); sink_->setBufferSize(48000 * 2 * channels_ / 4);   // 250 ms
    io_ = sink_->start(); available_ = io_ != nullptr;
    if (available_) { auto* t = new QTimer(this); t->setInterval(40); connect(t, &QTimer::timeout, this, [this] { keepAlive(); }); t->start(); }
#else
    (void)enable;
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
