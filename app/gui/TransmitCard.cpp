#include "TransmitCard.h"
#include "CwSidetone.h"
#include "PcAudioFeed.h"
#include "fireside/dxcc.h"
#include "fireside/siggen.h"
#include "fireside/preamp.h"
#include <QCheckBox>
#include <QDialog>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QFile>
#include <QFileInfo>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QSpinBox>
#include <QMessageBox>
#include <QProgressBar>
#include <QRegularExpression>
#include <QPushButton>
#include <QSettings>
#include <QTextStream>
#include <QStandardPaths>
#include <QTimer>
#include <QElapsedTimer>
#include <QTimeZone>
#include <QVBoxLayout>
#include <chrono>
#include <cmath>
#include <fstream>
#include <sstream>
#include "PowerMeter.h"
#include "AudioPanel.h"
#include <QApplication>
#include <QKeyEvent>
#include <QPlainTextEdit>
#include <QPointer>
#include <QAbstractSpinBox>
#include "RadioPanel.h"
#include "ReceiverPanel.h"
#include "Theme.h"
#include "fireside/fw_iso.h"
#include "fireside/logbook.h"
#include "fireside/session.h"
#include "fireside/tx_dsp.h"
#include "fireside/tx_render.h"
#include "fireside/tx_meter.h"

