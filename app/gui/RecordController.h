#pragma once
#include <QObject>
#include <QDateTime>
#include <QSet>
#include <QString>
#include <QStringList>
#include <memory>
#include "fireside/recorder.h"
class QWidget;
class ReceiverPanel;

// Recording from the Receiver tab: an always-on replay buffer of the live stream ("Grab" saves the last N seconds) and a start/stop recording with a pre-roll. Saves the band's I/Q
// (the radio's raw I/Q, replayable in the app: each file gets a sidecar with the dial, mirror and IF offset) and/or the audio you hear. Options are remembered
// (QSettings "fireside/record"; not with FIRESIDE_NO_SETTINGS). Writes files only: never touches the radio.
class RecordController : public QObject {
    Q_OBJECT
public:
    explicit RecordController(ReceiverPanel* rx);
    ~RecordController() override;
    void sourceChanged(bool live, double iq_rate, double audio_rate);   // the live source opened or closed: (re)allocate the replay buffers; buttons are only usable while live
    bool available() const { return live_; }
    bool recording() const;
    QString recordingText() const;                       // "REC 0:42  38 MB" while recording, else ""
    int grabSeconds() const { return grab_s_; }
    void grab();                                         // save the last grabSeconds() (clamped to what is buffered)
    void toggleRecord();                                 // start (with the pre-roll) / stop
    void showOptions(QWidget* parent);
    QString folder() const { return folder_; }
    QStringList recent() const { return recent_; }       // files saved this session, newest first
    // test and tool hooks
    void setFolder(const QString& f) { folder_ = f; }
    void setOptions(int replay_s, int grab_s, int preroll_s, bool iq, bool audio);
    fireside::Recorder& iq() { return iq_; }
    fireside::Recorder& audio() { return audio_; }
    QString lastSummary() const { return last_summary_; }
signals:
    void message(const QString& text);                   // "Saved ..." / "Cannot ..." for the status line
    void stateChanged();                                 // available / recording changed (buttons)
private:
    QString baseName(const QString& label);        // YYYYmmdd_HHMMSS_<dial kHz>kHz_<mode>[_label]
    void writeSidecar(const QString& wav, const QString& kind, double rate, double seconds, const QDateTime& end) const;
    void save(); void load();
    ReceiverPanel* rx_; fireside::Recorder iq_{{2, 32}}, audio_{{1, 16}}; bool live_ = false, rec_iq_ = false, rec_audio_ = false;
    int replay_s_ = 60, grab_s_ = 30, preroll_s_ = 5; bool want_iq_ = true, want_audio_ = true; QString folder_, label_, last_summary_, rec_base_; QStringList recent_;
    double iq_rate_ = 0, audio_rate_ = 0; QSet<QString> used_;
};
