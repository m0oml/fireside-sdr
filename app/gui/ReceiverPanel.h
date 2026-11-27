#pragma once
#include <array>
#include <atomic>
#include <QList>
#include <QColor>
#include <QSet>
#include <functional>
#include <QHash>
#include <QPair>
#include <QImage>
#include <QMap>
#include <QCheckBox>
#include <QComboBox>
#include <QPointer>
#include <QPushButton>
#include <QWidget>
class QLineEdit;
class RecordController;
#include <memory>
#include <vector>
#include "fireside/display.h"
#include "fireside/playback.h"
#include "fireside/rx_engine.h"
#include "fireside/dsp.h"
#include "fireside/tx_out.h"
#include "fireside/capture.h"
#include "fireside/fw_iso.h"
class QPlainTextEdit; class AudioOut; class FrequencyDisplay; class SMeter; class SpectrumWidget; class WaterfallWidget;
class QBoxLayout; class QGridLayout; class QVBoxLayout; class QAbstractButton; class QButtonGroup; class QCheckBox; class QComboBox; class QFrame; class QHBoxLayout; class QLabel; class QPushButton; class QSlider; class QSpinBox; class QStackedWidget; class QTimer;

// Receiver view on recorded, synthetic or live (read-only) I/Q. Layout: header (frequency + signal meter), spectrum over waterfall, then a control deck of labelled cards
// (Band, Mode, Filter, Audio, Noise, Source, Advanced). All DSP runs in fireside::RxEngine (worker thread); this widget only draws and plays what the engine hands over.
// It never touches the radio itself: dial changes are emitted as dialChanged and the RadioPanel decides what to do with them.
class ReceiverPanel : public QWidget {
    Q_OBJECT
public:
    explicit ReceiverPanel(bool enableAudio = true, bool threaded = true, QWidget* parent = nullptr);
    ~ReceiverPanel() override;
    bool loadFile(const QString& path);                 // false + status text on error
    void loadDemo();
    bool loadLive(const QString& alsaDevice, double rate);   // read-only ALSA capture (8-ch S32_LE, RX1 I/Q = ch1/ch2)
    void openLive();                                    // the old picker: any ALSA capture card + rate (kept for the radio-jacks audio output, which needs the ALSA capture)
    void syncRateButtons(unsigned rate);                // 48 / 96 / 192 show which direct-stream rate is running (0 = none)
    unsigned liveRate() const { return direct_rate_; }  // the running direct-stream rate, 0 if the source is not the direct stream
    void setRxAntennaText(const QString& name);   // shown in the status line while the radio runs ("" = hidden)
    void setRadioRunning(bool on);                      // enables the Stop radio button
    static QMap<QString, QString> readSidecar(const QString& wavPath);   // key=value notes next to a recording (<name>.txt): dial, mirror, IF offset ...
    fireside::RxOptions options() const;                     // what the engine is configured with, from the controls (tests)
    QString modeName() const;                            // "USB" "LSB" "AM" "CW" "FM"
    bool mirrorOn() const;                               // the Mirror I/Q pill
    double ifOffsetHz() const;                           // the IF offset in use: 9000 or 0
    QString rxAntennaText() const { return rx_ant_text_; }
    QString sourceText() const;                          // what the engine reads (for the sidecar)
    RecordController* recorder() const { return rec_; }
    void showNote(const QString& text);                  // a message in the status line for a few seconds
    QString myCall() const;                              // your callsign for FT8 a-priori decoding: the Decoder card's box, else the Log tab's station
    QLineEdit* myCallBox() const { return my_call_; }
    QComboBox* amDetectorBox() const { return am_det_; }
    bool openLiveAuto();                                // the last live source without asking: the direct FireWire stream at the remembered rate (192 kHz by default), else the radio's ALSA card at 48 kHz
    bool loadLiveDirect(unsigned rate);                      // the radio's FireWire stream received directly (48/96/192 kHz, bypasses snd-dice); PC audio only
    void advance(size_t samples);                       // manual mode (threaded=false): process this many source samples now, then refresh the display
    void refresh();                                     // pull what the engine produced and draw it (the UI timer calls this)
    void setPlaying(bool on);
    bool playing() const { return playing_; }
    double dialHz() const;
    void setDialControlsRadio(bool on);                 // false: the dial is a label; true: it tunes the radio (the display changes colour so it is obvious)
    void setSourceIsLive(bool live);                    // live radio: Mirror I/Q comes up from its own remembered state (default ON, user-confirmed on air); files and the demo keep theirs (default off)
    // The direct stream stalled or failed (no data from the radio, bus reset): restart just the stream, as pressing its rate button does, up to
    // kMaxRestarts times within kRestartWindowMs; then it stays stopped and says so. Never in a test's simulated radio (no direct stream there).
    static constexpr int kMaxRestarts = 3; static constexpr qint64 kRestartWindowMs = 120000;
    // A restart refused because another program has the radio's stream (snd-dice started by something opening the radio's ALSA PCM or MIDI port,
    // 2026-11-25): look again every kWaitPollMs (read-only: the refusal comes before any write) for up to kWaitMs, and take the stream back once it
    // is free. Stop radio, a source chosen by hand or a pause for transmit ends the wait.
    static constexpr qint64 kWaitPollMs = 5000, kWaitMs = 600000;
    bool closeLive();                                   // closes a live radio capture (releases the ALSA device, audio back to the PC); false if the source is not live
    void setPreampControlsRadio(bool on);               // the Preamp button is only usable while a radio session is started; leaving that state unticks it
    bool preampOn() const;
    void setPreampChecked(bool on);                     // sets the button without emitting preampRequested (the RadioPanel reports what the radio actually did)
    void lockIfOffset(bool locked);                     // while a radio session is started the DSP must use the session's 9 kHz IF offset (the DDS sits 9 kHz below the dial)
    void setMuted(bool m);
    void setTransmitWidget(QWidget* w);
    void setTransmitParts(QWidget* digi, QWidget* voice);   // the message controls (beside the decoder, Digital only) and the voice controls (under the waterfall, Voice only)
    void setTransmitMeters(QWidget* power, QLabel* status, QWidget* arm);   // the power meter joins the signal meter; transmit status under both
    void setMeterTx(bool keyed);                        // the meter tile shows the power meter while keyed (150 ms after, as the radio switches), the signal meter otherwise
    bool powerMeterShown() const { return tx_meter_ && tx_meter_->isVisibleTo(this); }
    void setTxLamp(QLabel* lamp);
    // The radio's factory RX level table (from the EEPROM, read with the transmit check): dBm = dBFS + meter[band] (+ preamp[band] while the preamp is on).
    // The EEPROM's meter column is in PowerSDR's units: DttSP's signal-strength meter is 10 log10 of the power SUMMED over the DSP buffer (sdr.c,
    // RXMETER_POST_FILT), and CalibrateLevel forces that buffer to 4096 samples, so it reads 10 log10(4096) = 36.1 dB above our in-passband dBFS (a mean).
    // Confirmed on this radio's 20 m with the internal generator 2026-11-21: measured offset +3.17 dB, table -32.74 + 36.12 = +3.38.
    static constexpr double kTableToDbfsDb = 36.123599479677736;   // 10 log10(4096)
    void setRxLevelCal(bool valid, const std::array<double, 11>& meter, const std::array<double, 11>& preamp);
    bool meterCalibrated() const; double meterOffsetDb() const; int meterSource() const;
    void setCheckedCal(int band, double meter, double preamp);      // from the S-meter check with the internal generator (preamp NaN: not measured)
    std::pair<double, double> levelForChecks() const { return {chk_dbfs_.load(), chk_peak_hz_.load()}; }   // thread-safe: in-passband dBFS, spectrum peak offset (Hz)                       // the TX light, left of the frequency
    int modeGroup() const;                               // 0 Voice, 1 CW, 2 Digital: the master switch
    void setModeGroup(int g) { chooseGroup(g); }
    bool decoderRowVisible() const { return dec_row_ && dec_row_->isVisibleTo(this); }
    bool voiceStripVisible() const { return voice_card_ && voice_card_->isVisibleTo(this); }
    bool sendCardVisible() const { return send_card_ && send_card_->isVisibleTo(this); }
    // the offline transmit section: sits under the decoder, opened with the Transmit button (closed by default)
    void setTransmitOpen(bool on); bool transmitOpen() const;
    void setLookupWidget(QWidget* w);                   // the small callsign lookup box (from the Log tab) lives in the control deck here; the card stays hidden until it is set
    void setLogFormWidget(QWidget* w);                  // the large log form (hidden until setLogActive(true))
    void setLogActive(bool on);                         // true while the log form is open: hides the Band and Mode/filter cards and shows the form in their place
    void markWorked(const QString& call, int state);
    void addDecodedText(const QString& text);
    int highlightedLines() const;
    // The QSO pane (right half of the decoder, FT8/FT4): only the stations we are working: lines to my call, lines from the current partner, and our
    // own transmissions. TransmitCard tells it the partner and what it sends.
    void setQsoPartner(const QString& call);
    void peekDecode();
    // Receive audio for other programs: a second output on the virtual device "Fireside RX audio" (system/setup-system.sh), at a fixed level
    // (level_db, dBFS gain on the demodulated audio) that the Volume slider and Mute do not touch. false if the device does not exist.
    bool setDataOutput(bool on, double level_db);
    bool dataOutputOn() const { return data_out_ != nullptr; }
    // External program mode (the window wires the rest): the decoder is held off (a decoder choice still sets its mode and filter) and its box greyed.
    QCheckBox* externalBox() const { return ext_box_; }
    void setExternalMode(bool on);
    bool externalMode() const { return external_; }
    void setTransmitting(bool keyed, double dds_below_dial_hz = 0);   // a CW over: the DDS sits 1 kHz below the dial (txc::kCwOffsetHz)
    void setTxMute(bool on);
    // The transmit picture: while keyed the spectrum and waterfall show the I/Q being sent (from the tap), in dBm at the antenna (k = the band's
    // measured watts per drive^2; 0 = unknown: dBFS), instead of the receiver's view of the radio's own leakage. Display only.
    void setTxIqTap(std::shared_ptr<fireside::txo::IqTap> tap) { tx_tap_ = std::move(tap); }
    void setTxView(bool keyed, double dds_below_dial_hz, double k_w_per_drive2);
    bool txViewOn() const { return tx_view_; }
    const std::vector<float>& lastTxFrameForTests() const { return tx_last_; }
    double speakerDbfs() const { return speaker_db_.load(); }    // the peak of what the speaker is playing now (anti-VOX), -200 when silent / muted; any thread                             // receive audio off for a CW over (the PC sidetone is heard instead); not the Mute button
    void setCwPart(QWidget* w);                          // the CW transmit controls, beside the Morse decoder (CW only)                  // duplex: while keyed the radio's shared RX1/TX DDS sits ON the dial (no 9 kHz IF shift): receive follows it, so the spectrum stays right                                   // duplex: decode the slot so far now (we transmit next; receive keeps running)
    void addQsoTx(const QString& utcLabel, int audioHz, const QString& message);
    void endQso(const QString& call, bool done);       // the QSO with call ended (done = completed and logged): a closing line in the QSO pane
    QString qsoPartner() const { return qso_partner_; }
    QList<QPair<double, QString>> recentFtx(qint64 window_ms);   // FT8/FT4 decodes heard on this dial within the window: {audio Hz, the line's time label}
    QPair<QString, QString> nextCaller(const QSet<QString>& skip) const;   // the top of the queue not in skip (and not the QSO partner): {call, decoded line}; empty if none
    void fadeQsos();                                       // finished QSOs in the QSO pane fade like callers (6 FT8 / 10 FT4 cycles), then go (tickCallers runs it)
    std::vector<double> decoderFocus() const;             // audio Hz of stations calling us (partner first, the last 3 minutes): the quick FT8/FT4 pass looks harder there
    QString qsoPaneText() const;
    // The queue (above the FT8/FT4 decodes; user 2026-11-27): first come, first served. A station calling us joins at the bottom (a repeat call refreshes its line
    // and keeps its place); the top is worked next when a QSO ends (auto mode). Each row: [up] swaps it with the one above, [x] removes it (not listed again for
    // 10 min), double-click replies now. During a QSO: a station that called us and is then heard calling someone else (or CQ) leaves the queue; a double-click
    // on a decode puts that station at the TOP of the queue instead of breaking off the QSO (added by hand: never removed by itself).
    QString callersText() const;                          // one line per queued station, top first (tests)
    void clearCallers();
    void queueTop(const QString& call, const QString& line);   // put at the top of the queue (by hand)
    void bumpCaller(const QString& call);                 // one place up
    void dropCaller(const QString& call);                 // [x]: off the queue, not listed again for 10 min
    void activateLine(const QString& line, bool from_queue);   // a double-clicked line: reply (decodedLineActivated), or during a QSO queue it at the top
    void setQsoActiveSource(std::function<bool()> f) { qso_active_ = std::move(f); }   // a QSO is under way (the sequencer past CQ): the Transmit card
    static constexpr int kMaxCallers = 30; static constexpr qint64 kMovedOnMs = 600000;   // callers kept; a station worked or given up on is not listed again for 10 min
    void tickCallers();                                  // fades and expires (a 1 s timer calls it; tests call it with their own clock)
    void setCallersClockForTests(std::function<qint64()> now_ms) { callers_now_ = std::move(now_ms); }
    // Tests only: what opens the direct stream (default open_fw_iso_pcm), and how often a stream held by another program is looked at again.
    using DirectOpener = std::function<std::unique_ptr<fireside::IPcm>(unsigned rate, const fireside::FwIsoOptions& o)>;
    void setDirectOpenerForTests(DirectOpener f, qint64 wait_poll_ms) { direct_opener_ = std::move(f); wait_poll_ms_ = wait_poll_ms; }
    bool qsoPaneAtBottom(int* maximum = nullptr) const;   // the QSO pane is scrolled to its newest line (maximum = the scroll range, 0 while the pane is not laid out)
    QWidget* takeOutputWidget();
    QWidget* takeAdvancedWidget();                      // Mirror I/Q, IF offset, Loop file: shown on the Device tab
    void setAntennaWidget(QWidget* body);
    void setHeardNote(const QString& text);
    void clearQso();                                    // the Clear QSO button: empties the QSO pane, forgets the partner (no effect on transmitting)
    QPushButton* clearQsoButton() const { return clear_qso_; }             // PSK Reporter: "Heard by 12 (hour), furthest 2300 km" in the status line
    // Mode card: FT8 / FT4 = the band's FT dial frequency (band plan), USB, the wide filter and that decoder; false if the band has none.
    bool selectFt(bool ft4);
    bool selectWspr();                                  // WSPR: the band's WSPR dial (20 m between bands), USB, the wide filter and the WSPR decoder (wsprd); false if the band has none
    static bool isFtIndex(int i) { return i == 4 || i == 5; }   // FT8 / FT4 (the QSO views, callers, reply); WSPR (6) has none of them
    void setMyGridSource(std::function<QString()> f) { my_grid_src_ = std::move(f); }   // my locator, for the WSPR spots' distance
    QCheckBox* autoSidebandBox() const { return auto_sb_; }   // LSB on 160 / 80 / 40 m, USB elsewhere, when the dial enters a band in USB / LSB               // the antenna selector card, in the top row                        // the named output box, for the Audio tab               // the transmit power meter card, next to the Signal card at the top
    void showCallInfo(const QString& call, const QString& text);
    QString heardGrid(const QString& call) const;       // the last locator this station sent in a decode (2 h), base call; "" if none: where it is NOW   // callbook text after the recent lines from this station ("  │ Klaus, Ulm, Germany, 770 km"), once per line                       // decoder lines highlighted as addressed to my call (tests)           // decoder output (one or more lines): appended, FT8/FT4 lines to my call highlighted, signals emitted    // tag recent decoder lines from this station: 0 new, 1 new band/mode, 2 worked
    bool logActive() const;                      // the quick log entry (from the Log tab) lives in the control deck here; the card stays hidden until it is set
    // where the audio comes out: 0 = the PC's sound system (default), 1 = the radio's own jacks through the FireWire playback stream on channel pair `pair` (0 = not chosen, 1..4 = ch 0/1, 2/3, 4/5, 6/7).
    // The radio needs a live capture to be open (it names the card) and a chosen pair: see tools/firesideplay and analysis/audio-stream-layout.md section 4. `confirmed` skips the question (tests).
    bool setAudioOutput(int target, int pair, bool confirmed = false);
    int audioOutputTarget() const { return radio_out_ ? 1 : 0; }
    uint64_t radioFramesWritten() const { return radio_out_ ? radio_out_->frames_written() : 0; }
    // text decoders (CW, PSK31, RTTY) on the demodulated audio
    int decoderIndex() const;                           // 0 Off, 1 CW, 2 PSK31, 3 RTTY, 4 FT8, 5 FT4, 6 WSPR
    void setDecoderIndex(int index);                    // also switches the receiver to the matching mode and sets the usual tone
    void setDecoderTone(int hz);                        // CW pitch / PSK31 centre / RTTY mark frequency (audio Hz)
    QString decodedText() const;
    void clearDecoded();
    void setAxisSettling(int index);                     // 0 quick (5 s), 1 normal (20 s), 2 slow (60 s): how fast the spectrum axis narrows again
    int axisSettling() const;
    void setPalette(int index);                         // 0 Classic, 1 Inferno, 2 Viridis, 3 Gray
    int paletteIndex() const;
    void setFloorOffsetDb(int db);
    WaterfallWidget* waterfall() const { return waterfall_; }
    SpectrumWidget* spectrum() const { return spectrum_; }
    // keyboard / memory helpers (the shortcuts and the memory card call these; tests call them directly)
    void stepDial(int steps);                           // +-steps x the tuning step
    void nextBand(int direction);                       // +1 / -1: the next band up / down from the current frequency (with its band memory)
    // Band memory: each band remembers where it was left (frequency, mode, filter, decoder); a band button / nextBand goes back there.
    // A band never visited: its usual start, or its FT8 / FT4 frequency while that decoder is on. The antenna per band is the Antennas tab's.
    struct BandMem { double hz = 0; int mode = 0; double width = 0, low = 150; int decoder = 0; bool set = false; };
    bool recallBand(int index);                         // index into the band buttons (160 = 0 ... 6 = 10)
    int bandIndexOf(double hz) const;                   // -1 between bands
    BandMem bandMemory(int index) const { return index >= 0 && size_t(index) < band_mem_.size() ? band_mem_[size_t(index)] : BandMem{}; }
    double stepHz() const { return step_hz_; }
    struct Memory { QString name; double hz; int mode; double width; double low = 150; };
    int saveMemory(const QString& name);                // current frequency, mode and filter; returns its index
    bool recallMemory(int index);
    void deleteMemory(int index);
    const std::vector<Memory>& memories() const { return memories_; }
    void saveSettings();                                // also called on destruction (unless FIRESIDE_NO_SETTINGS is set)
    void setDialHz(double hz);                          // moves the dial (and the axis); emits dialChanged
    // controls, by index (USB, LSB, AM, CW, FM) and (off, fast, med, slow)
    int modeIndex() const; void setModeIndex(int i);
    int agcIndex() const; void setAgcIndex(int i);
    double filterWidth() const { return filter_width_; }
    // independent passband edges, in audio Hz: SSB low/high cut (USB and LSB use the same positive numbers); AM/CW/FM: pass (0, width)
    void setPassbandCuts(double low, double high); double cutLow() const; double cutHigh() const;
    // manual notches (audio Hz, up to 4): the Notch box is number 1; Alt+click on the spectrum adds or removes one, dragging a marker moves it
    void setNotchesHz(const std::vector<double>& hz); const std::vector<double>& notchesHz() const { return notch_list_; }
    void setFilterShape(int i); int filterShape() const { return filter_shape_; }          // 0 Soft, 1 Normal, 2 Sharp
    void setFilterWidth(double hz);
    // introspection for tests / smoke
    size_t waterfallRows() const;
    double peakOffsetHz() const { return peak_hz_; }
    double signalDbfs() const { return signal_dbfs_; }
    size_t audioSamples() const { return audio_total_; }
    void zoomView(int notches, double anchor);          // + notches = zoom in about `anchor` (fraction of the full span); spectrum and waterfall together
    void setView(double lo, double hi);
    double zoomFactor() const;
    QString status() const;
    QAbstractButton* mirrorBox() const; QAbstractButton* nrBox() const; QAbstractButton* nr2Box() const { return nr2_; }   // NR2: WDSP spectral noise reduction for voice (hidden when not built in)
     QAbstractButton* anfBox() const; QAbstractButton* nbBox() const;
    QSpinBox* notchBox() const { return notch_; } QSpinBox* squelchBox() const { return squelch_; }
    QComboBox* ifBox() const { return if_; }
    FrequencyDisplay* frequencyDisplay() const { return freq_; }
    fireside::RxEngine& engine() { return engine_; }
signals:
    void cqDecoded(const QString& call);                // FT8 / FT4: a station calling CQ (QRZ looks these up, and callers to me)
    void qsoCleared();
    void ftxLine(const QString& line, double dial_hz, const QString& mode);   // FT8 / FT4 decode heard LIVE from the radio (not a recording): PSK Reporter uploads
    void decoderSelected(int index);                    // 0 Off .. 4 FT8, 5 FT4 (the Transmit card's FT8 / FT4 follows)
    void decodedCall(const QString& call);            // FT8/FT4: a station was just decoded (the Log tab can tag it new / worked)
    void decodedLineActivated(const QString& line);
    void callerDecoded(const QString& line);          // FT8/FT4: a decoded line addressed to my call (someone answering me); the line is highlighted   // FT8/FT4: the user double-clicked a decoded line
    void preampRequested(bool on);                      // user pressed Preamp: the RadioPanel asks the session (SET_TRX_PREAMP 0/1 only)
    void rateRequested(unsigned rate);                  // the Source card's 48 / 96 / 192 buttons: MainWindow hands it to the Radio tab (starts the radio if needed, one confirmation, then the stream at that rate)
    void stopRadioRequested();                           // the Source card's Stop radio button
    void dialChanged(double hz);
    // WSPR: a two-minute slot heard LIVE was decoded (date yymmdd, hhmm, the dial in MHz, one formatted line per spot; ok false: the decoder failed, why in error)
    void wsprSlot(const QString& date, const QString& hhmm, double dial_mhz, const QStringList& lines, bool ok, const QString& error);
    void wsprCall(const QString& call);                 // WSPR: a station spotted (after its slot's decode): looked up on QRZ like a CQ caller; the answer goes on its line
    void receiveLost(const QString& why);               // the live radio stream stopped by itself (no data, bus reset, device error): automatic transmit must stop                        // user changed the dial (digits, band button, click on the spectrum)
protected:
    bool eventFilter(QObject* o, QEvent* e) override;
private slots:
    void openFile();
    void settingsChanged();
private:
    QHash<QString, QPair<QString, qint64>> heard_grids_;  // base call -> (grid it sent, UTC s)
    QPushButton* wspr_btn_ = nullptr; std::shared_ptr<std::atomic<double>> wspr_dial_ = std::make_shared<std::atomic<double>>(0.0); std::function<QString()> my_grid_src_;
    QString wsprDistance(const QString& line) const;   // "  1234 km" from my grid to the spot's, or ""
    fireside::RxEngine engine_; bool threaded_; AudioOut* audioOut_; AudioOut* data_out_ = nullptr; float data_gain_ = 0.25f; QCheckBox* ext_box_ = nullptr; bool external_ = false;
    SpectrumWidget* spectrum_; WaterfallWidget* waterfall_; FrequencyDisplay* freq_; SMeter* meter_; QLabel *meter_text_, *mode_chip_, *status_, *source_label_;
    QButtonGroup *bands_, *modes_, *agcs_, *filters_, *steps_; QHBoxLayout* filter_row_; QBoxLayout* log_slot_ = nullptr; QWidget* log_card_ = nullptr; QBoxLayout* form_slot_ = nullptr; QWidget* form_card_ = nullptr; QWidget *band_card_ = nullptr, *mode_card_ = nullptr; QHBoxLayout* deck_ = nullptr;
    QPushButton *play_, *nb_, *nr_, *nr2_ = nullptr, *anf_, *mute_; QSpinBox *notch_, *squelch_; QSlider* volume_;
    QPointer<QPushButton> mirror_; QPointer<QCheckBox> loop_; QPointer<QComboBox> if_;   // these live on the Device tab (takeAdvancedWidget), which may be deleted first at exit
    QComboBox *out_target_, *out_pair_, *settle_; QComboBox* out_dest_ = nullptr; void syncOutDest(); void setRadioOutputsAvailable(bool on); std::unique_ptr<fireside::RadioAudioOut> radio_out_; QString live_device_; double live_rate_ = 48000;
    bool source_live_ = false, mirror_live_ = true, mirror_file_ = false;   // Mirror I/Q is remembered per kind of source
    struct Caller { QString call, line, info; int tag = -1; qint64 ms = 0; bool manual = false; };   // manual: queued by hand (double-click during a QSO), not because it called us          // info: the callbook text, tag: 0 NEW / 1 new band-mode / 2 worked, 3..9 fireside::award tags (as on the decoder lines)
    QHash<QString, QString> info_by_call_; QHash<QString, int> tag_by_call_;
    QFrame* callers_ = nullptr; QVBoxLayout* queue_rows_ = nullptr; QLabel* queue_title_ = nullptr; std::function<bool()> qso_active_; bool qsoActiveNow() const { return qso_active_ && qso_active_(); } QWidget* dec_left_ = nullptr; QList<Caller> caller_list_; std::function<qint64()> callers_now_;
    void addCaller(const QString& call, const QString& line); void removeCaller(const QString& call); void renderCallers(); qint64 callersNow() const;
    QTimer* ui_; QComboBox* mem_combo_; QComboBox* dec_mode_; QSpinBox* dec_tone_; QPlainTextEdit* dec_text_; QPlainTextEdit* qso_text_ = nullptr; QWidget* dec_split_ = nullptr; QHBoxLayout* header_ = nullptr; QString qso_partner_, logged_note_; bool tx_on_dial_ = false; QPushButton *ft8_btn_ = nullptr, *ft4_btn_ = nullptr; QCheckBox* auto_sb_ = nullptr; int last_band_idx_ = -2; bool recalling_ = false; QPushButton* clear_qso_ = nullptr; QString heard_note_; QWidget* advanced_ = nullptr; void syncFtButtons(); void addQsoLine(const QString& line, int kind); QLabel* dec_status_; QFrame* dec_card_; QPushButton* tx_btn_ = nullptr; QPushButton* preamp_ = nullptr; QWidget* tx_w_ = nullptr; QWidget* tx_side_ = nullptr; QButtonGroup* grp_mode_ = nullptr; QStackedWidget* mode_pages_ = nullptr; QPushButton *psk_btn_ = nullptr, *rtty_btn_ = nullptr; QWidget* dec_row_ = nullptr; QVBoxLayout* mode_switch_col_ = nullptr; QGridLayout* meters_grid_ = nullptr; QWidget* tx_meter_ = nullptr; struct DoneQso { int first = 0, last = 0; qint64 ms = 0; std::vector<QColor> bg, fg; QString call; }; bool addToDoneQso(const QString& call, const QString& line); QList<DoneQso> done_qsos_; int qso_start_block_ = -1; QHash<QString, QPair<double, qint64>> focus_; void noteFocus(const QString& call, const QString& line); void pushFocus(); bool cal_valid_ = false; std::array<bool, 11> chk_valid_{}; std::array<double, 11> chk_meter_{}, chk_preamp_{}; std::atomic<double> chk_dbfs_{-200}, chk_peak_hz_{0}; std::array<double, 11> cal_meter_{}, cal_preamp_{}; QTimer* tx_meter_hold_ = nullptr; void showTxMeter(bool tx); QFrame *send_card_ = nullptr, *voice_card_ = nullptr, *signal_card_ = nullptr; int last_voice_mode_ = 0, last_digi_ = 4; void applyGroup(int g); void syncGroup(); void chooseGroup(int g); QComboBox* palette_; QComboBox* avg_ = nullptr; QLineEdit* my_call_ = nullptr; QComboBox* am_det_ = nullptr; QPushButton *rate_btn_[3] = {nullptr, nullptr, nullptr}, *stop_radio_ = nullptr; struct HeardFtx { qint64 ms; double hz; QString label; }; QList<HeardFtx> heard_ftx_; QHash<QString, qint64> moved_on_; QSet<QString> kept_out_said_; unsigned direct_rate_ = 0; bool restart_pending_ = false, rx_lost_sent_ = false, tx_mute_ = false; std::atomic<double> speaker_db_{-200}; double tx_dds_below_ = 0; QFrame* cw_card_ = nullptr; int restarts_ = 0; qint64 first_restart_ms_ = 0; void restartDirect(const QString& why); std::vector<float> row_acc_; int row_n_ = 0; double row_t_ = 0; unsigned wait_rate_ = 0; bool wait_retry_ = false; DirectOpener direct_opener_; qint64 wait_poll_ms_ = kWaitPollMs; void waitForStream(unsigned rate, qint64 until_ms); QString rx_ant_text_; RecordController* rec_ = nullptr; QSlider* gain_ = nullptr; QLabel* gain_label_ = nullptr; QPushButton *grab_btn_ = nullptr, *rec_btn_ = nullptr; void updateRecordButtons(); QPushButton *zoom_in_ = nullptr, *zoom_out_ = nullptr, *zoom_full_ = nullptr; QCheckBox* bandplan_; QSlider* floor_; std::vector<Memory> memories_; std::vector<BandMem> band_mem_; bool band_recall_ = false; void noteBand(); bool restoring_ = false; double ref_hz_ = 0; bool tune_sw_ = false;
    bool playing_ = false; std::vector<double> notch_list_; double shift_acc_ = 0; double filter_width_ = 2700, filter_low_ = 150; int filter_shape_ = 1; QSpinBox *cut_lo_ = nullptr, *cut_hi_ = nullptr; QLabel *cut_lo_label_ = nullptr, *cut_hi_label_ = nullptr; QComboBox* shape_box_ = nullptr;
    double step_hz_ = 100;
    std::shared_ptr<fireside::txo::IqTap> tx_tap_; bool tx_view_ = false; double tx_view_below_ = 0, tx_k_ = 0, tx_an_rate_ = 0, tx_row_t_ = 0; size_t rx_bins_ = 0;
    std::unique_ptr<fireside::dsp::SpectrumAnalyzer> tx_an_; std::vector<std::complex<float>> tx_buf_; std::vector<std::vector<float>> tx_pending_; std::vector<float> tx_last_; void drawTx(double rate);
    fireside::display::Scale scale_; fireside::display::ScaleTracker scale_tracker_; double peak_hz_ = 0, peak_db_ = -200, signal_dbfs_ = -200; size_t audio_total_ = 0;
    QString note_; std::vector<float> audio_buf_;

    void applySettings(); void updateStatus(); void adopt(std::unique_ptr<fireside::IqSource> s, bool nineKhz, bool live = false); void rebuildFilterButtons();
    QFrame* card(const QString& title, QLayout* body);
    void syncFilterEdits(); void cutsEdited(); void syncNotchBox(); void updateSpectrumNotches(); double rfToAudio(double off) const; double audioToRf(double f) const;
    void loadSettings(); void refreshMemoryCombo(); void rebuildDecoder(); void outputControlsChanged(); void forgetLiveDevice();
};