using namespace fireside;
namespace {
bool no_settings() { return qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS"); }
QString cfg_dir() { return QDir::homePath() + "/.config/fireside"; }
QString mem_path() { return cfg_dir() + "/atu-memory.txt"; }
QString drive_path() { return cfg_dir() + "/tx-drive.txt"; }
// The simulated radio has no FireWire stream: a playback device that takes everything (at about real time).
struct NullPcm : IPcmOut {                               // ... and tells the simulated PA the level on ch0 (QSE I)
    double r; std::atomic<double>* level; NullPcm(double rate, std::atomic<double>* l) : r(rate), level(l) {}
    int channels() const override { return 8; } double rate() const override { return r; } std::string describe() const override { return "simulated playback"; }
    size_t write(const int32_t* x, size_t n) override {
        if (level) { double pk = 0; for (size_t f = 0; f < n; ++f) pk = std::max(pk, std::fabs(double(x[f * 8]) / 2147483647.0)); level->store(pk); }
        std::this_thread::sleep_for(std::chrono::microseconds(int64_t(1e6 * double(n) / r))); return n;
    }
};
QString dice_card() {                                    // "MyProduct" from /proc/asound/cards ("2 [MyProduct ]: DICE - MyProduct")
    QFile f("/proc/asound/cards"); if (!f.open(QIODevice::ReadOnly)) return {};
    for (const QString& l : QString::fromUtf8(f.readAll()).split('\n')) if (l.contains("DICE") || l.contains("FLEX-5000")) {
        const int a = l.indexOf('['), b = l.indexOf(']'); if (a >= 0 && b > a) return l.mid(a + 1, b - a - 1).trimmed(); }
    return {};
}
QString fmt_w(double w) { return QString::number(w, 'f', w < 10 ? 2 : 1) + " W"; }
}  // namespace

TransmitCard::TransmitCard(RadioPanel* radio, ReceiverPanel* rx, bool simulated, QWidget* parent) : QFrame(parent), radio_(radio), rx_(rx), simulated_(simulated) {
    auto* g = new QGridLayout; g->setHorizontalSpacing(10); g->setVerticalSpacing(6);
    arm_ = new QPushButton("Retry transmit check"); arm_->setCheckable(true); arm_->setVisible(false); auto_arm_ = !simulated_;
    arm_->setToolTip("Reads the radio's calibration EEPROM (read-only, ~16 s), compares it with your backup byte for byte, then sets the PA bias from it. Nothing transmits.");
    lamp_ = new QLabel("TX"); lamp_->setAlignment(Qt::AlignCenter); lamp_->setMinimumWidth(44);
    power_ = new QDoubleSpinBox; power_->setRange(1, 15); power_->setDecimals(0); power_->setValue(3); power_->setSuffix(" W"); power_->setToolTip("ATU tune power. 3 W (about 4 W into a match) tuned as well as 15 W on 2026-11-19.");
    thr_ = new QComboBox; for (double t : atu::kThresholds) thr_->addItem(QString("SWR %1").arg(t, 0, 'f', 1), t); thr_->setCurrentIndex(2);
    thr_->setToolTip("The tuner's good-tune threshold (sent right before every tune)");
    tune_ = new QPushButton("Tune ATU"); tune_->setToolTip("Transmits a carrier (up to 15 s) while the ATU searches. Asks first: listen to the frequency.");
    measure_ = new QPushButton("Measure drive (2 W)"); measure_->setToolTip("A 2 W tone ramp (about 4 s) that measures this band's drive level. Needed once per band before tuning there.");
    restore_ = new QPushButton("Restore ATU"); restore_->setToolTip("Puts this band's stored ATU relays back. No RF.");
    auto_restore_ = new QCheckBox("on band change"); auto_restore_->setChecked(true); auto_restore_->setToolTip("While armed: restore the stored ATU relays whenever the dial moves to another band (no RF)");
    stop_ = new QPushButton("Stop"); stop_->setObjectName("txstop"); stop_->setToolTip("Unkey now");
    test_ = new QPushButton("Test tone (8 s)"); test_->setToolTip("A steady tone exactly 1 kHz ABOVE the dial for 8 s at the power beside it, to find on another receiver: there = I/Q sign right; 1 kHz BELOW the dial = the mirror is wrong");
    test_w_ = new QDoubleSpinBox; test_w_->setRange(1, 15); test_w_->setDecimals(0); test_w_->setValue(2); test_w_->setSuffix(" W"); test_w_->setToolTip("Test tone power (ramped up to it; unkeys above 1.5 x, or at SWR > 3)");
    mirror_ = new QCheckBox("Mirror TX I/Q"); mirror_->setChecked(true);
    mirror_->setToolTip("The radio needs the transmit I/Q mirrored, like receive: verified on the air 2026-11-19 (test tone at dial + 1 kHz on a WebSDR). Untick only to re-test.");
    fwd_ = new QProgressBar; rev_ = new QProgressBar; for (auto* b : {fwd_, rev_}) { b->setRange(0, 300); b->setValue(0); b->setTextVisible(true); b->setMinimumWidth(160); }
    fwd_->setFormat("fwd 0 W"); rev_->setFormat("ref 0 W"); swr_ = new QLabel("SWR -"); swr_->setMinimumWidth(80);
    ft_mode_ = new QComboBox; ft_mode_->setObjectName("ftModeBox"); ft_mode_->addItems({"FT8", "FT4"});
    ft_msg_ = new QLineEdit; ft_msg_->setPlaceholderText("CQ YOURCALL GRID"); ft_msg_->setMinimumWidth(220); ft_msg_->setToolTip("The FT8 / FT4 message (standard forms: CQ CALL GRID, CALL CALL REPORT, RR73, 73 ...)");
    ft_hz_ = new QSpinBox; ft_hz_->setRange(200, 3000); ft_hz_->setValue(1500); ft_hz_->setSuffix(" Hz"); ft_hz_->setToolTip("Audio frequency above the dial (USB)");
    ft_par_ = new QComboBox; ft_par_->addItems({"Even (:00 :30)", "Odd (:15 :45)"}); ft_par_->setMinimumContentsLength(20); ft_par_->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    ft_w_ = new QDoubleSpinBox; ft_w_->setRange(1, 100); ft_w_->setDecimals(0); ft_w_->setValue(20); ft_w_->setSuffix(" W"); ft_w_->setToolTip("Power. Held by the forward meter during the first 3 s of each over (closed loop)");
    cq_ = new QPushButton("CQ"); cq_->setToolTip("Call CQ (CQ yourcall yourgrid) in the chosen slot; with Auto sequence the first station answering is worked and logged. Stop ends it.");
    pm_ = new PowerMeter;
    send_ = new QPushButton("Send"); send_->setToolTip("Transmit the message in the next slot of the chosen parity (no question: Stop cancels a queued send). Receive pauses only for the over itself.");
    auto_seq_ = new QCheckBox("Auto sequence + log"); auto_seq_->setChecked(true);
    auto_seq_->setToolTip("Double-click a decoded CQ or caller to reply. Each answer moves the QSO on (report, R-report, RR73, 73), repeated up to 6 times; the finished QSO is logged to Wavelog. Send with a CQ message calls CQ the same way.");
    ft_clear_ = new QCheckBox("Clear freq"); ft_clear_->setChecked(true);
    ft_clear_->setToolTip("CQ: if someone is on our transmit frequency (heard in the last 2 minutes), move to the middle of the widest clear gap (300-2700 Hz). A frequency that is already clear is kept.");
    ft_repeat_ = new QCheckBox("Attended CQ"); ft_repeat_->setToolTip("Keep calling while you are here, until Stop: CQ 6 times in one slot, listen for 4 slots, then CQ 6 times in the other slot, and so on. "
        "A message you typed is sent 6 times, then back to CQ. With Auto sequence, a QSO that ends (logged or given up) goes back to CQ, also one you started by double-clicking.");
    att_timer_ = new QTimer(this); att_timer_->setSingleShot(true);
    connect(att_timer_, &QTimer::timeout, this, [this] {                                     // the pause is over: CQ in the other slot (Clear freq looks again)
        if (!att_pause_) return; endAttPause();
        if (!ft_repeat_->isChecked() || !tx_ || !tx_->armed() || worker_running_ || ft_pending_ || rx_hold_ || (seq_ && seq_->active() && seq_->stage() != fireside::seq::Stage::Cq)) return;
        report(QString("Attended CQ: calling in the %1 slot now.").arg(ft_par_->currentIndex() ? "odd" : "even")); cq_->click(); });
    voice_mode_ = new QComboBox; voice_mode_->addItems({"Auto (as the receiver)", "USB", "LSB"}); voice_mode_->setToolTip("Sideband for voice: Auto = the receiver's USB / LSB (its Auto LSB / USB picks it per band), else USB above 10 MHz");
    voice_w_ = new QDoubleSpinBox; voice_w_->setRange(1, 100); voice_w_->setDecimals(0); voice_w_->setValue(20); voice_w_->setSuffix(" W PEP"); voice_w_->setToolTip("Peak envelope power: speech peaks reach it (the processing drives them into the limiter)");
    ptt_ = new QPushButton("PTT (hold)"); ptt_->setToolTip("Hold to talk: an SSB over with the Audio tab's input and processing (time-out 3 min)");
    space_ptt_ = new QCheckBox("Space bar"); space_ptt_->setToolTip("Hold the space bar to talk (not while typing in a box)"); space_ptt_->setChecked(false);
    mic_ptt_ = new QCheckBox("Mic / RCA PTT"); mic_ptt_->setChecked(true); mic_ptt_->setToolTip("The mic's PTT (front socket) or the rear RCA PTT keys an over");
    ext_allow_ = new QCheckBox("Let other programs key (CAT)"); ext_allow_->setChecked(false);
    ext_allow_->setToolTip("WSJT-X, JS8Call, fldigi ... may key the radio over the CAT server (PTT: CAT), sending what they play into \"Fireside TX audio\". "
                           "Off in every new window. Needs the direct stream, a measured drive level for the band; at most 2 minutes per over; unkeys if the program goes quiet for 15 s.");
    ext_w_ = new QDoubleSpinBox; ext_w_->setRange(1, 100); ext_w_->setDecimals(0); ext_w_->setValue(20); ext_w_->setSuffix(" W");
    ext_w_->setToolTip("Power when the other program's audio is at full scale (less if it is quieter; never more)");
    ft_timer_ = new QTimer(this); ft_timer_->setSingleShot(true); connect(ft_timer_, &QTimer::timeout, this, &TransmitCard::startSend);
    mem_ = new QLabel; mem_->setWordWrap(false); status_ = new QLabel("Transmit gets ready by itself once the radio is started (EEPROM check, about 16 s)."); status_->setWordWrap(true);
    auto row = [](std::initializer_list<QWidget*> ws, bool stretch = true) { auto* h = new QHBoxLayout; h->setSpacing(8); for (auto* w : ws) h->addWidget(w); if (stretch) h->addStretch(1); return h; };
    auto* tl = new QLabel("Tune at"); arm_->setMinimumWidth(130);
    const QString bar = "QProgressBar{background:#151b24;border:1px solid #2a3442;border-radius:4px;color:#d8dee8;text-align:center;min-height:18px}"
                        "QProgressBar::chunk{background:%1;border-radius:3px}";
    fwd_->setStyleSheet(bar.arg("#2e7d32")); rev_->setStyleSheet(bar.arg("#c77700")); fwd_->setMinimumWidth(150); rev_->setMinimumWidth(150);
    mem_->setWordWrap(true); status_->setWordWrap(true); ft_msg_->setMinimumWidth(160);
    fwd_->setVisible(false); rev_->setVisible(false); swr_->setVisible(false);       // the power meter lives at the top (powerMeter())
    auto part = [this](std::initializer_list<QLayout*> ls) { auto* w = new QWidget(this); auto* v = new QVBoxLayout(w); v->setContentsMargins(0, 0, 0, 0); v->setSpacing(6); for (auto* l : ls) v->addLayout(l); return w; };
    status_->setWordWrap(false); status_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed); status_->setMinimumWidth(0);          // one clipped line (the tooltip has all of it)
    auto_restore_->setText("on band change");
    stop2_ = new QPushButton("Stop"); stop2_->setObjectName("txstop"); stop2_->setToolTip("Unkey now (the same as the Stop button on the digital message card)"); connect(stop2_, &QPushButton::clicked, this, [this] { stop_->click(); });
    {   // the tuner settings: a window of their own, so the antenna card stays short
        tuner_dialog_ = new QDialog(this, Qt::Tool); tuner_dialog_->setWindowTitle("Tuner settings"); auto* dv = new QVBoxLayout(tuner_dialog_); dv->setSpacing(8);
        auto* cal = new QPushButton("Calibration  \u25B8"); cal->setCheckable(true); cal->setToolTip("Measure drive and the test tone: used once per band, not for everyday operation");
        gen_btn_ = new QPushButton("Check S-meter (internal generator)");
        gen_btn_->setToolTip("Receive only: for about 10 s the receiver listens to the radio's internal signal generator (taken as -24 dBm, as PowerSDR does) instead of the antenna, "
                             "with the preamp off and then on. The result is saved for this band and the S-meter uses it. Nothing is transmitted.");
        connect(gen_btn_, &QPushButton::clicked, this, &TransmitCard::onGenCheck);
        cal_body_ = part({row({measure_, test_, test_w_, mirror_}), row({gen_btn_})}); cal_body_->setVisible(false);
        connect(cal, &QPushButton::toggled, this, [this, cal](bool on) { cal_body_->setVisible(on); cal->setText(on ? QStringLiteral("Calibration  \u25BE") : QStringLiteral("Calibration  \u25B8")); tuner_dialog_->adjustSize(); });
        mem_->setWordWrap(true); mem_->setSizePolicy(QSizePolicy::Preferred, QSizePolicy::Preferred);
        tune2_ = new QPushButton("Tune ATU"); connect(tune2_, &QPushButton::clicked, this, [this] { tune_->click(); });   // the same Tune as on the card (it asks first)
        dv->addLayout(row({tl, power_, thr_, tune2_})); dv->addLayout(row({restore_, auto_restore_})); dv->addWidget(mem_); dv->addLayout(row({cal, stop2_})); dv->addWidget(cal_body_);
        { auto* close = new QPushButton("Close"); close->setDefault(true); connect(close, &QPushButton::clicked, tuner_dialog_, &QDialog::hide); auto* cr = new QHBoxLayout; cr->addStretch(1); cr->addWidget(close); dv->addLayout(cr); }
        tuner_btn_ = new QPushButton("Tuner settings..."); tuner_btn_->setToolTip("Tune power, the SWR limit, Restore ATU, calibration (Measure drive, Test tone)");
        connect(tuner_btn_, &QPushButton::clicked, this, [this] { tuner_dialog_->show(); tuner_dialog_->raise(); tuner_dialog_->activateWindow(); });
        atu_part_ = part({row({tune_, tuner_btn_}, false)}); }
    ext_part_ = part({row({ext_allow_, ext_w_})});                                      // goes on the Audio tab
    digi_part_ = part({row({ft_mode_, ft_hz_, ft_clear_, ft_par_}), row({ft_msg_, ft_w_}, false), row({send_, cq_, ft_repeat_, auto_seq_, stop_}, false)});
    voice_part_ = part({row({voice_mode_, voice_w_, ptt_, space_ptt_, mic_ptt_})});
    { auto* vl = qobject_cast<QVBoxLayout*>(voice_part_->layout()); if (auto* hl = qobject_cast<QHBoxLayout*>(vl->itemAt(0)->layout())) hl->insertWidget(hl->count() - 1, stop3_ = new QPushButton("Stop")); stop3_->setToolTip("Unkey now"); connect(stop3_, &QPushButton::clicked, this, [this] { stop_->click(); }); }
    if (stop3_) stop3_->setObjectName("txstop");                                            // the transmit Stops: a muted red, easy to find
    buildCw();                                                                        // the CW part (TransmitCardCw.cpp)
    buildVox();                                                                       // VOX + monitor under the voice controls (TransmitCardVox.cpp)
    hide();                                                                           // this object only owns the parts: the window places them (Antenna, Transmit, decoder row, voice strip)
    timer_ = new QTimer(this); timer_->setInterval(100); connect(timer_, &QTimer::timeout, this, &TransmitCard::tick); timer_->start();
    loadCheckedCal();
    connect(arm_, &QPushButton::toggled, this, &TransmitCard::onArm);
    connect(tune_, &QPushButton::clicked, this, &TransmitCard::onTune); connect(measure_, &QPushButton::clicked, this, &TransmitCard::onMeasure);
    connect(restore_, &QPushButton::clicked, this, &TransmitCard::onRestore);
    connect(test_, &QPushButton::clicked, this, &TransmitCard::onTestTone);
    connect(mirror_, &QCheckBox::toggled, this, [this] { saveState(); });
    connect(stop_, &QPushButton::clicked, this, [this] { stopEverything("Stop pressed: unkeying, nothing queued."); });
    connect(send_, &QPushButton::clicked, this, &TransmitCard::onSend);
    connect(ext_allow_, &QCheckBox::toggled, this, [this](bool on) {
        ext_allowed_ = on; if (!on) extCancel();                                            // a running over sees it and unkeys
        report(on ? "Other programs may key the radio over CAT (this window only)." : "Other programs can no longer key the radio."); });
    connect(ptt_, &QPushButton::pressed, this, [this] { ptt_btn_ = true; onPttChanged(); });
    connect(ptt_, &QPushButton::released, this, [this] { ptt_btn_ = false; onPttChanged(); });
    qApp->installEventFilter(this);
    ptt_poll_ = new QTimer(this); ptt_poll_->setInterval(50);
    connect(ptt_poll_, &QTimer::timeout, this, [this] {
        if (!mic_ptt_->isChecked() || !tx_ || !tx_->armed()) { if (ptt_mic_) { ptt_mic_ = false; onPttChanged(); } return; }
        const auto bits = radio_->session().read_ptt(); const bool down = bits && (*bits & 0xC);   // bit 3 mic PTT, bit 2 rear RCA PTT
        if (down != ptt_mic_) { ptt_mic_ = down; onPttChanged(); }
    });
    ptt_poll_->start();
    connect(cq_, &QPushButton::clicked, this, [this] {
        if (!haveStation()) return;
        pickClearCqHz();
        ft_msg_->setText("CQ " + rx_->myCall() + " " + myGrid4()); saveState(); onSend(); });
    connect(ft_clear_, &QCheckBox::toggled, this, [this] { saveState(); });
    auto parity_labels = [this] { const bool ft4 = ft_mode_->currentIndex() == 1;
        ft_par_->setItemText(0, ft4 ? "Even (:00 :15 :30 :45)" : "Even (:00 :30)"); ft_par_->setItemText(1, ft4 ? "Odd (:07.5 :22.5 ...)" : "Odd (:15 :45)"); };
    connect(ft_mode_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this, parity_labels] { parity_labels(); saveState(); });
    connect(rx_, &ReceiverPanel::decoderSelected, this, [this](int i) { if ((i == 4 || i == 5) && ft_mode_->currentIndex() != i - 4) ft_mode_->setCurrentIndex(i - 4); });   // send what is being decoded
    connect(ft_msg_, &QLineEdit::editingFinished, this, [this] { saveState(); });
    connect(radio_, &RadioPanel::runningChanged, this, &TransmitCard::onRadioRunning);
    connect(rx_, &ReceiverPanel::dialChanged, this, &TransmitCard::onDial);
    if (!no_settings()) { QSettings q("fireside", "transmit"); power_->setValue(q.value("tune_w", 3).toDouble()); thr_->setCurrentIndex(std::clamp(q.value("thr", 2).toInt(), 0, 6)); auto_restore_->setChecked(q.value("auto_restore", true).toBool()); mirror_->setChecked(q.value("mirror_q", true).toBool());
                           ft_msg_->setText(q.value("ft_msg", ft_msg_->text()).toString()); ft_hz_->setValue(q.value("ft_hz", 1500).toInt()); ft_w_->setValue(q.value("ft_w", 5).toDouble());
                           ft_mode_->setCurrentIndex(q.value("ft_mode", 0).toInt()); ft_par_->setCurrentIndex(q.value("ft_par", 0).toInt()); auto_seq_->setChecked(q.value("auto_seq", true).toBool()); ft_clear_->setChecked(q.value("ft_clear", true).toBool()); }
    connect(auto_seq_, &QCheckBox::toggled, this, [this](bool on) { if (!on && seq_) seq_->stop(); saveState(); });
    connect(power_, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { saveState(); }); connect(thr_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this] { saveState(); });
    connect(auto_restore_, &QCheckBox::toggled, this, [this] { saveState(); });
    last_band_ = txm::band_of(rx_->dialHz());
    updateButtons(); updateMemoryLabel();
}
void TransmitCard::connectTrip() {
    if (!tx_) return;
    QPointer<TransmitCard> me(this);                                                         // the hook runs on the worker thread: hand the reason to the GUI thread
    tx_->set_trip_hook([me](const std::string& why) { if (me) { me->job_tripped_ = true; const QString q = QString::fromStdString(why); QMetaObject::invokeMethod(me.data(), [me, q] { if (me) emit me->safetyTrip(q); }, Qt::QueuedConnection); } });
}
TransmitCard::~TransmitCard() { if (cw_keys_) cw_keys_->stop(); extCancel(); ext_allowed_ = false; if (tx_) tx_->abort(); if (worker_.joinable()) worker_.join(); }

