#pragma once
#include <QObject>
#include <vector>
class QAudioSink; class QIODevice;

// Optional speaker output for the demodulated audio (48 kHz; the mono audio is played on both left and right). Compiles to a stub without Qt Multimedia. Drops audio rather than letting latency grow.
class AudioOut : public QObject {
    Q_OBJECT
public:
    explicit AudioOut(bool enable, QObject* parent = nullptr);
    bool available() const { return available_; }
    void push(const std::vector<float>& audio, float volume);   // volume 0..1
private:
    int channels_ = 2; bool available_ = false; QAudioSink* sink_ = nullptr; QIODevice* io_ = nullptr; qint64 last_push_ms_ = 0;
    void keepAlive();   // no audio for a while (receive paused while we transmit): silence, so the sound server never suspends the output (waking it took 2-3 s)
};
