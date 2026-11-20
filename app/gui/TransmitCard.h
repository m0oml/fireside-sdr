#pragma once
#include <QFrame>
#include <QString>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include "fireside/sim_radio.h"
#include "fireside/tx_control.h"
#include "fireside/qso_seq.h"
#include <functional>
class QCheckBox; class QComboBox; class QDoubleSpinBox; class QLabel; class QLineEdit; class QProgressBar; class QPushButton; class QSpinBox; class QTimer;
class RadioPanel; class ReceiverPanel; class PowerMeter; class AudioPanel;

// The radio's transmit controls on the Receiver screen: transmit gets ready by itself once the radio runs (EEPROM check + PA bias; a Retry button only if that fails), ATU Tune at a chosen power (default 3 W), Measure drive (2 W ramp,
// needed once per band), Restore ATU from the per-band memory (no RF; optionally on every band change), forward / reflected / SWR meters and a TX lamp.
// Everything goes through RadioSession::transmit() (fireside/tx_control.h): the gate, its limits and the hard drive clamp live there.
// Tune and Measure drive ask first ("listen first": a carrier on a band that may not be matched); Arm (no RF), Test tone and FT8/FT4 Send
// go straight ahead (the status line says what is queued; Stop cancels). While transmitting the receive stream is paused (the radio's ALSA playback and our direct receive stream
// cannot run together) and reopened afterwards.
class TransmitCard : public QFrame {
    Q_OBJECT
public:
    TransmitCard(RadioPanel* radio, ReceiverPanel* rx, bool simulated, QWidget* parent = nullptr);
    ~TransmitCard() override;
    // test hooks
    QPushButton* armButton() const { return arm_; } QPushButton* tuneButton() const { return tune_; } QPushButton* measureButton() const { return measure_; }
    QPushButton* sendButton() const { return send_; } QLineEdit* messageBox() const { return ft_msg_; } QComboBox* parityBox() const { return ft_par_; }
    bool sendQueued() const { return ft_pending_ != nullptr; }
    QPushButton* restoreButton() const { return restore_; } QPushButton* testToneButton() const { return test_; } QDoubleSpinBox* powerBox() const { return power_; } QLabel* statusLabel() const { return status_; }
    QLabel* memoryLabel() const { return mem_; }
    void setConfirmForTests(bool yes) { auto_confirm_ = yes ? 1 : 0; }   // skip the questions (tests): 1 = answer yes
    bool busy() const { return worker_running_.load(); }
    bool keyedNow() const;                               // the transmitter is keyed right now (CAT get_ptt)
    void setSimulatedRadio(fireside::sim::RadioModel* m);
    void autoArm();                                      // the EEPROM check + PA bias (read-only check, ~16 s): run by itself once the radio is started (not in --simulate)
    void setAutoArm(bool on) { auto_arm_ = on; }
    // Voice: PTT (the button held, the space bar if ticked, the mic / RCA PTT if ticked) keys an SSB over with the Audio tab's input and processing.
    void setAudioPanel(AudioPanel* a) { audio_ = a; }
    void refreshButtons() { updateButtons(); }
    bool haveStation();                                  // your callsign and locator are known; if not, says so (nothing is sent)
    QString myGrid4();                                  // the Log tab's (Wavelog station profile) grid, else the one in our CQ, else empty (never a guess)
    void setTxAntennaSource(std::function<int(double dialHz)> f) { tx_ant_ = std::move(f); updateButtons(); }   // the Antennas tab's TX antenna for a frequency
    QPushButton* pttButton() const { return ptt_; }
    void setPttForTests(bool down) { ptt_btn_ = down; onPttChanged(); }
    bool eventFilter(QObject* o, QEvent* e) override;
    void armThen(std::function<void()> done);           // start-up: the EEPROM check first, then `done` (opens the receive stream); at once if not needed   // --simulate: the model gets the backup as its EEPROM and the transmit level for its PA
    static QString findBackup();                                  // the newest F5K_*_trx.bin in backupDir() (or the one named in the settings)
    static QString backupDir();                                   // $FIRESIDE_BACKUP_DIR, else ~/.local/share/fireside/backups
    void setBackupOfferForTests(bool on) { backup_offer_tests_ = on; }   // tests answer the "save a backup?" question themselves (it is skipped otherwise)
    // Click to reply: a double-clicked FT8/FT4 decoder line. Sets the opposite slot and the right message, queues it (when armed), and from then on the
    // sequencer answers each step (report, R-report, RR73, 73) in our slots, repeating up to 6 times; Stop ends it. The finished QSO goes out on logQso.
    void replyTo(const QString& decoderLine);
    void heardLine(const QString& decoderLine);          // every decoded line addressed to me (the sequencer moves on; nothing is sent here)
    void setMyGridSource(std::function<QString()> f) { my_grid_ = std::move(f); }
    QCheckBox* autoSeqBox() const { return auto_seq_; }
    QPushButton* cqButton() const { return cq_; }
    double recentTxPowerW() const;                       // mean forward power of our last FT8/FT4 over if it was in the last 10 minutes, else 0 (the log's TX_PWR)
    PowerMeter* powerMeter() const { return pm_; }
    QString qsoText() const;                             // the sequencer's one-line state ("" if idle)
    QString messageText() const;                         // newest F5K_*_trx.bin in backupDir() (or the remembered one)
signals:
    void message(const QString& text);
    void qsoPartner(const QString& call);                // the station the sequencer is working ("" = none)
    void txSent(const QString& utcLabel, int audioHz, const QString& message);   // an over starts
    void logQso(const QString& call, const QString& grid, const QString& rstSent, const QString& rstRcvd);   // a QSO the sequencer completed
private slots:
    void onArm(bool on); void onTune(); void onMeasure(); void onRestore(); void onTestTone(); void onSend(); void tick();
    void onRadioRunning(bool on); void onDial(double hz);
private:
    RadioPanel* radio_; ReceiverPanel* rx_; bool simulated_; fireside::sim::RadioModel* sim_radio_ = nullptr;
    QPushButton *arm_, *tune_, *measure_, *restore_, *stop_, *test_; QCheckBox* mirror_; QDoubleSpinBox* power_; QDoubleSpinBox* test_w_ = nullptr; QComboBox* thr_; QCheckBox* auto_restore_;
    QProgressBar *fwd_, *rev_; QLabel *swr_, *lamp_, *status_, *mem_; QTimer* timer_;
    QComboBox *ft_mode_, *ft_par_; QLineEdit* ft_msg_; QSpinBox* ft_hz_; QDoubleSpinBox* ft_w_; QPushButton* send_; QCheckBox* ft_repeat_; QTimer* ft_timer_;
    std::shared_ptr<fireside::txc::TxControl::FtPrepared> ft_pending_; void startSend(); void queueSend();
    std::shared_ptr<fireside::txc::TxControl> tx_; std::thread worker_; std::atomic<bool> worker_running_{false}; int auto_confirm_ = -1; bool backup_offer_tests_ = false; std::atomic<bool> eeprom_verified_{false};
    std::mutex m_; fireside::txc::Progress last_; bool have_last_ = false; int last_band_ = -1; unsigned paused_rate_ = 0; bool paused_live_ = false; std::atomic<bool> unkey_failed_{false};
    bool ask(const QString& title, const QString& text);
    std::atomic<double> last_tx_w_{0}, unkeyed_utc_{0}, job_end_utc_{0}; std::atomic<qint64> last_tx_t_{0};
    std::function<int(double)> tx_ant_; int txAnt() const;
    AudioPanel* audio_ = nullptr; QComboBox* voice_mode_ = nullptr; QDoubleSpinBox* voice_w_ = nullptr; QPushButton* ptt_ = nullptr; QCheckBox *space_ptt_ = nullptr, *mic_ptt_ = nullptr; QTimer* ptt_poll_ = nullptr;
    std::atomic<bool> ptt_btn_{false}, ptt_key_{false}, ptt_mic_{false}; std::unique_ptr<fireside::txc::IAudioIn> voice_in_;
    bool pttActive() const { return ptt_btn_ || ptt_key_ || ptt_mic_; }
    void onPttChanged(); void startVoice();
    std::function<void()> after_arm_; bool auto_arm_ = true, arm_tried_ = false; int stop_gen_ = 0; QPushButton* cq_ = nullptr; PowerMeter* pm_ = nullptr;
    QCheckBox* auto_seq_ = nullptr; std::unique_ptr<fireside::seq::Sequencer> seq_; std::function<QString()> my_grid_;
    void pauseReceive(); QString resumeReceive();        // receive pauses for an over (the slot so far decoded first) and comes back after it
    fireside::seq::Sequencer& sequencer();
    void takeLog();
    fireside::txc::PcmFactory pcmFactory() const;
    static double playbackRate();
    static double txRate();                              // duplex stream running: its rate (receive keeps going); else what snd-dice offers                        // what snd-dice offers now (8 channels): 48, 96 or 192 kHz
    void run(const QString& what, bool pauses_receive, std::function<QString(fireside::txc::TxControl&)> job);   // on the worker thread; result text back on the GUI thread
    void updateButtons(); void updateMemoryLabel(); void loadState(); void saveState();
    void report(const QString& m);
    void offerBackup();                                          // no backup yet: asks, then reads the EEPROM twice, saves it and arms
};