// ---- keying by other programs over CAT --------------------------------------------------------------------------------------------------
struct TransmitCard::ExtPtt : rigctl::PttRequest {
    TransmitCard* c;
    explicit ExtPtt(TransmitCard* t) : c(t) {}
    bool request_ptt(int client, bool on) override { return c->requestExt(client, on); }
    void heartbeat(int client) override { c->extHeartbeat(client); }
};
rigctl::PttRequest* TransmitCard::pttRequest() { if (!ext_) ext_ = std::make_unique<ExtPtt>(this); return ext_.get(); }
static qint64 now_ms() { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
void TransmitCard::extHeartbeat(int client) { if (client == ext_owner_) ext_beat_ms_ = now_ms(); }
bool TransmitCard::extActive() const {                                       // asked by the over's loop on the worker thread
    std::lock_guard<std::mutex> l(ext_m_);
    if (ext_closing_) return false;                                          // voice() unkeys on the first no: stay no until the over has ended
    const bool on = ext_ptt_ && ext_allowed_ && now_ms() - ext_beat_ms_ < ext_watchdog_ms_;
    if (!on) ext_closing_ = true;
    return on;
}
void TransmitCard::extCancel() { std::lock_guard<std::mutex> l(ext_m_); ext_ptt_ = false; ext_rekey_ = false; }
void TransmitCard::extOverEnded(bool may_restart) {
    int again = -1;
    {
        std::lock_guard<std::mutex> l(ext_m_);
        if (!ext_over_) return;                                              // not the program's over
        if (ext_rekey_ && may_restart) again = ext_owner_;
        ext_ptt_ = false; ext_owner_ = -1; ext_over_ = false; ext_closing_ = false; ext_rekey_ = false;   // the program's next T 1 starts a new over
    }
    if (again >= 0) requestExt(again, true);                                 // it re-keyed while its last over was unkeying: all the checks again
}
bool TransmitCard::requestExt(int client, bool on) {                         // GUI thread (the CAT sockets live here)
    if (!on) {                                                               // releasing is never refused; a running over sees it and unkeys
        std::lock_guard<std::mutex> l(ext_m_);
        if (client == ext_owner_) { ext_ptt_ = false; ext_rekey_ = false; }   // also drops a re-key held while the last over closes
        return true;
    }
    auto refuse = [&](const QString& why) { report("Transmit request from another program refused: " + why + "."); return false; };
    if (!ext_allow_->isChecked()) return refuse("\"Let other programs key\" is off");
    if (!tx_ || !tx_->armed()) return refuse("transmit is not ready");
    {
        std::lock_guard<std::mutex> l(ext_m_);
        if (ext_over_) {
            if (client != ext_owner_) return refuse("another program is transmitting");
            if (ext_closing_) ext_rekey_ = true;                             // the over is already unkeying: start a new one when it has ended (extOverEnded)
            else { ext_ptt_ = true; ext_beat_ms_ = now_ms(); }
            return true;
        }
    }
    if (worker_running_ || ft_pending_) return refuse("this app is transmitting or has an over queued");
    const double hz = rx_->dialHz(); const int band = txm::band_of(hz);
    if (band < 0) return refuse("the dial is outside the amateur bands");
    if (!(tx_->k_for(band) > 1000)) return refuse("no measured drive level on this band (press Measure drive first)");
    if (!simulated_ && !fw_iso_duplex_running()) return refuse("it needs the direct FireWire stream (48 / 96 / 192 on the Radio screen) so receive keeps running");
    QString err; voice_in_ = audio_ ? audio_->openExternalInput(&err) : nullptr;
    if (!voice_in_) return refuse("cannot open \"Fireside TX audio\" (" + err + ")");
    txc::VoiceOptions v; v.o.rate = simulated_ ? 48000 : txRate(); v.o.mirror_q = mirror_->isChecked(); v.o.tx_ant = txAnt();
    v.peak_w = ext_w_->value(); v.max_key_s = kExtMaxKeyS; v.chain = tx::data_chain_defaults(); v.lsb = rx_->modeIndex() == 1;
    if (!(v.o.rate > 0)) { voice_in_.reset(); return refuse("no playback rate for transmit"); }
    const double mhz = hz / 1e6; auto* in = voice_in_.get();
    { std::lock_guard<std::mutex> l(ext_m_); ext_owner_ = client; ext_beat_ms_ = now_ms(); ext_ptt_ = true; ext_over_ = true; ext_closing_ = false; ext_rekey_ = false; }
    run(QString("Keyed by another program over CAT: %1 on %2 MHz, up to %3 W").arg(v.lsb ? "LSB" : "USB").arg(mhz, 0, 'f', 4).arg(v.peak_w, 0, 'f', 0), false,
        [this, mhz, v, in](txc::TxControl& t) {
            txc::VoiceResult r;
            try { r = t.voice(mhz, *in, [this] { return extActive(); }, v, [this](const txc::Progress& p) { std::lock_guard<std::mutex> l(m_); last_ = p; have_last_ = true; }); }
            catch (...) { std::lock_guard<std::mutex> l(ext_m_); ext_ptt_ = false; ext_rekey_ = false; throw; }   // a throw is a safety stop: no restart
            bool quiet;
            {
                std::lock_guard<std::mutex> l(ext_m_); ext_closing_ = true;                          // a T 1 from here on is held for extOverEnded
                quiet = ext_ptt_ && now_ms() - ext_beat_ms_ >= ext_watchdog_ms_;
                if (quiet || r.unkey_failed || !r.keyed) ext_rekey_ = false;                         // these never restart by themselves
                ext_ptt_ = false;
            }
            if (r.unkey_failed) { unkey_failed_ = true; return QString("UNKEY FAILED: ") + QString::fromStdString(r.stop); }
            if (!r.keyed) return QString("Other program: not keyed: ") + QString::fromStdString(r.why_not.empty() ? r.stop : r.why_not);
            QString stop = QString::fromStdString(r.stop); if (quiet) stop = "the program stopped talking to us for 15 s";
            if (quiet) QMetaObject::invokeMethod(this, [this] { emit safetyTrip("the other program stopped talking to us for 15 s"); }, Qt::QueuedConnection);
            return QString("Other program's over: %1, %2 s, peak %3 W (set %4 W)").arg(stop).arg(r.seconds, 0, 'f', 1).arg(r.max_fwd_w, 0, 'f', 1).arg(v.peak_w, 0, 'f', 0);
        });
    if (!worker_running_) { { std::lock_guard<std::mutex> l(ext_m_); ext_ptt_ = false; ext_owner_ = -1; ext_over_ = false; } voice_in_.reset(); return refuse("the transmit worker did not start"); }
    return true;
}

void TransmitCard::setSimulatedRadio(sim::RadioModel* m) {
    sim_radio_ = m; if (!m) return;
    const QString p = findBackup(); QFile f(p);
    if (!p.isEmpty() && f.open(QIODevice::ReadOnly)) {            // the simulated radio carries the same EEPROM as the backup, so Arm works in practice mode
        const QByteArray a = f.readAll(); m->eeprom.assign(a.begin(), a.end());
        const auto c = txs::from_image(m->eeprom); if (c.valid) { m->meter = c.meter; m->pa_from_level = true; }
    }
}
QString TransmitCard::findBackup() {
    if (!no_settings()) { QSettings q("fireside", "transmit"); const QString p = q.value("backup").toString(); if (!p.isEmpty() && QFileInfo::exists(p)) return p; }
    QDir d(backupDir()); const auto l = d.entryInfoList({"F5K_*_trx.bin"}, QDir::Files, QDir::Time);
    return l.isEmpty() ? QString() : l.first().absoluteFilePath();
}
QString TransmitCard::backupDir() {
    const QString e = qEnvironmentVariable("FIRESIDE_BACKUP_DIR"); if (!e.isEmpty()) return e;
    return QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/fireside/backups";
}
double TransmitCard::playbackRate() {
    const QString card = dice_card(); if (card.isEmpty()) return 0;
    for (int n = 0; n < 16; ++n) {
        QFile c(QString("/proc/asound/card%1/id").arg(n)); if (!c.open(QIODevice::ReadOnly) || QString::fromUtf8(c.readAll()).trimmed() != card) continue;
        QFile f(QString("/proc/asound/card%1/firewire/formation").arg(n)); if (!f.open(QIODevice::ReadOnly)) return 0;
        for (const QString& l : QString::fromUtf8(f.readAll()).split('\n')) if (l.trimmed().startsWith("Rx 0:")) {
            const auto p = l.trimmed().mid(5).split(QRegularExpression("\\s+"), Qt::SkipEmptyParts);
            if (p.size() >= 3) { if (p[0].toInt() >= 8) return 48000; if (p[1].toInt() >= 8) return 96000; if (p[2].toInt() >= 8) return 192000; }
        }
    }
    return 0;
}
txc::PcmFactory TransmitCard::pcmFactory() const {
    if (simulated_) { std::atomic<double>* lv = sim_radio_ ? &sim_radio_->level : nullptr; return [lv](double rate) { return std::unique_ptr<IPcmOut>(new NullPcm(rate, lv)); }; }
    const std::string dev = "hw:CARD=" + dice_card().toStdString() + ",DEV=0";
    return [dev](double rate) -> std::unique_ptr<IPcmOut> {
        double r = 0; if (fw_iso_duplex_running(&r) && r == rate) return open_fw_iso_out();   // duplex: our own FireWire stream, receive keeps running
        return open_alsa_play(dev, rate, 8, 1024); };
}

bool TransmitCard::ask(const QString& title, const QString& text) {
    if (auto_confirm_ >= 0) return auto_confirm_ == 1;
    return QMessageBox::question(this, title, text, QMessageBox::Yes | QMessageBox::No, QMessageBox::No) == QMessageBox::Yes;
}
void TransmitCard::report(const QString& m) { status_->setText(m); emit message("Transmit: " + m); }
QString TransmitCard::statusText() const { return status_->text(); }

void TransmitCard::run(const QString& what, bool pauses_receive, std::function<QString(txc::TxControl&)> job) {
    if (worker_running_ || !tx_) return;
    if (worker_.joinable()) worker_.join();
    worker_running_ = true; job_tripped_ = false; { std::lock_guard<std::mutex> l(m_); have_last_ = false; }
    if (pauses_receive) pauseReceive();                                                      // the ALSA playback and our receive stream cannot share the radio
    report(what + " ..."); updateButtons();
    auto tx = tx_;
    worker_ = std::thread([this, tx, job] {
        QString res; bool threw = false; try { res = job(*tx); } catch (const std::exception& e) { res = QString("failed: ") + e.what(); threw = true; }
        const bool tripped = job_tripped_.exchange(false) || threw;                             // the core's trip hook fired for this job (or it threw)
        job_end_utc_ = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();   // the playback is closed by now
        QMetaObject::invokeMethod(this, [this, res, tripped]() mutable {
            if (worker_.joinable()) worker_.join();
            worker_running_ = false;
            const QString rxnote = resumeReceive(); { QSignalBlocker b(arm_); arm_->setChecked(tx_ && tx_->armed()); }   // a refused arm springs back
            const bool sent_ok = res.endsWith(" #sent");
            // A safety stop ends the sequence and any repeat: nothing is retried by itself. Decided by the core's own trip signal (TxControl::set_trip_hook,
            // from the gate's stop reason) or an exception, never by reading the status text (an S-meter check or ATU restore message may say "failed").
            const bool safety = (!sent_ok && tripped) || rx_hold_;                                  // receive lost: ends like a safety stop (nothing resumes by itself)
            if (safety) {
                { QSignalBlocker b(ft_repeat_); ft_repeat_->setChecked(false); } ft_timer_->stop(); ft_pending_.reset();
                follow_pending_ = false; auto_tried_.clear(); from_cq_ = false; if (seq_) seq_->stop();
                res += rx_hold_ && !tripped ? "  Receive stopped: auto transmit stays off until you press CQ or Send, or double-click a station." : "  Stopped: nothing is retried after a safety stop.";
            }
            if (sent_ok) takeLog();                                                                  // the over that completed a QSO has gone out: log it
            noteSeqEnd(!safety);                                                                     // after a safety stop: announce the end, but never go on to the next caller
            bool resend = !safety && (sent_ok && ft_repeat_->isChecked() && !(seq_ && seq_->stage() != fireside::seq::Stage::Idle)) || (seq_ && seq_->active() && !res.startsWith("Not sent"));
            bool att_pause = false, att_cq = false;                                             // Attended CQ: 6 calls, then a pause (CQ) or back to CQ (a typed message)
            if (!safety && sent_ok && ft_repeat_->isChecked()) {
                const bool plain = !(seq_ && seq_->stage() != fireside::seq::Stage::Idle), seq_cq = seq_ && seq_->stage() == fireside::seq::Stage::Cq;
                if (seq_cq || (plain && last_sent_.startsWith("CQ "))) { if (++att_count_ >= att_calls_) { resend = false; att_pause = true; } }
                else if (plain && ++att_count_ >= att_calls_) { resend = false; att_cq = true; }
            }
            if (seq_ && !seq_->describe().empty()) res += "  " + QString::fromStdString(seq_->describe()) + ".";
            if (res.endsWith(" #sent")) res.chop(6);
            report(res + rxnote); saveState(); updateButtons(); updateMemoryLabel();
            if (resend) QTimer::singleShot(200, this, [this, g = stop_gen_] { if (g == stop_gen_) queueSend(); });   // Stop in between cancels it             // calling: the next slot of the same parity
            if (!resend && !safety) QTimer::singleShot(300, this, [this] { autoNext(); });               // an ended QSO waiting for this over to finish
            if (att_pause) startAttPause();
            if (att_cq) QTimer::singleShot(300, this, [this, g = stop_gen_] {
                if (g != stop_gen_ || !ft_repeat_->isChecked() || worker_running_ || ft_pending_ || (seq_ && seq_->active())) return;
                report(QString("Attended CQ: \"%1\" was sent %2 times, back to CQ.").arg(last_sent_).arg(att_calls_)); cq_->click(); });
            voice_in_.reset(); vox_over_in_.reset();                                            // a voice over's input (a PC capture lives on this thread; the worker has been joined)
            if (after_arm_) { auto d = std::move(after_arm_); after_arm_ = nullptr; d(); }               // start-up: receiving starts once the check is done
            extOverEnded(!safety);                                                                  // a program re-keyed while its last over was unkeying
            if (unkey_failed_) { unkey_failed_ = false; if (auto_confirm_ < 0) QMessageBox::critical(this, "The radio may still be transmitting",
                "The unkey command did not get through (three tries, then 2 s of retries).\n\nPower the radio off now (front panel), then check the FireWire connection.\n\n" + res); }
        }, Qt::QueuedConnection);
    });
}

double TransmitCard::txRate() { double r = 0; return fw_iso_duplex_running(&r) ? r : playbackRate(); }   // duplex: the stream's rate
void TransmitCard::pauseReceive() {
    if (simulated_ || paused_live_) return;
    if (fw_iso_duplex_running()) { rx_->peekDecode(); return; }                              // duplex: receive keeps running; just read the slot so far
    paused_rate_ = rx_->liveRate(); paused_live_ = rx_->closeLive();                        // closeLive decodes the slot heard so far first (an answer to us)
}
QString TransmitCard::resumeReceive() {
    QString rxnote;
    if (paused_live_ && !simulated_) {                                                       // receive back as it was, and say so
        auto utc = [] { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); };
        const double t_resume = utc();
        QElapsedTimer t; t.start(); const bool ok = paused_rate_ ? rx_->loadLiveDirect(paused_rate_) : rx_->openLiveAuto();
        const double now = utc(), slot = ft_mode_->currentIndex() == 1 ? 7.5 : 15.0;
        QString phases;
        if (unkeyed_utc_.load() > 0) {                                                        // where the time went (status + the message log)
            const double a = job_end_utc_.load() - unkeyed_utc_.load(), b = t_resume - job_end_utc_.load(), c = now - t_resume;
            phases = QString(" After the over: unkey to playback closed %1 s, to GUI %2 s, stream restart %3 s, total %4 s.").arg(a, 0, 'f', 2).arg(b, 0, 'f', 2).arg(c, 0, 'f', 2).arg(a + b + c, 0, 'f', 2);
            emit message("Transmit:" + phases);
            unkeyed_utc_ = 0;
        }
        const double into = std::fmod(now, slot), late = into < slot / 2 ? into : into - slot;   // seconds after (+) or before (-) a slot boundary
        const QString when = QString(" (stream restart %1 s, %2 s %3 the slot boundary%4)").arg(t.elapsed() / 1000.0, 0, 'f', 1).arg(std::fabs(late), 0, 'f', 1).arg(late < 0 ? "before" : "after")
                             .arg(late > 0.4 ? ": that slot is lost" : "");
        rxnote = ok ? (paused_rate_ ? QString(" Receive resumed (%1 kHz direct)").arg(paused_rate_ / 1000) + when + "." : QString(" Receive resumed") + when + ".")
                    : QString(" Receive did NOT resume: press 48 / 96 / 192 in the Source card.");
        rxnote += phases;
    }
    paused_rate_ = 0; paused_live_ = false; return rxnote;
}
QString TransmitCard::myGrid4() {
    QString grid = my_grid_ ? my_grid_() : QString();                                       // the Wavelog station profile's grid
    if (grid.size() < 4) { const QStringList w = ft_msg_->text().split(' ', Qt::SkipEmptyParts); if (w.size() >= 3 && w[0] == "CQ" && w.last().size() == 4) grid = w.last(); }   // else the grid in our CQ
    if (grid.size() < 4) return QString();                                                   // unknown: never a made-up locator
    return grid.left(4).toUpper();
}
bool TransmitCard::haveStation() {
    if (!rx_->myCall().isEmpty() && !myGrid4().isEmpty()) return true;
    report("Set your station first: your callsign in the Decoder card's My call box (or the Log tab), and your 4-character locator in the Log tab's station "
           "profile or by typing a CQ here once (CQ YOURCALL GRID). Nothing was sent.");
    return false;
}
fireside::seq::Sequencer& TransmitCard::sequencer() {
    const QString grid = myGrid4();
    const QString me = rx_->myCall();                                                       // callers have checked haveStation(): never another station's call
    if (!seq_ || QString::fromStdString(seq_->describe()).isEmpty())
        seq_ = test_tries_ > 0 ? std::make_unique<fireside::seq::Sequencer>(me.toStdString(), grid.left(4).toUpper().toStdString(), test_tries_)
                               : std::make_unique<fireside::seq::Sequencer>(me.toStdString(), grid.left(4).toUpper().toStdString());
    return *seq_;
}
namespace {
// the slot parity a decoder line was heard in, from its HHMMSS label (S000-style labels of a file: index = number)
int line_parity(const QString& line, bool ft4) { return fireside::seq::line_parity(line.toStdString(), ft4); }
}  // namespace
void TransmitCard::pickClearCqHz() {
    if (!ft_clear_->isChecked()) return;
    const bool ft4 = ft_mode_->currentIndex() == 1; const int mine = ft_par_->currentIndex();
    std::vector<fireside::seq::HeardSignal> heard;
    for (const auto& h : rx_->recentFtx(ft4 ? 60000 : 120000)) heard.push_back({h.first, line_parity(h.second, ft4)});   // the last 8 slots
    const int was = ft_hz_->value(); const int now = int(std::lround(fireside::seq::clear_tx_hz(heard, mine, was, ft4)));
    if (now != was) { ft_hz_->setValue(now); emit message(QString("Transmit: CQ moved from %1 Hz to %2 Hz, clear of the %3 station(s) heard").arg(was).arg(now).arg(heard.size())); }
}
QString TransmitCard::qsoText() const { return seq_ ? QString::fromStdString(seq_->describe()) : QString(); }
QString TransmitCard::messageText() const { return ft_msg_->text(); }
double TransmitCard::recentTxPowerW() const { return QDateTime::currentSecsSinceEpoch() - last_tx_t_.load() < 600 ? last_tx_w_.load() : 0.0; }
void TransmitCard::replyTo(const QString& line) {
    if (!auto_seq_->isChecked()) return;
    if (!auto_reply_) { auto_tried_.clear(); from_cq_ = false; follow_pending_ = false; rx_hold_ = false; }   // the operator's own choice: a fresh start
    int snr = 0; const std::string text = fireside::logbook::ftx_line_text(line.toStdString(), &snr); const auto h = fireside::seq::parse(text, snr);
    if (!h.ok) { report("That line has nothing to answer."); return; }
    if (!auto_reply_ && !fireside::decode::callsign_prefix_known(h.from)) {                // the operator's double-click on a callsign no country uses: only on a second click
        const QString who = QString::fromStdString(h.from); const qint64 now = QDateTime::currentMSecsSinceEpoch();
        if (who != odd_call_ || now - odd_call_ms_ > 15000) { odd_call_ = who; odd_call_ms_ = now; report(QString("%1 has a prefix no country uses: most likely a false decode. Double-click it again to answer anyway.").arg(who)); return; }
        odd_call_.clear();
    }
    if (!haveStation()) return;
    if (seq_) seq_->stop();
    auto& sq = sequencer();
    if (!sq.reply_to(h)) { report("Nothing to answer in: " + QString::fromStdString(text)); return; }
    { QSignalBlocker b(ft_par_); ft_par_->setCurrentIndex(1 - line_parity(line, ft_mode_->currentIndex() == 1)); }   // our slots are the other parity
    att_count_ = 0; endAttPause();                                                          // Attended CQ stays ticked: when this QSO ends, back to CQ
    ft_msg_->setText(QString::fromStdString(sq.current())); saveState(); ended_for_.clear(); logged_for_.clear(); emit qsoPartner(QString::fromStdString(sq.dx()));
    ft_timer_->stop(); ft_pending_.reset();
    if (tx_ && tx_->armed() && !worker_running_) queueSend(); else report(QString("Reply ready: %1 (%2). Transmit is not ready yet.").arg(ft_msg_->text(), ft_par_->currentText()));
}
void TransmitCard::heardLine(const QString& line) {
    if (!seq_ || !auto_seq_->isChecked()) return;
    int snr = 0; const std::string text = fireside::logbook::ftx_line_text(line.toStdString(), &snr); const auto h = fireside::seq::parse(text, snr);   // two statements: argument order is unspecified
    const bool was_cq = seq_->stage() == fireside::seq::Stage::Cq;
    if (seq_->heard(h)) {
        if (was_cq && seq_->stage() != fireside::seq::Stage::Cq) {                             // someone answered our CQ: afterwards, back to CQ
            from_cq_ = true; ended_for_.clear(); logged_for_.clear();                           // a new QSO, even with a station we just gave up on or worked
            if (att_pause_) {                                                                   // answered during the Attended CQ pause: reply in the slot after theirs
                endAttPause(); { QSignalBlocker b(ft_par_); ft_par_->setCurrentIndex(1 - line_parity(line, ft_mode_->currentIndex() == 1)); } saveState();
                if (!seq_->current().empty()) ft_msg_->setText(QString::fromStdString(seq_->current()));
                if (!ft_pending_ && !worker_running_) queueSend();
            }
        }
        emit qsoPartner(QString::fromStdString(seq_->dx()));
        if (!seq_->current().empty()) ft_msg_->setText(QString::fromStdString(seq_->current()));
        report(QString::fromStdString(seq_->describe()) + (seq_->current().empty() ? QString() : QString(": next ") + QString::fromStdString(seq_->current())));
        if (!seq_->active()) { takeLog(); if (!ft_pending_ && !worker_running_) { ft_timer_->stop(); updateButtons(); } }
        else if (seq_->stage() == fireside::seq::Stage::SentRR73 && seq_->tries() == 0 && !ft_pending_ && !worker_running_ && auto_seq_->isChecked())
            queueSend();                                                                  // they repeated their R-report after we had stopped: RR73 once more
    }
}
void TransmitCard::takeLog() {
    if (!seq_) return;
    if (auto l = seq_->take_log()) {
        emit logQso(QString::fromStdString(l->call), QString::fromStdString(l->grid), QString::fromStdString(l->rst_sent), QString::fromStdString(l->rst_rcvd));
        logged_for_ = QString::fromStdString(l->call);                                  // logged; "done" is said when the sequence really ends (RR73 repeats may follow)
    }
    noteSeqEnd();
}
void TransmitCard::noteSeqEnd(bool may_continue) {      // the sequence has ended: say so once in the QSO pane (done = it was logged; else only if it gave up)
    if (!seq_ || seq_->active()) return;
    const QString dx = QString::fromStdString(seq_->dx()); if (dx.isEmpty() || dx == ended_for_) return;
    const bool logged = dx == logged_for_;
    if (!logged && seq_->stage() != fireside::seq::Stage::GaveUp) return;                // stopped before anything was logged: nothing to announce
    ended_for_ = dx; emit qsoEnded(dx, logged);
    if (may_continue) { follow_pending_ = true; QTimer::singleShot(300, this, [this] { autoNext(); }); }
}
void TransmitCard::autoNext() {                         // auto mode, a QSO (or a 2-try call) has ended: answer the next station waiting in the callers area
    if (rx_hold_ || !follow_pending_ || !auto_seq_->isChecked() || !tx_ || !tx_->armed() || worker_running_ || ft_pending_ || (seq_ && seq_->active())) return;   // (an over still running: the run's end calls this again)
    follow_pending_ = false;
    for (;;) {                                                                               // the newest waiting caller that can be answered; a sign-off (73, RR73) is skipped
        const auto c = rx_->nextCaller(auto_tried_);
        if (c.first.isEmpty()) break;
        auto_tried_.insert(c.first);
        int snr = 0; const auto h = fireside::seq::parse(fireside::logbook::ftx_line_text(c.second.toStdString(), &snr), snr);
        if (!h.ok || h.seventy3 || h.rr73 || h.rrr) continue;
        if (!fireside::decode::callsign_prefix_known(h.from)) { report(QString("Not answering %1 by itself: no country uses that prefix (most likely a false decode)").arg(c.first)); continue; }
        auto_reply_ = true; replyTo(c.second); auto_reply_ = false;
        if (seq_ && seq_->active() && QString::fromStdString(seq_->dx()).contains(c.first)) {
            seq_->limit_tries(kCallerTries); report(QString("Answering %1, who called earlier (%2 tries, then the next one): %3").arg(c.first).arg(kCallerTries).arg(ft_msg_->text()));
            return;
        }
        if (seq_ && seq_->active()) return;                                                  // answering someone (the operator's or this one)
    }
    auto_tried_.clear();
    if (from_cq_ || ft_repeat_->isChecked()) { from_cq_ = false; report(ft_repeat_->isChecked() ? "Attended CQ: no one else is waiting, calling CQ again." : "No one else is waiting: calling CQ again."); cq_->click(); }   // we were calling CQ (or Attended CQ): carry on
}

