#include "CwSidetone.h"
#include "AudioOut.h"
#include <QDateTime>
#include <QThread>
#include <QTimer>
#include <algorithm>
#ifdef FIRESIDE_QT_AUDIO
#include <QAudioDevice>
#include <QAudioFormat>
#include <QAudioSink>
#endif

namespace {
constexpr int kIdleMs = 30, kStartMs = 60;   // silence kept queued while idle; the cushion before the first element of an over (its stream comes in ~40 ms bursts)
#ifdef FIRESIDE_QT_AUDIO
class Player : public QObject {               // lives on the sidetone's own thread: its sink, its 5 ms timer
public:
    Player(CwSidetone* o, const QAudioDevice& d, int ch) : o_(o), dev_(d), ch_(ch) {}
    void begin() {
        QAudioFormat fmt; fmt.setSampleRate(48000); fmt.setChannelConfig(ch_ == 2 ? QAudioFormat::ChannelConfigStereo : QAudioFormat::ChannelConfigMono); fmt.setSampleFormat(QAudioFormat::Int16);
        sink_ = new QAudioSink(dev_, fmt, this); sink_->setBufferSize(48000 * 2 * ch_ / 6);   // ~170 ms of room; only ~30-60 ms is kept queued
        io_ = sink_->start(); o_->avail_ = io_ != nullptr;
        t_ = new QTimer(this); t_->setTimerType(Qt::PreciseTimer); t_->setInterval(5); connect(t_, &QTimer::timeout, this, [this] { tick(); }); t_->start();
    }
    void end() { if (t_) t_->stop(); if (sink_) sink_->stop(); o_->avail_ = false; }
private:
    CwSidetone* o_; QAudioDevice dev_; int ch_; QAudioSink* sink_ = nullptr; QIODevice* io_ = nullptr; QTimer* t_ = nullptr; qint64 last_ms_ = 0;
    int queuedMs() const { return int((sink_->bufferSize() - sink_->bytesFree()) / (2 * ch_) / 48); }
    void write(const std::vector<float>& v, float vol) {
        const qint64 bytes = qint64(v.size()) * 2 * ch_; if (sink_->bytesFree() < bytes) { ++o_->dropped_; return; }
        std::vector<int16_t> pcm(v.size() * size_t(ch_));
        for (size_t i = 0; i < v.size(); ++i) { const int16_t s = int16_t(std::lround(std::clamp(v[i] * vol, -1.0f, 1.0f) * 32767.0f)); for (int c = 0; c < ch_; ++c) pcm[i * size_t(ch_) + size_t(c)] = s; }
        io_->write(reinterpret_cast<const char*>(pcm.data()), bytes);
    }
    void tick() {
        if (!io_) return;
        const int q = queuedMs(); std::vector<float> v = o_->take(48000 / 5); const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (v.empty()) { if (now - last_ms_ > 100 && q < kIdleMs) write(std::vector<float>(size_t(48 * (kIdleMs - q)), 0.0f), 0); return; }   // idle: a little silence, so the output never sleeps
        if (now - last_ms_ > 150 && q < kStartMs) v.insert(v.begin(), size_t(48 * (kStartMs - q)), 0.0f);   // an over starts
        else if (q == 0) ++o_->dry_;
        last_ms_ = now; write(v, o_->volume());
    }
};
#endif
}  // namespace

CwSidetone::CwSidetone(bool enable, QObject* parent) : QObject(parent), enable_(enable) {
    if (!enable_) return;
    start();
    watch_ = new QTimer(this); watch_->setInterval(1000);
    connect(watch_, &QTimer::timeout, this, [this] { if (AudioOut::pcOutput() != dev_) { stop(); start(); } }); watch_->start();
}
CwSidetone::~CwSidetone() { stop(); }
void CwSidetone::start() {
    dev_ = AudioOut::pcOutput();
#ifdef FIRESIDE_QT_AUDIO
    QAudioDevice d; int ch = 2;
    if (!AudioOut::radioPhones() && AudioOut::pcDeviceFormat(&d, &ch)) {
        thr_ = new QThread; thr_->setObjectName("cw-sidetone"); auto* p = new Player(this, d, ch); p->moveToThread(thr_); worker_ = p;
        connect(thr_, &QThread::started, p, [p] { p->begin(); });
        thr_->start(QThread::TimeCriticalPriority); return;
    }
#endif
    out_ = new AudioOut(true, this); out_->setIdleFillMs(kIdleMs); avail_ = out_->available();   // the radio's headphones: mixed on the GUI thread
    t_ = new QTimer(this); t_->setInterval(10); connect(t_, &QTimer::timeout, this, [this] { const auto v = take(48000 / 5); if (!v.empty()) out_->push(v, vol_); }); t_->start();
}
void CwSidetone::stop() {
#ifdef FIRESIDE_QT_AUDIO
    if (thr_) {
        auto* p = static_cast<Player*>(worker_);
        QMetaObject::invokeMethod(p, [p] { p->end(); delete p; }, Qt::BlockingQueuedConnection);
        thr_->quit(); thr_->wait(); delete thr_; thr_ = nullptr; worker_ = nullptr;
    }
#endif
    if (t_) { delete t_; t_ = nullptr; } if (out_) { delete out_; out_ = nullptr; } avail_ = false;
}
void CwSidetone::feed(const uint8_t* key, size_t n, double rate) {
    std::lock_guard<std::mutex> l(m_);
    const double step = rate / 48000.0, de = 1.0 / (0.005 * 48000.0), dph = 2 * M_PI * pitch_ / 48000.0;
    while (pos_ < double(n)) {
        const bool k = key[size_t(pos_)] != 0; env_ = k ? std::min(1.0, env_ + de) : std::max(0.0, env_ - de);
        pending_.push_back(float(0.5 * (1 - std::cos(M_PI * env_)) * std::sin(ph_))); ph_ += dph; if (ph_ > 2 * M_PI) ph_ -= 2 * M_PI;
        pos_ += step;
    }
    pos_ -= double(n);
    if (pending_.size() > 48000 / 5) { pending_.erase(pending_.begin(), pending_.end() - 48000 / 10); ++trimmed_; }   // never let the delay grow (nothing is playing it)
}
std::vector<float> CwSidetone::take(size_t max) {
    std::lock_guard<std::mutex> l(m_); std::vector<float> v;
    if (pending_.size() <= max) v.swap(pending_); else { v.assign(pending_.begin(), pending_.begin() + long(max)); pending_.erase(pending_.begin(), pending_.begin() + long(max)); }
    return v;
}
QString CwSidetone::takeStats() {
    long t; { std::lock_guard<std::mutex> l(m_); t = trimmed_; trimmed_ = 0; }
    const long d = dropped_.exchange(0), g = dry_.exchange(0);
    return d || t || g ? QString("sidetone: %1 time(s) ran dry, %2 block(s) dropped, %3 backlog trim(s)").arg(g).arg(d).arg(t) : QString();
}
