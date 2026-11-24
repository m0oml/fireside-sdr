// The Transmit card's CW part: the controls beside the Morse decoder, the paddle watcher and the CW over (TxControl::cw does the keying, with the
// gate, its limits and the hard drive clamp). Semi break-in: a paddle press on the CW screen starts an over; MOX drops after the break-in delay.
#include "TransmitCard.h"
#include "CwSidetone.h"
#include "RadioPanel.h"
#include "ReceiverPanel.h"
#include "fireside/fw_iso.h"
#include "fireside/session.h"
#include "fireside/tx_meter.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

using namespace fireside;
namespace {
bool cw_no_settings() { return qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS"); }
struct NoKeys : txc::ICwKeys { uint8_t paddles() override { return 0; } };   // a text over with the paddle not read
}  // namespace

void TransmitCard::buildCw() {
    cw_wpm_ = new QSpinBox; cw_wpm_->setRange(5, 60); cw_wpm_->setValue(20); cw_wpm_->setSuffix(" wpm"); cw_wpm_->setToolTip("Keyer and text speed (PARIS)");
    cw_mode_ = new QComboBox; cw_mode_->addItems({"Iambic B", "Iambic A", "Straight key"});
    cw_mode_->setToolTip("Iambic B (squeeze release sends one more element) or A, for paddles on the KEY jack (dot = tip, dash = ring). "
                         "Straight key: either contact keys directly (also for an external keyer).");
    cw_rev_ = new QCheckBox("Reverse"); cw_rev_->setToolTip("Swap the dot and dash paddles");
    cw_weight_ = new QSpinBox; cw_weight_->setRange(25, 75); cw_weight_->setValue(50); cw_weight_->setPrefix("weight "); cw_weight_->setToolTip("50 = the standard 1:3 dot:dash; more lengthens every element, less shortens it (the speed stays)");
    cw_w_ = new QDoubleSpinBox; cw_w_->setRange(1, 100); cw_w_->setDecimals(0); cw_w_->setValue(10); cw_w_->setSuffix(" W"); cw_w_->setToolTip("Key-down power (needs a measured drive level on the band)");
    cw_hang_ = new QSpinBox; cw_hang_->setRange(50, 3000); cw_hang_->setSingleStep(50); cw_hang_->setValue(600); cw_hang_->setSuffix(" ms"); cw_hang_->setPrefix("delay ");
    cw_hang_->setToolTip("Break-in delay: the transmitter stays keyed this long after the last element, then receive is back. About 7 dots (a word space) at your speed keeps it keyed between words");
    cw_paddle_ = new QCheckBox("Paddle keys"); cw_paddle_->setChecked(true);
    cw_paddle_->setToolTip("On the CW screen, with transmit ready: the paddle or key on the radio's KEY jack starts an over (semi break-in). Untick to send text only");
    cw_side_ = new QCheckBox("Sidetone"); cw_side_->setChecked(true); cw_side_->setToolTip("Hear what you send on the PC's speakers (receive audio is off while the transmitter is keyed)");
    cw_lights_ = new QLabel; cw_lights_->setMinimumWidth(110); cw_lights_->setToolTip("The KEY jack as the radio reports it: lights while a contact is closed (no RF needed to check the wiring)");
    cw_text_ = new QLineEdit; cw_text_->setPlaceholderText("Text to send (Enter or Send)"); cw_text_->setToolTip("A-Z, 0-9 and . , ? / = + -. Touching the paddle stops it");
    cw_send_ = new QPushButton("Send"); cw_send_->setToolTip("Send the text at the keyer speed (semi break-in: receive is back after the delay)");
    cw_stop_ = new QPushButton("Stop"); cw_stop_->setToolTip("Unkey now");
    cw_mem_ = new QComboBox; cw_mem_->addItem("Memory..."); cw_mem_->setToolTip("The CW memories (TX lab): pick one to put it in the text box");
    auto row = [](std::initializer_list<QWidget*> ws, bool stretch = true) { auto* h = new QHBoxLayout; h->setSpacing(8); for (auto* w : ws) h->addWidget(w); if (stretch) h->addStretch(1); return h; };
    cw_part_ = new QWidget(this); auto* v = new QVBoxLayout(cw_part_); v->setContentsMargins(0, 0, 0, 0); v->setSpacing(6);
    v->addLayout(row({cw_mode_, cw_wpm_, cw_weight_, cw_rev_})); v->addLayout(row({cw_w_, cw_hang_, cw_paddle_, cw_side_}));
    v->addLayout(row({cw_text_, cw_mem_}, false)); v->addLayout(row({cw_send_, cw_stop_, cw_lights_}));
    connect(cw_send_, &QPushButton::clicked, this, [this] { startCw(cw_text_->text()); });
    connect(cw_text_, &QLineEdit::returnPressed, this, [this] { startCw(cw_text_->text()); });
    connect(cw_stop_, &QPushButton::clicked, this, [this] { stop_->click(); });
    connect(cw_mem_, qOverload<int>(&QComboBox::activated), this, [this](int i) { if (i > 0) cw_text_->setText(cw_mem_->itemText(i)); QSignalBlocker b(cw_mem_); cw_mem_->setCurrentIndex(0); });
    connect(cw_mode_, qOverload<int>(&QComboBox::currentIndexChanged), this, [this](int i) { cw_rev_->setEnabled(i != 2); cw_weight_->setEnabled(i != 2); saveCw(); });
    for (auto* b : {cw_rev_, cw_paddle_, cw_side_}) connect(b, &QCheckBox::toggled, this, [this] { saveCw(); });
    for (auto* b : {cw_wpm_, cw_weight_, cw_hang_}) connect(b, &QSpinBox::editingFinished, this, [this] { saveCw(); });
    connect(cw_w_, &QDoubleSpinBox::editingFinished, this, [this] { saveCw(); });
    cw_timer_ = new QTimer(this); cw_timer_->setInterval(50); connect(cw_timer_, &QTimer::timeout, this, &TransmitCard::cwTick); cw_timer_->start();
    loadCw();
}
void TransmitCard::loadCw() {
    if (cw_no_settings()) return;
    QSettings q("fireside", "cw");
    cw_wpm_->setValue(q.value("wpm", 20).toInt()); cw_mode_->setCurrentIndex(std::clamp(q.value("mode", 0).toInt(), 0, 2)); cw_rev_->setChecked(q.value("reverse", false).toBool());
    cw_weight_->setValue(q.value("weight", 50).toInt()); cw_w_->setValue(q.value("power", 10).toDouble()); cw_hang_->setValue(q.value("hang_ms", 600).toInt());
    cw_paddle_->setChecked(q.value("paddle", true).toBool()); cw_side_->setChecked(q.value("sidetone", true).toBool()); cw_text_->setText(q.value("text").toString());
}
void TransmitCard::saveCw() {
    if (cw_no_settings()) return;
    QSettings q("fireside", "cw");
    q.setValue("wpm", cw_wpm_->value()); q.setValue("mode", cw_mode_->currentIndex()); q.setValue("reverse", cw_rev_->isChecked()); q.setValue("weight", cw_weight_->value());
    q.setValue("power", cw_w_->value()); q.setValue("hang_ms", cw_hang_->value()); q.setValue("paddle", cw_paddle_->isChecked()); q.setValue("sidetone", cw_side_->isChecked()); q.setValue("text", cw_text_->text());
}
bool TransmitCard::cwWanted() const {                         // read the paddle: CW screen, ticked, armed, the radio started (and duplex on the real radio)
    if (!cw_paddle_->isChecked() || rx_->modeGroup() != 1 || !tx_ || !tx_->armed()) return false;
    auto& s = radio_->session(); if (s.state() != RadioSession::State::Started) return false;
    return simulated_ || fw_iso_duplex_running();
}
bool TransmitCard::cwWatching() const { return cw_keys_ && cw_keys_->running(); }
void TransmitCard::cwTick() {
    if (cw_over_ && !worker_running_) { cw_over_ = false; rx_->setTxMute(false); }
    const bool want = cwWanted();
    if (want && !cwWatching()) {
        auto* sess = &radio_->session();
        const bool event = cw_no_settings() || QSettings("fireside", "cw").value("key_source", "event").toString() != "read_ptt";   // "read_ptt": the slower command, if the EVENT register does not follow the key
        cw_keys_ = std::make_shared<cwk::KeyWatcher>([sess, event]() { return sess->read_keys(event); }, event ? 0.003 : 0.005);
        QPointer<TransmitCard> self(this);
        cw_keys_->set_on_press([self] { QMetaObject::invokeMethod(self, [self] { if (self && !self->worker_running_ && !self->ft_pending_ && self->cwWanted()) self->startCw(QString()); }, Qt::QueuedConnection); });
        cw_keys_->start();
    } else if (!want && cwWatching() && !cw_over_) cw_keys_->stop();
    const uint8_t k = cwWatching() ? cw_keys_->current() : 0;
    const QString on = "<span style='color:#3ddc84'>●</span>", off = "<span style='color:#4a5263'>●</span>";
    const QString t = cwWatching() ? QString("%1 dot  %2 dash").arg(k & 1 ? on : off, k & 2 ? on : off) : QString("<span style='color:#6b7385'>key not read</span>");
    if (cw_lights_->text() != t) cw_lights_->setText(t);
    const bool armed = tx_ && tx_->armed(), band_ok = txm::band_of(rx_->dialHz()) >= 0;
    cw_send_->setEnabled(armed && band_ok && !worker_running_ && !ft_pending_); cw_stop_->setEnabled(worker_running_ || ft_pending_ != nullptr);
    if (cw_mems_ && cw_mem_->count() == 1) { const QStringList m = cw_mems_(); for (const QString& x : m) if (!x.trimmed().isEmpty()) cw_mem_->addItem(x.trimmed()); }
}
void TransmitCard::startCw(const QString& raw) {
    if (!tx_ || !tx_->armed()) { report("CW: transmit is not ready yet."); return; }
    if (worker_running_ || ft_pending_) { report("CW: the transmitter is busy."); return; }
    const double mhz = rx_->dialHz() / 1e6; if (txm::band_of(mhz * 1e6) < 0) { report("CW: the dial is outside the amateur bands."); return; }
    if (!simulated_ && !fw_iso_duplex_running()) { report("CW needs the direct FireWire stream (48 / 96 / 192 on the Radio screen), so receive carries on between overs."); return; }
    const QString text = raw.simplified().toUpper();
    if (raw.isEmpty() && !cwWatching()) { report("CW: the paddle is not being read."); return; }
    txc::CwOptions c; c.o.rate = simulated_ ? 48000 : txRate(); c.o.mirror_q = mirror_->isChecked(); c.o.tx_ant = txAnt();
    c.power_w = cw_w_->value(); c.keyer.wpm = cw_wpm_->value(); c.keyer.weight = cw_weight_->value(); c.keyer.reverse = cw_rev_->isChecked();
    c.keyer.mode = cw_mode_->currentIndex() == 1 ? tx::IambicMode::A : tx::IambicMode::B; c.straight = cw_mode_->currentIndex() == 2; c.hang_s = cw_hang_->value() / 1000.0; c.text = text.toStdString();
    if (!(c.o.rate > 0)) { report("No playback rate for transmit."); return; }
    if (cw_side_->isChecked()) {
        if (!cw_tone_) cw_tone_ = std::make_unique<CwSidetone>(pc_audio_, this);
        CwSidetone* st = cw_tone_.get(); c.sidetone = [st](const uint8_t* k, size_t n, double r) { st->feed(k, n, r); };
    }
    std::shared_ptr<txc::ICwKeys> keys = cw_keys_ && cw_keys_->running() ? std::static_pointer_cast<txc::ICwKeys>(cw_keys_) : std::make_shared<NoKeys>();
    cw_over_ = true; rx_->setTxMute(true);
    const QString what = text.isEmpty() ? QString("CW (paddle) on %1 MHz, %2 W, %3 wpm").arg(mhz, 0, 'f', 4).arg(c.power_w, 0, 'f', 0).arg(c.keyer.wpm, 0, 'f', 0)
                                        : QString("CW \"%1\" on %2 MHz, %3 W, %4 wpm").arg(text).arg(mhz, 0, 'f', 4).arg(c.power_w, 0, 'f', 0).arg(c.keyer.wpm, 0, 'f', 0);
    if (!text.isEmpty()) saveCw();
    run(what, false, [this, mhz, c, keys](txc::TxControl& t) {
        auto r = t.cw(mhz, *keys, c, [this](const txc::Progress& p) { std::lock_guard<std::mutex> l(m_); last_ = p; have_last_ = true; });
        if (r.unkey_failed) { unkey_failed_ = true; return QString("UNKEY FAILED: ") + QString::fromStdString(r.stop); }
        if (!r.keyed) return QString("CW not keyed: ") + QString::fromStdString(r.why_not.empty() ? r.stop : r.why_not);
        if (r.max_fwd_w > 0.1) { last_tx_w_ = r.max_fwd_w; last_tx_t_ = QDateTime::currentSecsSinceEpoch(); }
        return QString("CW over: %1, %2 s, max %3 W (set %4 W)%5; first element %6 ms after the key%7").arg(QString::fromStdString(r.stop)).arg(r.seconds, 0, 'f', 1).arg(r.max_fwd_w, 0, 'f', 1)
               .arg(r.limited_to_w > 0 ? r.limited_to_w : c.power_w, 0, 'f', 0).arg(r.text_stopped ? QString(", text stopped by the paddle") : QString()).arg(1000 * r.key_delay_s, 0, 'f', 0)
               .arg(r.underruns > 4800 ? QString(", stream gaps %1").arg(r.underruns) : QString());
    });
}