void TransmitCard::offerBackup() {
    auto& s = radio_->session();
    if (worker_running_ || (tx_ && tx_->armed()) || !findBackup().isEmpty()) return;
    if (s.state() != RadioSession::State::Started || !s.allow_write()) return;
    const QString dir = backupDir();
    if (!ask("Save this radio's calibration?",
             "Transmit needs a saved copy of this radio's calibration EEPROM. Each time the app starts it reads the radio's EEPROM again and compares it with this copy "
             "before setting the PA bias from it, so a corrupted EEPROM can never set the PA's bias.\n\n"
             "Read it now and save it to\n" + dir + "?\n\n"
             "It takes about half a minute (the EEPROM is read twice and both reads must match). It is read-only: nothing is written to the radio's EEPROM. Receive carries on.")) {
        report("No EEPROM backup was saved, so transmit stays off. Press Retry transmit check later to be asked again."); { QSignalBlocker b(arm_); arm_->setChecked(false); } return;
    }
    tx_ = s.transmit(pcmFactory()); connectTrip();
    if (!tx_) { report("The radio is not started."); return; }
    run("Reading the EEPROM twice and saving a backup", false, [this, dir](txc::TxControl& t) {
        const auto log = [this](const std::string& m) { const QString q = QString::fromStdString(m); QMetaObject::invokeMethod(this, [this, q] { emit message("Transmit: " + q); }, Qt::QueuedConnection); };
        const auto b = t.backup_eeprom(dir.toStdString(), log);
        t.arm(b.image, b.image, log); eeprom_verified_ = true;                       // just read twice and identical: that is the reference for this run
        { const auto cal = rx_level_cal(decode_eeprom(b.image, "TRX")); QMetaObject::invokeMethod(this, [this, cal] { applyRxCal(cal); }, Qt::QueuedConnection); }   // the S-meter's factory table, from the image just verified
        loadState();
        return QString("EEPROM read twice (identical) and saved to %1. Transmit ready: PA bias set.").arg(b.paths.empty() ? dir : QString::fromStdString(b.paths.front()));
    });
}
void TransmitCard::applyRxCal(const RxLevelCal& c) {
    rx_cal_ = c;
    rx_->setRxLevelCal(c.valid, c.meter, c.preamp);
    emit message(c.valid ? QString("S-meter: using the radio's factory RX level table (20 m: meter %1 dB in PowerSDR units = %2 dB here, preamp %3 dB)").arg(c.meter[5], 0, 'f', 1).arg(c.meter[5] + ReceiverPanel::kTableToDbfsDb, 0, 'f', 1).arg(c.preamp[5], 0, 'f', 1)
                         : QString("S-meter: the RX level table is not usable (%1); the meter stays uncalibrated").arg(QString::fromStdString(c.problem)));
}
void TransmitCard::onGenCheck() {
    if (!tx_ || !tx_->armed() || worker_running_ || ft_pending_) { report("The S-meter check needs the transmit check done and nothing else running."); return; }
    const QString mode = rx_->modeName(); const int band = txm::band_of(rx_->dialHz());
    if (band < 0) { report("The dial is outside the amateur bands."); return; }
    if (mode != "USB" && mode != "LSB") { report("Switch the receiver to USB or LSB for the S-meter check (the generator is put 1 kHz inside the passband)."); return; }
    if (!ask("Check the S-meter with the internal generator?",
             "Receive only: nothing is transmitted.\n\nFor about 10 seconds the receiver listens to the radio's built-in signal generator instead of the antenna "
             "(PowerSDR's \"Enable HW Signal Generator\" and its RX level calibration), with the preamp off and then on. PowerSDR takes the generator as -24 dBm.\n\n"
             "The result is saved for this band and the S-meter uses it from then on.")) return;
    const double dial = rx_->dialHz() / 1e6, tone = mode == "LSB" ? -1000.0 : 1000.0; const bool pre = radio_->session().preamp_on();
    ReceiverPanel* rx = rx_; RadioPanel* radio = radio_;
    auto read = [rx] { return rx->levelForChecks(); };                                       // atomics: safe from the worker
    auto preamp = [radio](bool on) { const auto r = radio->session().set_preamp(on); return r == PreampResult::Sent || r == PreampResult::Unchanged; };
    run(QString("Checking the S-meter on %1 with the internal generator (receive only)").arg(txm::band_name(band)), false, [this, dial, tone, read, preamp, pre, band, cal = rx_cal_](txc::TxControl& t) {
        const auto log = [this](const std::string& m) { const QString q = QString::fromStdString(m); QMetaObject::invokeMethod(this, [this, q] { emit message("S-meter check: " + q); }, Qt::QueuedConnection); };
        const auto r = t.sig_gen_check(dial, tone, read, preamp, pre, log);
        QMetaObject::invokeMethod(this, [this, r, band] { applyGenCheck(r, band); }, Qt::QueuedConnection);
        if (!r.ok) return QString("S-meter check: %1.").arg(QString::fromStdString(r.why));
        const double meter = siggen::kLevelDbm - r.on_dbfs;
        QString t2 = QString("S-meter check on %1: the generator (-24 dBm) read %2 dBFS, so the meter offset is %3 dB").arg(txm::band_name(band)).arg(r.on_dbfs, 0, 'f', 1).arg(meter, 0, 'f', 1);
        if (cal.valid) { const double tbl = cal.meter[size_t(band)] + ReceiverPanel::kTableToDbfsDb; t2 += QString(" (factory table %1 dB in our units, difference %2 dB)").arg(tbl, 0, 'f', 1).arg(meter - tbl, 0, 'f', 1); }
        if (r.preamp_measured) { t2 += QString("; preamp gain %1 dB").arg(r.preamp_dbfs - r.on_dbfs, 0, 'f', 1); if (cal.valid) t2 += QString(" (table %1)").arg(-cal.preamp[size_t(band)], 0, 'f', 1); }
        return t2 + ". Saved: the S-meter uses it on this band.";
    });
}
void TransmitCard::applyGenCheck(const txc::TxControl::GenCheck& r, int band) {
    if (!r.restore_error.empty() && auto_confirm_ < 0)
        QMessageBox::critical(this, "Signal generator still on?", "The radio did not confirm switching its internal generator off:\n\n" + QString::fromStdString(r.restore_error) +
                              "\n\nThe receiver may still be listening to the generator instead of the antenna. Stop the radio and power it off and on.");
    if (!r.ok || band < 0 || band >= txm::kBands) return;
    const double meter = siggen::kLevelDbm - r.on_dbfs, pre = r.preamp_measured ? -(r.preamp_dbfs - r.on_dbfs) : std::nan("");
    rx_->setCheckedCal(band, meter, pre);
    if (!qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) {
        QSettings q("fireside", "smeter"); q.beginGroup(txm::band_name(band));
        q.setValue("meter_db", meter); if (r.preamp_measured) q.setValue("preamp_db", pre); else q.remove("preamp_db");
        q.setValue("generator_dbfs", r.on_dbfs); q.setValue("utc", QDateTime::currentDateTimeUtc().toString(Qt::ISODate)); q.endGroup();
    }
}
void TransmitCard::loadCheckedCal() {                     // the S-meter checks saved earlier (per band), used before the EEPROM table
    if (qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) return;
    QSettings q("fireside", "smeter");
    for (int b = 0; b < txm::kBands; ++b) { q.beginGroup(txm::band_name(b));
        if (q.contains("meter_db")) rx_->setCheckedCal(b, q.value("meter_db").toDouble(), q.contains("preamp_db") ? q.value("preamp_db").toDouble() : std::nan(""));
        q.endGroup(); }
}
void TransmitCard::onArm(bool on) {
    if (!on) { if (tx_) tx_->disarm(); report("Transmit disarmed."); updateButtons(); return; }
    auto& s = radio_->session();
    if (s.state() != RadioSession::State::Started || !s.allow_write()) { QSignalBlocker b(arm_); arm_->setChecked(false); report("Start the radio with Allow writes first."); return; }
    const QString bpath = findBackup();
    if (bpath.isEmpty()) {
        { QSignalBlocker b(arm_); arm_->setChecked(false); }
        report("No EEPROM backup yet (looked in " + backupDir() + "), so transmit is off. Receive works as normal.");
        if (auto_confirm_ < 0 || backup_offer_tests_) QTimer::singleShot(400, this, [this] { offerBackup(); });   // asked after receive has started, not before
        return;
    }
    QFile f(bpath); std::vector<uint8_t> backup; if (f.open(QIODevice::ReadOnly)) { const QByteArray a = f.readAll(); backup.assign(a.begin(), a.end()); }
    tx_ = s.transmit(pcmFactory()); connectTrip();
    if (!tx_) { QSignalBlocker b(arm_); arm_->setChecked(false); report("The radio is not started."); return; }
    run("Arming (reading the EEPROM)", false, [this, backup](txc::TxControl& t) {
        // once per app run: the EEPROM is read and compared at start-up only (later arms rely on that check)
        if (eeprom_verified_) t.arm(backup, backup);
        else { t.arm_from_radio(backup, [this](const std::string& m) { const QString s = QString::fromStdString(m); QMetaObject::invokeMethod(this, [this, s] { emit message("Transmit: " + s); }, Qt::QueuedConnection); }); eeprom_verified_ = true; }
        { const auto cal = rx_level_cal(decode_eeprom(backup, "TRX")); QMetaObject::invokeMethod(this, [this, cal] { applyRxCal(cal); }, Qt::QueuedConnection); }   // the S-meter's factory table, from the image just verified
        loadState();
        return QString("Transmit ready: the EEPROM matches the backup, PA bias set.");
    });
}

