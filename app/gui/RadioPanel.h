#pragma once
#include <QWidget>
#include <functional>
#include <memory>
#include "fireside/session.h"
class QCheckBox; class QComboBox; class QLabel; class QPushButton; class QTimer; class ReceiverPanel;

// FireWire radio session controls. Connect and Identify are read-only. Start/Stop run the REVIEWED start-up/shutdown writes and need the explicit "Allow writes" box
// (off by default) plus a confirmation dialog. Once started, the receiver's dial box (and click-to-tune on the spectrum) retunes the radio through fireside::Tuner only.
// All writes go through fireside::RadioSession; nothing in the GUI touches the mailbox directly.
class RadioPanel : public QWidget {
    Q_OBJECT
public:
    RadioPanel(ReceiverPanel* rx, fireside::RadioSession::Opener opener, bool simulated, QWidget* parent = nullptr);
    ~RadioPanel() override;
    bool connectRadio();                                // read-only: probe + verify the byte order
    void identifyRadio();                               // read-only
    void setAntennaProvider(std::function<std::string(double dialHz)> f) { antenna_for_ = std::move(f); }   // the Antennas tab: which RX antenna for this dial (start-up uses it)
    void requestRxAntenna(const QString& name, const QString& why);   // while started: switch the RX antenna through the session (SET_RX1_ANT 1..4 only); silent when already set
    void autoStart();
    bool simulated() const { return simulated_; }
    void setBeforeStream(std::function<void(std::function<void()>)> f) { before_stream_ = std::move(f); }   // autoStart: runs (the EEPROM check) between starting the radio and opening the stream                                    // the app opened: start receiving by itself when the box is ticked (not simulated)
    bool quickStart(bool confirmed = false, unsigned rate = 0);   // rate 0 = the remembered one
    bool startAtRate(unsigned rate, bool confirmed = false);   // the Receiver's 48 / 96 / 192 buttons: radio already running -> only the stream restarts (no question, no write); else quickStart at that rate            // the one-click path: ask once, connect (with writes), start the radio, open the live stream. confirmed=true skips the question (tests)
    bool startRadio(bool confirmed = false);            // confirmed=false asks first (QMessageBox); needs "Allow writes"
    bool stopRadio();                                   // reviewed shutdown
    void disconnectRadio();
    bool shutdownOnExit();                              // called when the window closes: stops a started radio. false if the shutdown failed
    fireside::RadioSession& session() { return session_; }
    bool confirmStart(double mhz);                        // the "Start the radio?" question (shared by Start and Start receiving)
    QCheckBox* allowWritesBox() const { return allow_; }
    QComboBox* deviceBox() const { return device_; } QComboBox* orderBox() const { return order_; } QComboBox* antBox() const { return ant_; }
    QString statusText() const;
signals:
    void message(const QString& text);
    void runningChanged(bool started);                  // the radio started / stopped (for the Antennas tab)
    void rxAntennaChanged(const QString& name);         // the antenna the radio is using now ("" after a stop)
    void radioStarted();                                // after quickStart succeeded: MainWindow shows the Receiver tab
private slots:
    void onDial(double hz);
    void onPreamp(bool on);                             // the Receiver's Preamp button: SET_TRX_PREAMP 0/1 through the session, nothing else
    void tick();
private:
    ReceiverPanel* rx_; fireside::RadioSession session_; bool simulated_;
    QComboBox *device_, *order_, *ant_; QCheckBox* auto_ = nullptr; std::function<void(std::function<void()>)> before_stream_; QCheckBox* allow_; std::function<std::string(double)> antenna_for_; bool was_started_ = false; QPushButton *alsa_ = nullptr, *quick_ = nullptr, *refresh_, *connect_, *identify_, *start_, *stop_, *disconnect_; QLabel* status_; QTimer* timer_;
    QString last_error_; bool maybe_started_ = false;   // set from the first start write until a confirmed shutdown: a failed start may have left the radio out of standby
    void updateButtons(); void report(const QString& m); void refreshDevices(); void loadSettings(); void saveSettings();
};
