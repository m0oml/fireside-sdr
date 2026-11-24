#pragma once
#include <QFrame>
#include <QString>
#include <QSet>
#include <atomic>
#include <memory>
#include <mutex>
#include <thread>
#include "fireside/sim_radio.h"
#include "fireside/tx_control.h"
#include "fireside/qso_seq.h"
#include "fireside/rigctl.h"
#include "fireside/eeprom.h"
#include "fireside/cw_keys.h"
#include "fireside/vox.h"
#include <functional>
class QCheckBox; class QComboBox; class QDialog; class QDoubleSpinBox; class QLabel; class QLineEdit; class QProgressBar; class QPushButton; class QSpinBox; class QTimer;
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
    void setMaxTriesForTests(int n) { test_tries_ = n; }   // smoke tests: a QSO gives up after n tries (not 6)
    QSpinBox* ftHzBox() const { return ft_hz_; } QCheckBox* ftClearBox() const { return ft_clear_; }
    QPushButton* cqButton() const { return cq_; }
    double recentTxPowerW() const;                       // mean forward power of our last FT8/FT4 over if it was in the last 10 minutes, else 0 (the log's TX_PWR)
    PowerMeter* powerMeter() const { return pm_; }
    // The card is only the owner: these are the pieces the window places. key: lamp, Stop, status. atu: tune power, threshold, Tune, Restore. calibration: a fold with Measure drive and Test tone.
    // digital: the FT8 / FT4 message controls. voice: mic, power, PTT. other: the CAT keying tick and its power.
    QWidget* atuPart() const { return atu_part_; }
    void applyRxCal(const fireside::RxLevelCal& c);
    void onGenCheck();                                   // the S-meter check with the radio's internal generator (receive only)
    void applyGenCheck(const fireside::txc::TxControl::GenCheck& r, int band);
    void loadCheckedCal();
    QPushButton* genCheckButton() const { return gen_btn_; }
    QDoubleSpinBox* ftPowerBox() const { return ft_w_; }    // hands the EEPROM's RX level table to the S-meter
    QWidget* otherProgramsPart() const { return ext_part_; } QLabel* lampLabel() const { return lamp_; } QDialog* tunerDialog() const { return tuner_dialog_; } QPushButton* tunerButton() const { return tuner_btn_; } QPushButton* stopButton() const { return stop_; }
    QWidget* digitalPart() const { return digi_part_; } QWidget* voicePart() const { return voice_part_; }
    QString qsoText() const;                             // the sequencer's one-line state ("" if idle)
    QString statusText() const;
    // CW (semi break-in): the KEY jack's paddle or straight key keys an over while the screen is in CW, transmit is armed and "Paddle keys" is
    // ticked; typed text (or a memory) goes out with Send. TxControl::cw() does the keying (the gate, limits and clamp); see TransmitCardCw.cpp.
    QWidget* cwPart() const { return cw_part_; }
    void setCwMemorySource(std::function<QStringList()> f) { cw_mems_ = std::move(f); }
    QLineEdit* cwTextBox() const { return cw_text_; } QPushButton* cwSendButton() const { return cw_send_; } QLabel* cwLights() const { return cw_lights_; }
    QCheckBox* cwPaddleBox() const { return cw_paddle_; } QDoubleSpinBox* cwPowerBox() const { return cw_w_; }
    bool cwWatching() const;                             // the paddle is being read (CW screen, armed, ticked)
    bool cwOver() const { return cw_over_; }
    // VOX (voice keys the transmitter): off in every new window, never remembered; only on the Voice screen with transmit ready. Monitor: hear the
    // processed over on the PC's speakers. See TransmitCardVox.cpp.
    QCheckBox* voxBox() const { return vox_; } QCheckBox* monitorBox() const { return monitor_; } QSpinBox* voxThresholdBox() const { return vox_thr_; }
    bool voxListening() const { return vox_in_ != nullptr; }
    void setVoiceTimeoutForTests(double s) { voice_timeout_s_ = s; }
    void setPcAudioEnabled(bool on) { pc_audio_ = on; }    // false (smoke tests): the sidetone and monitor never open a sound device
    void onReceiveLost(const QString& why);              // receive stopped by itself: the QSO, CQ, anything queued and a CAT-keyed over stop (a tune or the operator's voice PTT carries on)                          // the card's status line (tests)
    QString messageText() const;                         // newest F5K_*_trx.bin in backupDir() (or the remembered one)
    // Keying by other programs over CAT (Hamlib set_ptt): the ONLY implementation of rigctl::PttRequest. An over is TxControl::voice() with the data
    // chain, the "Fireside TX audio" input and the card's power; it keys only while: "Let other programs key" is ticked (never remembered), transmit
    // is armed, the owning program holds PTT and has talked to us in the last 15 s, at most 2 minutes, in an amateur band with a measured drive level,
    // with the direct FireWire stream running (receive keeps going). Stop, unticking, a radio stop and the program disconnecting all unkey.
    fireside::rigctl::PttRequest* pttRequest();
    QCheckBox* extAllowBox() const { return ext_allow_; }
    QDoubleSpinBox* extPowerBox() const { return ext_w_; }
    bool extOverRunning() const { return ext_over_.load(); }
    void setExtWatchdogForTests(qint64 ms) { ext_watchdog_ms_ = ms; }