void TransmitCard::onTune() {
    if (!tx_ || !tx_->armed()) return;
    const double mhz = rx_->dialHz() / 1e6, w = power_->value(), thr = thr_->currentData().toDouble(); const int band = txm::band_of(mhz * 1e6);
    if (band < 0) { report("The dial is outside the amateur bands."); return; }
    if (!ask("Tune the ATU?", QString("Transmit a tune carrier on %1 MHz at about %2 W for up to 15 s while the ATU searches?\n\nListen first: is the frequency clear?").arg(mhz, 0, 'f', 4).arg(w))) return;
    txc::Options o; o.rate = simulated_ ? 48000 : txRate(); o.mirror_q = mirror_->isChecked(); o.tx_ant = txAnt();
    if (!(o.rate > 0)) { report("The radio's ALSA playback offers no 8-channel rate (see /proc/asound/cardN/firewire/formation)."); return; }
    run(QString("Tuning on %1 MHz at %2 W").arg(mhz, 0, 'f', 4).arg(w), true, [this, mhz, w, thr, o](txc::TxControl& t) {
        auto r = t.tune(mhz, w, thr, o, [this](const txc::Progress& p) { std::lock_guard<std::mutex> l(m_); last_ = p; have_last_ = true; });
        if (r.unkey_failed) { unkey_failed_ = true; return QString("UNKEY FAILED: ") + QString::fromStdString(r.stop); }
        if (!r.outcome) return QString("Tune refused: ") + QString::fromStdString(r.why_not);
        const auto& s = r.outcome->status;
        QString res = r.outcome->result == atu::Atu::TuneResult::Pass ? "PASS" : r.outcome->result == atu::Atu::TuneResult::Fail ? QString("FAIL (%1)").arg(atu::fail_text(r.outcome->fail_reason)) : "stopped";
        if (r.outcome->result != atu::Atu::TuneResult::Pass)   // the tuner's own report belongs to a pass only (its last status may be from an earlier tune)
            return QString("Tune %1 after %2 s (%3). Max %4.").arg(res).arg(r.outcome->seconds, 0, 'f', 1).arg(QString::fromStdString(r.stop)).arg(fmt_w(r.max_fwd_w))
                   + (r.pass_rejected.empty() ? QString() : " " + QString::fromStdString(r.pass_rejected) + ".");
        res = QString("Tune %1 after %2 s (%3). Tuner: ind %4 cap %5 %6, %7, SWR %8. Max %9.").arg(res).arg(r.outcome->seconds, 0, 'f', 1).arg(QString::fromStdString(r.stop))
              .arg(s.ind ? *s.ind : -1).arg(s.cap ? *s.cap : -1).arg(s.hiz && *s.hiz ? "Hi-Z" : "Lo-Z").arg(s.fwd_w ? fmt_w(*s.fwd_w) : "?").arg(s.swr ? QString::number(*s.swr, 'f', 2) : "?").arg(fmt_w(r.max_fwd_w));
        if (r.saved) res += " Saved for this band.";
        if (r.estimated_drive) res += " (No measured drive on this band yet, so it was estimated: press Measure drive now.)";
        return res;
    });
}

