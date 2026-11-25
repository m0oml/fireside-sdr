#pragma once
#include <QObject>
#include <QString>
#include <vector>
class QAudioSink; class QIODevice; class QTimer;

// Optional speaker output for the demodulated audio (48 kHz; the mono audio is played on both left and right). Compiles to a stub without Qt Multimedia. Drops audio rather than letting latency grow.
// `device`: empty = the system's default output; otherwise the output whose description contains it (e.g. "Fireside RX audio", the virtual device
// other programs record from); not available() if there is none.
class AudioOut : public QObject {
    Q_OBJECT
public:
    explicit AudioOut(bool enable, QObject* parent = nullptr, const QString& device = QString());
    ~AudioOut() override;
    static bool deviceExists(const QString& device);              // an output whose description contains `device`
    // The PC output for every AudioOut made without a device (receive audio, monitor, feedback test, CW sidetone): empty = the system's default.
    // Remembered (QSettings fireside/audio pc_output); changing it reopens those outputs at once.
    static QString pcOutput();
    static void setPcOutput(const QString& description);
    // pcOutput() == kRadioPhones: those outputs play from the RADIO's headphone jack (channels 2/3 of the duplex FireWire stream, on the audio
    // ring: the transmit I/Q is on channels 0/1 and is not touched). Every such output is mixed into one writer; nothing plays while the radio
    // is stopped. Silent (not an error) until the duplex stream runs.
    static constexpr const char* kRadioPhones = "@radio-headphones";
    static bool radioPhones() { return pcOutput() == kRadioPhones; }
    // Tests: the last block mixed for the radio's headphones (peak, 0 if none) and how many blocks were mixed since start.
    static float radioPhonesPeakForTests(); static long radioPhonesBlocksForTests();
    bool available() const { return available_; }
    void push(const std::vector<float>& audio, float volume);   // volume 0..1
private:
    void open();
    QString device_; bool follows_pc_ = false, enabled_ = false, to_radio_ = false;
    int channels_ = 2; bool available_ = false; QAudioSink* sink_ = nullptr; QIODevice* io_ = nullptr; QTimer* keep_ = nullptr; qint64 last_push_ms_ = 0;
    void keepAlive();   // no audio for a while (receive paused while we transmit): silence, so the sound server never suspends the output (waking it took 2-3 s)
};