signals:
    void message(const QString& text);
    void safetyTrip(const QString& reason);               // an over ended on a safety limit or failure (SWR, power, time limit, meter / stream / audio failure ...): shown in red by the window
    void qsoPartner(const QString& call);                // the station the sequencer is working ("" = none)
    void txSent(const QString& utcLabel, int audioHz, const QString& message);   // an over starts
    void logQso(const QString& call, const QString& grid, const QString& rstSent, const QString& rstRcvd);   // a QSO the sequencer completed
    void qsoEnded(const QString& call, bool done);      // the sequence with call ended: done (completed, logged) or gave up
private slots:
    void onArm(bool on); void onTune(); void onMeasure(); void onRestore(); void onTestTone(); void onSend(); void tick();
    void onRadioRunning(bool on); void onDial(double hz);
private:
    RadioPanel* radio_; ReceiverPanel* rx_; bool simulated_; fireside::sim::RadioModel* sim_radio_ = nullptr;
    QPushButton *arm_, *tune_, *measure_, *restore_, *stop_, *test_; QCheckBox* mirror_; QDoubleSpinBox* power_; QDoubleSpinBox* test_w_ = nullptr; QComboBox* thr_; QCheckBox* auto_restore_;
    QProgressBar *fwd_, *rev_; QLabel *swr_, *lamp_, *status_, *mem_; QTimer* timer_;
    QComboBox *ft_mode_, *ft_par_; QLineEdit* ft_msg_; QSpinBox* ft_hz_; QCheckBox* ft_clear_ = nullptr; void pickClearCqHz(); QDoubleSpinBox* ft_w_; QPushButton* send_; QCheckBox* ft_repeat_; QTimer* ft_timer_;
    std::shared_ptr<fireside::txc::TxControl::FtPrepared> ft_pending_; void startSend(); void queueSend();
    std::shared_ptr<fireside::txc::TxControl> tx_; std::thread worker_; std::atomic<bool> worker_running_{false}; int auto_confirm_ = -1; bool backup_offer_tests_ = false; std::atomic<bool> eeprom_verified_{false};
    std::mutex m_; fireside::txc::Progress last_; bool have_last_ = false; int last_band_ = -1; unsigned paused_rate_ = 0; bool paused_live_ = false; std::atomic<bool> unkey_failed_{false}; std::atomic<bool> job_tripped_{false};   // set by the core's trip hook (worker thread) during the running job
    bool ask(const QString& title, const QString& text);
    std::atomic<double> last_tx_w_{0}, unkeyed_utc_{0}, job_end_utc_{0}; std::atomic<qint64> last_tx_t_{0};
    std::function<int(double)> tx_ant_; int txAnt() const;
    AudioPanel* audio_ = nullptr; QComboBox* voice_mode_ = nullptr; QDoubleSpinBox* voice_w_ = nullptr; QPushButton* ptt_ = nullptr; QCheckBox *space_ptt_ = nullptr, *mic_ptt_ = nullptr; QTimer* ptt_poll_ = nullptr;
    std::atomic<bool> ptt_btn_{false}, ptt_key_{false}, ptt_mic_{false}; std::unique_ptr<fireside::txc::IAudioIn> voice_in_;
    bool pttActive() const { return ptt_btn_ || ptt_key_ || ptt_mic_; }
    void onPttChanged(); void startVoice(); void connectTrip();
    struct ExtPtt; std::unique_ptr<ExtPtt> ext_; QCheckBox* ext_allow_ = nullptr; QDoubleSpinBox* ext_w_ = nullptr;
    std::atomic<int> ext_owner_{-1}; std::atomic<bool> ext_ptt_{false}, ext_allowed_{false}, ext_over_{false}; std::atomic<qint64> ext_beat_ms_{0};
    static constexpr qint64 kExtWatchdogMs = 15000; static constexpr double kExtMaxKeyS = 120;
    std::atomic<qint64> ext_watchdog_ms_{kExtWatchdogMs};                       // tests shorten it (setExtWatchdogForTests)
    // ext_m_ orders the end of an over against a re-key from the same program. Once extActive() has said no, the over is closing (latched); a T 1 from
    // its owner from then until the job's completion has run on the GUI thread is held (ext_rekey_) and started there as a new over, never folded into
    // the over that is unkeying and never refused as "busy". The owner and ext_over_ are cleared only by that completion (extOverEnded).
    mutable std::mutex ext_m_; mutable bool ext_closing_ = false; bool ext_rekey_ = false;
    void stopEverything(const QString& why);                                       // the Stop button: unkey, nothing queued, the QSO / CQ / repeat ended
    void extCancel();                                                              // Stop, untick, radio stop, close: release and drop any held re-key
    void extOverEnded(bool may_restart);                                           // GUI thread, from run()'s completion of the program's over
    bool extActive() const; bool requestExt(int client, bool on); void extHeartbeat(int client);
    std::function<void()> after_arm_; bool auto_arm_ = true, arm_tried_ = false; int stop_gen_ = 0; QPushButton* cq_ = nullptr; PowerMeter* pm_ = nullptr; fireside::RxLevelCal rx_cal_; QPushButton* gen_btn_ = nullptr; QString ended_for_, logged_for_; void noteSeqEnd(bool may_continue = true); void autoNext(); QSet<QString> auto_tried_; bool auto_reply_ = false, from_cq_ = false, follow_pending_ = false; QPushButton *tune2_ = nullptr, *stop2_ = nullptr, *stop3_ = nullptr, *tuner_btn_ = nullptr; QDialog* tuner_dialog_ = nullptr; QWidget *key_part_ = nullptr, *atu_part_ = nullptr, *cal_part_ = nullptr, *cal_body_ = nullptr, *digi_part_ = nullptr, *voice_part_ = nullptr, *ext_part_ = nullptr;
    QString odd_call_; qint64 odd_call_ms_ = 0;
    // Attended CQ (the box that was "Repeat"): kAttCalls calls in one slot parity, then kAttPauseSlots or more silent slots (to the next slot of the
    // other parity), then CQ again in that other parity. A message typed by hand is sent kAttCalls times, then back to CQ; a QSO that ends goes back to CQ.
    static constexpr int kAttCalls = 6, kAttPauseSlots = 3;
    QTimer* att_timer_ = nullptr; bool att_pause_ = false; int att_count_ = 0; std::atomic<double> last_slot_utc_{0}; QString last_sent_;
    void startAttPause(); void endAttPause();