void TransmitCard::onMeasure() {
    if (!tx_ || !tx_->armed()) return;
    const double mhz = rx_->dialHz() / 1e6;
    if (txm::band_of(mhz * 1e6) < 0) { report("The dial is outside the amateur bands."); return; }
    if (!ask("Measure the drive?", QString("Transmit a 2 W tone ramp on %1 MHz (about 4 s) to measure this band's drive level?\n\nThe ATU should hold a good match for this band "
             "(Restore ATU, or tune at low power first). Listen first: is the frequency clear?").arg(mhz, 0, 'f', 4))) return;
    txc::Options o; o.rate = simulated_ ? 48000 : txRate(); o.mirror_q = mirror_->isChecked(); o.tx_ant = txAnt();
    if (!(o.rate > 0)) { report("The radio's ALSA playback offers no 8-channel rate."); return; }
    run(QString("Measuring the drive on %1 MHz").arg(mhz, 0, 'f', 4), true, [this, mhz, o](txc::TxControl& t) {
        auto r = t.ramp(mhz, 2.0, o, [this](const txc::Progress& p) { std::lock_guard<std::mutex> l(m_); last_ = p; have_last_ = true; });
        if (r.unkey_failed) { unkey_failed_ = true; return QString("UNKEY FAILED: ") + QString::fromStdString(r.stop); }
        if (!r.reached) return QString("Measure stopped: %1 (max %2)").arg(QString::fromStdString(r.stop)).arg(fmt_w(r.max_fwd_w));
        return QString("Drive measured: %1 W per drive² (1 W at %2 dBFS). Tune is available on this band.").arg(r.k, 0, 'f', 0).arg(20 * std::log10(std::sqrt(1 / r.k)), 0, 'f', 1);
    });
}

void TransmitCard::onTestTone() {
    if (!tx_ || !tx_->armed()) return;
    const double mhz = rx_->dialHz() / 1e6;
    if (txm::band_of(mhz * 1e6) < 0) { report("The dial is outside the amateur bands."); return; }
    txc::Options o; o.rate = simulated_ ? 48000 : txRate(); o.mirror_q = mirror_->isChecked(); o.tx_ant = txAnt(); o.tone_hz = 1000;
    if (!(o.rate > 0)) { report("The radio's ALSA playback offers no 8-channel rate."); return; }
    const double w = test_w_->value();
    run(QString("Test tone at %1 MHz, %2 W").arg(mhz + 0.001, 0, 'f', 4).arg(w, 0, 'f', 0), true, [this, mhz, o, w](txc::TxControl& t) {
        auto r = t.ramp(mhz, w, o, [this](const txc::Progress& p) { std::lock_guard<std::mutex> l(m_); last_ = p; have_last_ = true; }, {}, 8.0);
        if (r.unkey_failed) { unkey_failed_ = true; return QString("UNKEY FAILED: ") + QString::fromStdString(r.stop); }
        QString cmp;                                                          // our detector beside the tuner's own meter (the SWR question, 2026-11-19)
        { double f = 0, rv = 0, sw = 0, af = 0, ar = 0, as = 0; int n = 0, na = 0; QStringList rows;
          for (const auto& c : r.compare) { f += c.fwd_w; rv += c.rev_w; sw += c.swr; ++n;
              if (c.atu_fwd_w && c.atu_rev_w && c.atu_swr) { af += *c.atu_fwd_w; ar += *c.atu_rev_w; as += *c.atu_swr; ++na; }
              rows << QString("  %1 s: ours adc %2 / %3 = %4 W / %5 W SWR %6 | tuner %7").arg(c.t, 0, 'f', 1).arg(c.adc_fwd).arg(c.adc_rev).arg(c.fwd_w, 0, 'f', 2).arg(c.rev_w, 0, 'f', 2).arg(c.swr, 0, 'f', 2)
                          .arg(c.atu_fwd_w && c.atu_rev_w && c.atu_swr ? QString("%1 W / %2 W SWR %3").arg(*c.atu_fwd_w, 0, 'f', 2).arg(*c.atu_rev_w, 0, 'f', 2).arg(*c.atu_swr, 0, 'f', 2) : QString("no report")); }
          if (n) cmp = QString(" Meter: ours %1 W fwd / %2 W ref, SWR %3; tuner %4.\n%5").arg(f / n, 0, 'f', 2).arg(rv / n, 0, 'f', 2).arg(sw / n, 0, 'f', 2)
                         .arg(na ? QString("%1 W / %2 W, SWR %3").arg(af / na, 0, 'f', 2).arg(ar / na, 0, 'f', 2).arg(as / na, 0, 'f', 2) : QString("gave no report")).arg(rows.join("\n")); }
        return r.reached ? QString("Test tone done (%1 for 8 s, meant to be at %2 MHz%3).%4").arg(fmt_w(r.max_fwd_w)).arg(mhz + 0.001, 0, 'f', 4).arg(o.mirror_q ? ", Q mirrored" : "").arg(cmp)
                         : QString("Test tone stopped: %1.%2").arg(QString::fromStdString(r.stop)).arg(cmp);
    });
}