public:
    bool attendedPause() const { return att_pause_; }
    void setAttendedPauseForTests(int ms) { att_pause_ms_for_tests_ = ms; }   // smoke tests: a short (or long) pause, and fewer calls per block
    void setAttendedCallsForTests(int n) { att_calls_ = n > 0 ? n : kAttCalls; }
    QCheckBox* attendedBox() const { return ft_repeat_; }
private:
    int att_pause_ms_for_tests_ = -1, att_calls_ = kAttCalls;
    // CW
    QWidget* cw_part_ = nullptr; QSpinBox *cw_wpm_ = nullptr, *cw_weight_ = nullptr, *cw_hang_ = nullptr; QComboBox *cw_mode_ = nullptr, *cw_mem_ = nullptr; QCheckBox *cw_rev_ = nullptr, *cw_paddle_ = nullptr, *cw_side_ = nullptr;
    QDoubleSpinBox* cw_w_ = nullptr; QLineEdit* cw_text_ = nullptr; QPushButton *cw_send_ = nullptr, *cw_stop_ = nullptr; QLabel* cw_lights_ = nullptr; QTimer* cw_timer_ = nullptr;
    std::unique_ptr<class CwSidetone> cw_tone_; std::shared_ptr<fireside::cwk::KeyWatcher> cw_keys_; bool cw_over_ = false; std::function<QStringList()> cw_mems_;
    void buildCw(); void cwTick(); void startCw(const QString& text); void saveCw(); void loadCw(); bool cwWanted() const;
    bool rx_hold_ = false;
    // VOX + monitor
    QCheckBox *vox_ = nullptr, *anti_vox_ = nullptr, *monitor_ = nullptr; QSpinBox *vox_thr_ = nullptr, *vox_hang_ = nullptr; QLabel* vox_lvl_ = nullptr; QTimer* vox_timer_ = nullptr;
    std::shared_ptr<fireside::vox::VoxInput> vox_in_; class PcAudioFeed* mon_feed_ = nullptr; std::atomic<bool> vox_kill_{false}; std::string vox_kill_why_; std::mutex vox_m_; double voice_timeout_s_ = 180; bool pc_audio_ = true;
    void buildVox(); void voxTick(); void voxOn(bool on); void saveVox(); void loadVox(); bool voxScreen() const;                                     // receive was lost: nothing automatic sends again until the operator presses Send / CQ or double-clicks a station                // a double-click on a callsign no country uses: asked to confirm (second click within 15 s)
    QCheckBox* auto_seq_ = nullptr; std::unique_ptr<fireside::seq::Sequencer> seq_; std::function<QString()> my_grid_;
    void pauseReceive(); QString resumeReceive();        // receive pauses for an over (the slot so far decoded first) and comes back after it
    fireside::seq::Sequencer& sequencer(); int test_tries_ = 0;
    static constexpr int kCallerTries = 3;                                         // a waiting caller answered by auto mode: this many tries, then the next one
    void takeLog();
    fireside::txc::PcmFactory pcmFactory() const;
    static double playbackRate();
    static double txRate();                              // duplex stream running: its rate (receive keeps going); else what snd-dice offers                        // what snd-dice offers now (8 channels): 48, 96 or 192 kHz
    void run(const QString& what, bool pauses_receive, std::function<QString(fireside::txc::TxControl&)> job);   // on the worker thread; result text back on the GUI thread
    void updateButtons(); void updateMemoryLabel(); void loadState(); void saveState();
    void report(const QString& m);
    void offerBackup();                                          // no backup yet: asks, then reads the EEPROM twice, saves it and arms
};