void TransmitCard::onSend() {
    rx_hold_ = false; att_count_ = 0; endAttPause();                                         // the operator's Send / CQ (or the end of an Attended CQ pause)
    if (seq_) seq_->stop();
    if (auto_seq_->isChecked() && ft_msg_->text().trimmed().toUpper().startsWith("CQ ")) { if (!haveStation()) return; sequencer().call_cq(); ft_msg_->setText(QString::fromStdString(seq_->current())); }   // CQ: answers are worked automatically
    queueSend();
}
void TransmitCard::startAttPause() {          // after the 6th call in slot N: silent for at least kAttPauseSlots slots, then CQ in the other parity
    const bool ft4 = ft_mode_->currentIndex() == 1; const double slot = ft4 ? 7.5 : 15.0;
    const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    double n = last_slot_utc_.load(); if (!(n > 0) || n > now + slot) n = std::floor(now / slot) * slot;
    int k = kAttPauseSlots + 1; if (k % 2 == 0) ++k;                                         // an odd number of slots on: the other parity (3 -> slot N+5, 4 silent)
    const double resume = n + k * slot;
    const int was = ft_par_->currentIndex(); { QSignalBlocker b(ft_par_); ft_par_->setCurrentIndex(1 - was); } saveState();
    att_pause_ = true; att_count_ = 0;
    const int ms = att_pause_ms_for_tests_ >= 0 ? att_pause_ms_for_tests_ : int(std::max(0.0, resume - 3.5 - now) * 1000);   // queued 3.5 s ahead, like a resend
    att_timer_->start(ms);
    report(QString("Attended CQ: %1 calls in the %2 slot, now listening for %3 slots; CQ in the %4 slot at %5 UTC (an answer is worked straight away; Stop ends it).")
           .arg(att_calls_).arg(was ? "odd" : "even").arg(k - 1).arg(was ? "even" : "odd")
           .arg(QDateTime::fromMSecsSinceEpoch(qint64(resume * 1000), Qt::UTC).toString("HH:mm:ss")));
    updateButtons();
}
void TransmitCard::endAttPause() { if (att_timer_) att_timer_->stop(); if (att_pause_) { att_pause_ = false; updateButtons(); } }
void TransmitCard::queueSend() {
    if (!tx_ || !tx_->armed() || worker_running_) return;
    const double mhz = rx_->dialHz() / 1e6; const bool ft4 = ft_mode_->currentIndex() == 1; const QString text = ft_msg_->text().trimmed().toUpper();
    if (txm::band_of(mhz * 1e6) < 0) { report("The dial is outside the amateur bands."); return; }
    const double rate = simulated_ ? 48000 : txRate();
    if (!(rate > 0)) { report("The radio's ALSA playback offers no 8-channel rate."); return; }
    if (text.isEmpty() || !tx::ft_message_ok(text.toStdString())) { report(QString("Cannot send \"%1\": not a message FT8/FT4 can carry").arg(text)); return; }   // cheap check now; the audio is rendered by the transmit worker, never on the window's thread (it froze the waterfall)
    auto prep = std::make_shared<txc::TxControl::FtPrepared>(); prep->text = text.toStdString(); prep->audio_hz = ft_hz_->value(); prep->ft4 = ft4; prep->rate = rate;
    const tx::DigiMode mode = ft4 ? tx::DigiMode::FT4 : tx::DigiMode::FT8; const tx::Parity par = ft_par_->currentIndex() ? tx::Parity::Odd : tx::Parity::Even;
    const double now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
    tx::TxPlan plan; try { plan = tx::plan_tx(now, mode, par, tx::default_start_offset_s(mode), 2.0, 1.0); } catch (const std::exception& e) { report(e.what()); return; }
    const QString when = QDateTime::fromMSecsSinceEpoch(qint64(plan.slot.start_utc * 1000), Qt::UTC).toString("HH:mm:ss");
    ft_pending_ = prep;
    const double start_in = plan.skip_s > 0 ? 0.0 : std::max(0.0, plan.slot.start_utc - 0.8 - now);   // receive pauses ~0.8 s before the slot
    ft_timer_->start(int(start_in * 1000)); report(QString("%1 \"%2\" queued for %3 UTC, %4 slot, %5 Hz, %6 W%7 (Stop cancels).").arg(ft4 ? "FT4" : "FT8", text, when).arg(par == tx::Parity::Even ? "even" : "odd")
           .arg(ft_hz_->value()).arg(ft_w_->value()).arg(ft_repeat_->isChecked() ? ", attended CQ" : "")); updateButtons();
}
void TransmitCard::startSend() {
    if (!ft_pending_ || !tx_ || !tx_->armed() || worker_running_) { ft_pending_.reset(); updateButtons(); return; }
    auto prep = ft_pending_; ft_pending_.reset();
    if (seq_ && seq_->stage() != fireside::seq::Stage::Idle) {                                  // sequencing: the answer from the slot just heard decides what we send
        pauseReceive();                                                                      // (decodes that slot; heardLine moves the QSO on)
        const std::string next = seq_->next_message();
        if (next.empty()) { takeLog(); report(QString::fromStdString(seq_->describe()) + "." + resumeReceive()); updateButtons(); return; }
        if (next != prep->text) { auto p2 = std::make_shared<txc::TxControl::FtPrepared>(*prep); p2->text = next; p2->buf.clear(); prep = p2; }   // rendered by the worker below
        ft_msg_->setText(QString::fromStdString(next));
    }
    last_sent_ = QString::fromStdString(prep->text);
    const double mhz = rx_->dialHz() / 1e6, w = ft_w_->value(); const tx::Parity par = ft_par_->currentIndex() ? tx::Parity::Odd : tx::Parity::Even;
    txc::Options o; o.rate = prep->rate; o.mirror_q = mirror_->isChecked(); o.tx_ant = txAnt();
    {   // the QSO pane: what we send, labelled with the slot it goes out in
        const double slot = prep->ft4 ? 7.5 : 15.0, now = std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count();
        const double start = std::floor((now + 2.0) / slot) * slot;                         // the slot about to start (or just started: joining late)
        emit txSent(QDateTime::fromMSecsSinceEpoch(qint64(start * 1000), QTimeZone::UTC).toString("HHmmss"), int(std::lround(prep->audio_hz)), QString::fromStdString(prep->text));
    }
    run(QString("Sending %1 \"%2\"").arg(prep->ft4 ? "FT4" : "FT8", QString::fromStdString(prep->text)), true, [this, prep, mhz, w, par, o](txc::TxControl& t) {
        if (prep->buf.empty()) { const auto f = txc::TxControl::prepare_ft(prep->text, prep->audio_hz, prep->ft4, prep->rate); prep->buf = f.buf; }   // here, on the worker thread (throws -> "failed: ...")
        auto r = t.send_prepared(mhz, *prep, par, w, o, [this](const txc::Progress& p) { std::lock_guard<std::mutex> l(m_); last_ = p; have_last_ = true; }, {}, 0.3);
        if (r.unkey_failed) { unkey_failed_ = true; return QString("UNKEY FAILED: ") + QString::fromStdString(r.stop); }
        if (!r.sent && !r.why_not.empty()) return QString("Not sent: ") + QString::fromStdString(r.why_not);
        const QString when = QDateTime::fromMSecsSinceEpoch(qint64(r.slot_utc * 1000), Qt::UTC).toString("HH:mm:ss");
        QString res = QString("%1 %2 at %3 UTC: %4, %5 W (set %6 W)%7.").arg(prep->ft4 ? "FT4" : "FT8", r.sent ? "sent" : "stopped", when, QString::fromStdString(r.stop))
                      .arg(r.mean_fwd_w, 0, 'f', 1).arg(w, 0, 'f', 0).arg(r.late_s > 0 ? ", joined late" : "");
        if (r.limited_to_w > 0) res += QString(" This band's limit is %1 W: set the power to %1 W or less.").arg(r.limited_to_w, 0, 'f', 0);
        unkeyed_utc_ = r.unkeyed_utc; last_slot_utc_ = r.slot_utc;
        if (r.sent && r.mean_fwd_w > 0.1) { last_tx_w_ = r.mean_fwd_w; last_tx_t_ = QDateTime::currentSecsSinceEpoch(); }   // for the log's TX_PWR
        if (r.sent) res += " #sent";
        return res;
    });
}

void TransmitCard::onRestore() {
    if (!tx_ || !tx_->armed()) return;
    const int band = txm::band_of(rx_->dialHz());
    run("Restoring the ATU (no RF)", false, [band](txc::TxControl& t) {
        std::string msg; const bool ok = t.restore_relays(band, [&](const std::string& m) { msg = m; });
        return QString::fromStdString(msg.empty() ? (ok ? "ATU restored" : "ATU not restored") : msg);
    });
}

void TransmitCard::onRadioRunning(bool on) {
    if (!on) { extCancel(); tx_.reset(); QSignalBlocker b(arm_); arm_->setChecked(false); arm_tried_ = false; report("Radio stopped: transmit disarmed."); }
    else if (auto_arm_) QTimer::singleShot(300, this, [this] { autoArm(); });               // the EEPROM check runs as soon as the radio is up
    updateButtons();
}
void TransmitCard::armThen(std::function<void()> done) {
    if (!auto_arm_ || (tx_ && tx_->armed())) { if (done) done(); return; }
    after_arm_ = std::move(done); autoArm();
    if (!worker_running_ && after_arm_) { auto d = std::move(after_arm_); after_arm_ = nullptr; d(); }   // the check could not start (no backup ...): receive anyway
}
bool TransmitCard::eventFilter(QObject* o, QEvent* e) {
    if ((e->type() == QEvent::KeyPress || e->type() == QEvent::KeyRelease) && space_ptt_ && space_ptt_->isChecked()) {
        auto* k = static_cast<QKeyEvent*>(e);
        QWidget* f = QApplication::focusWidget();
        const bool typing = f && (qobject_cast<QLineEdit*>(f) || qobject_cast<QAbstractSpinBox*>(f) || qobject_cast<QPlainTextEdit*>(f));
        if (k->key() == Qt::Key_Space && !k->isAutoRepeat() && !typing && window()->isActiveWindow()) {
            const bool down = e->type() == QEvent::KeyPress; if (down != ptt_key_) { ptt_key_ = down; onPttChanged(); }
            return true;
        }
    }
    return QFrame::eventFilter(o, e);
}
void TransmitCard::onPttChanged() {
    ptt_->setDown(ptt_btn_ || ptt_key_ || ptt_mic_);
    if (pttActive() && !worker_running_ && tx_ && tx_->armed() && !ft_pending_) startVoice();   // released: the over's loop sees it and unkeys
}
void TransmitCard::startVoice() {
    if (!audio_) { report("No Audio tab."); return; }
    const double mhz = rx_->dialHz() / 1e6; if (txm::band_of(mhz * 1e6) < 0) { report("The dial is outside the amateur bands."); return; }
    std::shared_ptr<vox::VoxInput> vin = vox_in_;                                         // VOX listening: the over reads its input (with the pre-roll); else the input opened now
    txc::IAudioIn* in = nullptr;
    if (vin) { vin->set_reading(true); in = vin.get(); vox_over_in_ = vin; }                  // the card holds it: released on this thread after the over (a PC capture must close here)
    else {
        if (audio_->testInputOpen()) { report("Voice: the Audio tab's input test is running: stop it first."); return; }
        QString err; voice_in_ = audio_->openInput(&err);
        if (!voice_in_) { report("Voice: cannot open the input (" + err + ")"); return; }
        in = voice_in_.get();
    }
    txc::VoiceOptions v; v.o.rate = simulated_ ? 48000 : txRate(); v.o.mirror_q = mirror_->isChecked(); v.o.tx_ant = txAnt(); v.peak_w = voice_w_->value(); v.chain = audio_->chainConfig(); v.max_key_s = voice_timeout_s_;
    if (monitor_ && monitor_->isChecked()) { if (!mon_feed_) mon_feed_ = new PcAudioFeed(pc_audio_, this); PcAudioFeed* f = mon_feed_; v.monitor = [f](const float* a, size_t n) { f->push(a, n); }; }
    v.lsb = voice_mode_->currentIndex() == 2 || (voice_mode_->currentIndex() == 0 && (rx_->modeIndex() == 1 || (rx_->modeIndex() != 0 && mhz < 10.0 && !(mhz > 5.2 && mhz < 5.5))));   // Auto: as the receiver
    if (!(v.o.rate > 0)) { report("No playback rate for transmit."); voice_in_.reset(); vox_over_in_.reset(); if (vin) vin->set_reading(false); return; }
    run(QString("Voice %1 on %2 MHz, %3 W PEP (%4%5)").arg(v.lsb ? "LSB" : "USB").arg(mhz, 0, 'f', 4).arg(v.peak_w, 0, 'f', 0).arg(audio_->inputName()).arg(vin ? ", VOX" : ""), !fw_iso_duplex_running(),
        [this, mhz, v, in, vin = vin.get()](txc::TxControl& t) {
            auto r = t.voice(mhz, *in, [this, vin] { return pttActive() || (vin && vox_cur_.load() == vin && vin->on()); }, v, [this](const txc::Progress& p) { std::lock_guard<std::mutex> l(m_); last_ = p; have_last_ = true; });   // unticking VOX unkeys (vox_cur_)
            if (vin) {
                vin->set_reading(false);
                if (txc::TxControl::is_safety_stop(r.stop) || vin->failed() || !r.why_not.empty()) {   // (a blip too short to key is not a refusal)     // VOX never keys again by itself after a time-out (noise holding it on?), a safety stop or a refusal
                    std::lock_guard<std::mutex> l(vox_m_); vox_kill_why_ = vin->failed() ? "the input stopped" : !r.why_not.empty() ? r.why_not : r.stop; vox_kill_ = true;
                }
            }
            if (r.unkey_failed) { unkey_failed_ = true; return QString("UNKEY FAILED: ") + QString::fromStdString(r.stop); }
            if (!r.keyed) return QString("Voice not keyed: ") + QString::fromStdString(r.why_not.empty() ? r.stop : r.why_not);
            if (r.max_fwd_w > 0.1) { last_tx_w_ = r.max_fwd_w; last_tx_t_ = QDateTime::currentSecsSinceEpoch(); }
            return QString("Voice over: %1, %2 s, peak %3 W (set %4 W PEP)%5").arg(QString::fromStdString(r.stop)).arg(r.seconds, 0, 'f', 1).arg(r.max_fwd_w, 0, 'f', 1).arg(v.peak_w, 0, 'f', 0)
                   .arg(r.underruns > 4800 ? QString(", audio gaps %1").arg(r.underruns) : QString());
        });
}
int TransmitCard::txAnt() const { const int a = tx_ant_ ? tx_ant_(rx_->dialHz()) : 1; return a >= 1 && a <= 3 ? a : 1; }
void TransmitCard::autoArm() {
    if (worker_running_ || (tx_ && tx_->armed())) return;
    arm_tried_ = true; { QSignalBlocker b(arm_); arm_->setChecked(true); } onArm(true);
}
void TransmitCard::stopEverything(const QString& why) {
    ++stop_gen_; ft_timer_->stop(); ft_pending_.reset(); endAttPause(); { QSignalBlocker b(ft_repeat_); ft_repeat_->setChecked(false); } follow_pending_ = false; auto_tried_.clear(); from_cq_ = false;
    if (seq_) { seq_->stop(); noteSeqEnd(false); }                                           // a logged QSO stopped during its RR73 repeats is still done
    extCancel(); if (tx_) tx_->abort(); report(why); updateButtons();
}
void TransmitCard::onDial(double hz) {
    const int band = txm::band_of(hz); if (band == last_band_) return;
    const bool qso_running = ft_pending_ || att_pause_ || ft_repeat_->isChecked() || follow_pending_ || from_cq_ || (seq_ && seq_->active()) || (worker_running_ && !ext_over_);
    last_band_ = band; updateMemoryLabel(); updateButtons();
    if (qso_running) {                                                                        // a QSO or CQ belongs to its band: never carried to another one
        stopEverything("Band changed: the QSO / CQ is stopped and nothing is queued (the callers waiting on the old band are cleared).");
        rx_->clearCallers();
    }
    if (band >= 0 && auto_restore_->isChecked() && tx_ && tx_->armed() && !worker_running_ && tx_->memory().recall(band)) onRestore();
}

void TransmitCard::onReceiveLost(const QString& why) {
    const bool automatic = ft_pending_ || att_pause_ || ft_repeat_->isChecked() || follow_pending_ || from_cq_ || (seq_ && seq_->active()) || extActive();
    if (!automatic) return;
    rx_hold_ = true;
    stopEverything(QString("Receive stopped (%1): auto transmit stopped, nothing queued. It does not start again by itself: check the receive, then press CQ or double-click a station.").arg(why));
    emit safetyTrip("receive stopped, so auto transmit stopped");
}
bool TransmitCard::keyedNow() const { return tx_ && tx_->keyed(); }
void TransmitCard::tick() {
    const bool keyed = tx_ && tx_->keyed();
    rx_->setMeterTx(keyed);
    stop2_->setEnabled(stop_->isEnabled()); stop3_->setEnabled(stop_->isEnabled());
    if (status_->toolTip() != status_->text()) status_->setToolTip(status_->text());    // the line is clipped: the tooltip has all of it
    if (!simulated_) rx_->setTransmitting(keyed && fw_iso_duplex_running(), cw_over_ ? txc::kCwOffsetHz : 0.0);   // a CW over: the DDS 1 kHz below the dial
    rx_->setTxMute(keyed || cw_over_);                                                  // receive audio off while keyed (our own signal; it would also feed back into the mic)           // duplex: receive follows the DDS onto the dial for the over
    lamp_->setStyleSheet(keyed ? "background:#c62828;color:white;font-weight:bold;border-radius:4px;padding:2px" : "background:#3a3a3a;color:#888;border-radius:4px;padding:2px");
    txc::Progress p; bool have; { std::lock_guard<std::mutex> l(m_); p = last_; have = have_last_; }
    if (have) pm_->setReading(p.rd.fwd_w, p.rd.rev_w, p.rd.swr ? *p.rd.swr : 0.0, keyed); else pm_->setReading(0, 0, 0, false);
    stop_->setEnabled(worker_running_ || ft_pending_ != nullptr || att_pause_);
}

void TransmitCard::updateButtons() {
    auto& s = radio_->session(); const bool started = s.state() == RadioSession::State::Started && s.allow_write();
    const bool armed = tx_ && tx_->armed(), idle = !worker_running_; const int band = txm::band_of(rx_->dialHz());
    arm_->setEnabled(started && idle); arm_->setVisible(started && idle && arm_tried_ && !(tx_ && tx_->armed()));   // only to retry a failed check
    const bool on_atu = txAnt() == 1;
    tune_->setEnabled(armed && idle && band >= 0 && on_atu);
    tune_->setToolTip(band >= 0 && armed && !(tx_->k_for(band) > 0) ? "No measured drive on this band yet: Tune borrows the next band's measured level (or the EEPROM table, which errs low). Press Measure drive after a good tune."
                                                                    : "Transmits a carrier (up to 15 s) while the ATU searches. Asks first: listen to the frequency.");
    measure_->setEnabled(armed && idle && band >= 0); test_->setEnabled(armed && idle && band >= 0); test_w_->setEnabled(idle); mirror_->setEnabled(idle);
    send_->setEnabled(armed && idle && band >= 0 && !ft_pending_); cq_->setEnabled(send_->isEnabled()); ptt_->setEnabled(armed && band >= 0 && audio_ && !ft_pending_);
    restore_->setEnabled(armed && idle && band >= 0 && on_atu && tx_->memory().recall(band).has_value());
    if (!on_atu) { tune_->setToolTip(QString("This band transmits on ANT%1: the ATU is on ANT1 only (Antennas tab)").arg(txAnt())); }
    if (tune2_) { tune2_->setEnabled(tune_->isEnabled()); tune2_->setToolTip(tune_->toolTip()); }
    power_->setEnabled(idle); thr_->setEnabled(idle); stop_->setEnabled(worker_running_ || ft_pending_ != nullptr || att_pause_);
    ext_allow_->setEnabled(armed || ext_allow_->isChecked());                      // can always be unticked
    gen_btn_->setEnabled(armed && idle && band >= 0 && !ft_pending_);
}
void TransmitCard::updateMemoryLabel() {
    const int band = txm::band_of(rx_->dialHz());
    if (band < 0) { mem_->setText("ATU memory: (outside the amateur bands)"); return; }
    std::optional<atu::Setting> st;
    if (tx_) st = tx_->memory().recall(band);
    else if (!no_settings()) { std::ifstream in(mem_path().toStdString()); std::stringstream ss; ss << in.rdbuf(); st = atu::Memory::from_text(ss.str()).recall(band); }
    mem_->setText(st ? QString("ATU %1: ind %2 cap %3 %4, SWR %5 (%6)").arg(txm::band_name(band)).arg(st->ind).arg(st->cap).arg(st->hiz ? "Hi-Z" : "Lo-Z").arg(st->swr, 0, 'f', 2).arg(QString::fromStdString(st->utc))
                     : QString("ATU %1: nothing stored").arg(txm::band_name(band)));
}
void TransmitCard::loadState() {                         // worker thread, right after arming: the per-band ATU memory and drive calibrations
    if (no_settings() || !tx_) return;
    { std::ifstream in(mem_path().toStdString()); std::stringstream ss; ss << in.rdbuf(); tx_->memory() = atu::Memory::from_text(ss.str()); }
    std::ifstream in(drive_path().toStdString()); std::stringstream ss; ss << in.rdbuf(); tx_->set_all_k(txc::parse_drive_text(ss.str()));
}
void TransmitCard::saveState() {
    if (no_settings()) return;
    QSettings q("fireside", "transmit"); q.setValue("tune_w", power_->value()); q.setValue("thr", thr_->currentIndex()); q.setValue("auto_restore", auto_restore_->isChecked()); q.setValue("mirror_q", mirror_->isChecked());
    q.setValue("auto_seq", auto_seq_->isChecked()); q.setValue("ft_msg", ft_msg_->text()); q.setValue("ft_hz", ft_hz_->value()); q.setValue("ft_clear", ft_clear_->isChecked()); q.setValue("ft_w", ft_w_->value()); q.setValue("ft_mode", ft_mode_->currentIndex()); q.setValue("ft_par", ft_par_->currentIndex());
    if (!tx_ || worker_running_) return;
    QDir().mkpath(cfg_dir());
    std::ofstream(mem_path().toStdString()) << tx_->memory().to_text();
    std::ofstream(drive_path().toStdString()) << txc::drive_text(tx_->all_k());
}
