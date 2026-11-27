// The Transmit card's WSPR part (shown in the digital controls while the decoder is WSPR):
//  - the beacon: attended, it runs until Stop, unticking, a band or decoder change, a refusal or a safety stop, or receive being lost. Each over is
//    TxControl::send_wspr (the gate, its limits, the hard drive clamp, the closed-loop power), 1..20 W, in one two-minute slot in N chosen at random,
//    only in the slot it was planned for (never a later one). Receive pauses for the over and comes back for the other slots.
//  - the wsprnet.org upload of the spots heard live: off until ticked (remembered). One POST per spot (WSJT-X's protocol), a "wsprstat" for a slot with none.
#include "TransmitCard.h"
#include "RadioPanel.h"
#include "ReceiverPanel.h"
#include "fireside/tx_meter.h"
#include "fireside/wspr.h"
#include <QCheckBox>
#include <QDateTime>
#include <QTimeZone>
#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QPushButton>
#include <QRandomGenerator>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>
#include <chrono>
#include <cmath>

using namespace fireside;
namespace {
bool wspr_no_settings() { return qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS"); }
double utc_now() { return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count(); }
QString hhmm(double utc) { return QDateTime::fromMSecsSinceEpoch(qint64(utc * 1000), QTimeZone::UTC).toString("HH:mm"); }
constexpr double kPrepareS = 2.5;            // the over is started this long before its slot (receive pauses, the 110 s signal is rendered)
const char* kVersion = "Fireside 1";         // wsprnet's "version" field
}  // namespace

void TransmitCard::buildWspr() {
    wspr_beacon_ = new QPushButton("Beacon"); wspr_beacon_->setCheckable(true);
    wspr_beacon_->setToolTip("Transmit a WSPR beacon (your call, 4-character locator and power) in about one two-minute slot in N, chosen at random, and decode the others. "
                             "Attended: it runs until you press Stop or untick it, change band or decoder, or a limit stops an over. Needs a measured drive level on the band");
    wspr_w_ = new QDoubleSpinBox; wspr_w_->setRange(1, txc::TxControl::kWsprMaxWatts); wspr_w_->setDecimals(0); wspr_w_->setValue(5); wspr_w_->setSuffix(" W");
    wspr_w_->setToolTip("Beacon power, 1-20 W (sent in the message as the nearest WSPR dBm: 5 W = 37 dBm). Each over keys for 110.6 s");
    wspr_every_ = new QSpinBox; wspr_every_->setRange(2, 10); wspr_every_->setValue(5); wspr_every_->setPrefix("1 slot in ");
    wspr_every_->setToolTip("How often the beacon transmits: on average one two-minute slot in this many (5 = 20 %, WSJT-X's default)");
    wspr_upload_ = new QCheckBox("Upload spots to wsprnet.org");
    wspr_upload_->setToolTip("Send every WSPR spot heard live (and a status report for slots with none) to wsprnet.org with your callsign and locator, as WSJT-X does");
    auto* stop = new QPushButton("Stop"); stop->setObjectName("txstop"); stop->setToolTip("Unkey now and end the beacon");
    wspr_note_ = new QLabel; wspr_note_->setWordWrap(true);
    wspr_part_ = new QWidget(this); auto* v = new QVBoxLayout(wspr_part_); v->setContentsMargins(0, 0, 0, 0); v->setSpacing(6);
    auto* r1 = new QHBoxLayout; r1->setSpacing(8); for (QWidget* w : {static_cast<QWidget*>(wspr_beacon_), static_cast<QWidget*>(wspr_w_), static_cast<QWidget*>(wspr_every_), static_cast<QWidget*>(stop)}) r1->addWidget(w); r1->addStretch(1);
    auto* r2 = new QHBoxLayout; r2->addWidget(wspr_upload_); r2->addStretch(1);
    v->addLayout(r1); v->addLayout(r2); v->addWidget(wspr_note_);
    if (auto* dl = qobject_cast<QVBoxLayout*>(digi_part_->layout())) dl->addWidget(wspr_part_);
    wspr_timer_ = new QTimer(this); wspr_timer_->setSingleShot(true);
    connect(wspr_timer_, &QTimer::timeout, this, [this] { startWsprOver(wspr_next_utc_); });
    wspr_up_timer_ = new QTimer(this); wspr_up_timer_->setInterval(250); connect(wspr_up_timer_, &QTimer::timeout, this, &TransmitCard::wsprPump);
    connect(wspr_beacon_, &QPushButton::toggled, this, &TransmitCard::wsprBeacon);
    connect(stop, &QPushButton::clicked, this, [this] { stop_->click(); });
    connect(wspr_w_, &QDoubleSpinBox::editingFinished, this, [this] { saveWspr(); });
    connect(wspr_every_, &QSpinBox::editingFinished, this, [this] { saveWspr(); });
    connect(wspr_upload_, &QCheckBox::toggled, this, [this](bool on) { saveWspr(); report(on ? "WSPR spots heard live will be uploaded to wsprnet.org." : "WSPR spots are no longer uploaded."); });
    auto show_for = [this](int i) { ft_box_->setVisible(i != 6); wspr_part_->setVisible(i == 6); };
    connect(rx_, &ReceiverPanel::decoderSelected, this, [this, show_for](int i) { show_for(i); if (i != 6 && wspr_on_) stopEverything("Decoder changed: the WSPR beacon is stopped."); });
    connect(rx_, &ReceiverPanel::wsprSlot, this, &TransmitCard::onWsprSlot);
    show_for(rx_->decoderIndex());
    loadWspr();
}
void TransmitCard::loadWspr() {
    if (wspr_no_settings()) return;
    QSettings q("fireside", "transmit");
    wspr_w_->setValue(q.value("wspr_w", 5).toDouble()); wspr_every_->setValue(q.value("wspr_every", 5).toInt());
    QSignalBlocker b(wspr_upload_); wspr_upload_->setChecked(q.value("wspr_upload", false).toBool());
}
void TransmitCard::saveWspr() {
    if (wspr_no_settings()) return;
    QSettings q("fireside", "transmit"); q.setValue("wspr_w", wspr_w_->value()); q.setValue("wspr_every", wspr_every_->value()); q.setValue("wspr_upload", wspr_upload_->isChecked());
}

QString TransmitCard::wsprRefusal() {
    if (!tx_ || !tx_->armed()) return "transmit is not ready";
    if (rx_->decoderIndex() != 6) return "pick WSPR first (Digital modes)";
    const double dial = rx_->dialHz(); const int band = txm::band_of(dial);
    if (band < 0) return "the dial is outside the amateur bands";
    const double want = wspr::dial_hz_for(dial);
    if (!(want > 0) || std::fabs(dial - want) > 1) return QString("the dial is not on this band's WSPR frequency (press WSPR to go to %1 MHz)").arg(want > 0 ? want / 1e6 : 14.0956, 0, 'f', 4);
    const QString call = rx_->myCall(), grid = myGrid4();
    if (call.isEmpty() || grid.isEmpty()) return "set your callsign (Decoder card or Log tab) and 4-character locator (Log tab) first";
    if (!wspr::plain_call(call.toStdString())) return QString("\"%1\" cannot go in a WSPR beacon message (a call with / needs a type 2/3 message, not supported)").arg(call);
    if (!(tx_->k_for(band) > 0)) return "no measured drive level on this band: press Measure drive first (Tuner settings, Calibration)";
    if (ft_pending_ || att_pause_ || ft_repeat_->isChecked() || (seq_ && seq_->active()) || ext_over_) return "an FT8 / FT4 QSO, CQ or another program's over is running";
    return QString();
}
void TransmitCard::wsprBeacon(bool on) {
    if (!on) { if (wspr_on_) { wsprOff("WSPR beacon off."); } return; }
    if (const QString why = wsprRefusal(); !why.isEmpty()) { QSignalBlocker b(wspr_beacon_); wspr_beacon_->setChecked(false); report("WSPR beacon not started: " + why + "."); updateButtons(); return; }
    wspr_on_ = true; saveWspr(); updateButtons(); scheduleWspr();
}
void TransmitCard::wsprOff(const QString& why) {
    const bool was = wspr_on_; wspr_on_ = false; wspr_timer_->stop(); wspr_next_utc_ = 0;
    { QSignalBlocker b(wspr_beacon_); wspr_beacon_->setChecked(false); }
    if (was && worker_running_ && tx_ && !ext_over_) tx_->abort();                          // an over in progress unkeys now
    if (!why.isEmpty()) report(why);
    wspr_note_->setText(QString()); updateButtons();
}
void TransmitCard::scheduleWspr() {
    if (!wspr_on_) return;
    if (const QString why = wsprRefusal(); !why.isEmpty()) { wsprOff("WSPR beacon stopped: " + why + "."); return; }
    const double now = utc_now(); const int n = wspr_every_->value();
    long long k = (long long)std::floor((now + kPrepareS + 1.0) / wspr::kSlotS) + 1;         // the first slot we can still be ready for
    auto* rng = QRandomGenerator::global();
    for (int i = 0; i < 3 * n && rng->bounded(n) != 0; ++i) ++k;                             // about one slot in n
    wspr_next_utc_ = double(k) * wspr::kSlotS;
    wspr_hz_ = 1500 + rng->bounded(-80, 81);                                                 // spread inside the 200 Hz sub-band, as WSJT-X does
    wspr_timer_->start(int(std::max(0.0, wspr_next_utc_ - kPrepareS - now) * 1000));
    const int dbm = wspr::dbm_for_watts(wspr_w_->value());
    const QString m = QString("WSPR beacon on: next over %1 UTC at %2 Hz, %3 W (%4 dBm), 1 slot in %5. Decoding the other slots. Stop ends it.")
                          .arg(hhmm(wspr_next_utc_)).arg(wspr_hz_).arg(wspr_w_->value(), 0, 'f', 0).arg(dbm).arg(n);
    wspr_note_->setText(m); if (!worker_running_) report(m);
    updateButtons();
}
void TransmitCard::startWsprOver(double slot_utc) {
    if (!wspr_on_) return;
    if (worker_running_) { scheduleWspr(); return; }                                         // something else is transmitting: the next slot
    if (const QString why = wsprRefusal(); !why.isEmpty()) { wsprOff("WSPR beacon stopped: " + why + "."); return; }
    const double mhz = rx_->dialHz() / 1e6, w = wspr_w_->value(); const int hz = wspr_hz_;
    const std::string call = rx_->myCall().toStdString(), grid = myGrid4().toStdString();
    const tx::Parity par = ((long long)std::llround(slot_utc / wspr::kSlotS)) % 2 ? tx::Parity::Odd : tx::Parity::Even;
    txc::Options o; o.rate = simulated_ ? 48000 : txRate(); o.mirror_q = mirror_->isChecked(); o.tx_ant = txAnt();
    if (!(o.rate > 0)) { wsprOff("WSPR beacon stopped: the radio's playback offers no 8-channel rate."); return; }
    emit txSent(QDateTime::fromMSecsSinceEpoch(qint64(slot_utc * 1000), QTimeZone::UTC).toString("HHmmss"), hz, QString("WSPR %1 %2 %3").arg(QString::fromStdString(call), QString::fromStdString(grid)).arg(wspr::dbm_for_watts(w)));
    run(QString("WSPR beacon: sending %1 %2 %3 dBm at %4 UTC").arg(QString::fromStdString(call), QString::fromStdString(grid)).arg(wspr::dbm_for_watts(w)).arg(hhmm(slot_utc)), true,
        [this, mhz, w, hz, call, grid, par, o, slot_utc](txc::TxControl& t) {
            auto r = t.send_wspr(mhz, call, grid, hz, par, w, o, [this](const txc::Progress& p) { std::lock_guard<std::mutex> l(m_); last_ = p; have_last_ = true; }, {}, 0.3, slot_utc);
            if (r.unkey_failed) { unkey_failed_ = true; return QString("UNKEY FAILED: ") + QString::fromStdString(r.stop); }
            if (!r.sent && !r.why_not.empty()) return QString("Not sent: WSPR ") + QString::fromStdString(r.why_not);
            QString res = QString("WSPR %1 at %2 UTC: %3, %4 W (set %5 W)%6.").arg(r.sent ? "sent" : "stopped", hhmm(r.slot_utc), QString::fromStdString(r.stop))
                              .arg(r.mean_fwd_w, 0, 'f', 1).arg(w, 0, 'f', 0).arg(r.swr ? QString(", SWR %1").arg(*r.swr, 0, 'f', 2) : QString());
            unkeyed_utc_ = r.unkeyed_utc;
            if (r.sent && r.mean_fwd_w > 0.1) { last_tx_w_ = r.mean_fwd_w; last_tx_t_ = QDateTime::currentSecsSinceEpoch(); }
            if (r.sent) res += " #sent";
            return res;
        });
}

void TransmitCard::onWsprSlot(const QString& date, const QString& hhmm_label, double dial_mhz, const QStringList& lines, bool ok, const QString& error) {
    if (!ok) { wspr_note_->setText("WSPR decode failed: " + error); return; }
    if (!wspr_upload_->isChecked()) return;
    const QString call = rx_->myCall(), grid = myGrid4();
    if (call.isEmpty() || grid.isEmpty()) { wspr_note_->setText("wsprnet upload skipped: set your callsign and locator first."); return; }
    const std::string rc = call.toStdString(), rg = grid.toStdString();
    int queued = 0;
    for (const QString& l : lines) if (auto s = wspr::parse_wsprd_line(l.toStdString())) { s->hhmm = hhmm_label.toStdString(); wspr_queue_.append({QByteArray::fromStdString(wspr::wsprnet_spot_body(*s, date.toStdString(), rc, rg, dial_mhz, kVersion)), true, 0}); ++queued; }
    if (!queued) {                                                                           // no spots: tell wsprnet we listened (and our beacon settings)
        const int tpct = wspr_on_ ? int(std::lround(100.0 / wspr_every_->value())) : 0;
        wspr_queue_.append({QByteArray::fromStdString(wspr::wsprnet_stat_body(rc, rg, dial_mhz, tpct, dial_mhz + wspr_hz_ / 1e6, wspr::dbm_for_watts(wspr_w_->value()), kVersion)), false, 0});
    }
    if (!wspr_up_timer_->isActive()) wspr_up_timer_->start();
}
void TransmitCard::wsprPump() {
    if (wspr_queue_.isEmpty()) { wspr_up_timer_->stop(); return; }
    if (!wspr_net_) wspr_net_ = new QNetworkAccessManager(this);
    WsprPost p = wspr_queue_.takeFirst();
    QNetworkRequest req{QUrl(QString::fromLatin1(wspr::kWsprnetUrl))}; req.setHeader(QNetworkRequest::ContentTypeHeader, "application/x-www-form-urlencoded");
    req.setTransferTimeout(20000);
    QNetworkReply* reply = wspr_net_->post(req, p.body);
    connect(reply, &QNetworkReply::finished, this, [this, reply, p]() mutable {
        reply->deleteLater();
        const QByteArray answer = reply->readAll();
        const bool good = reply->error() == QNetworkReply::NoError && (!p.spot || answer.contains("spot(s) added"));
        if (good) { if (p.spot) ++wspr_sent_; wspr_note_->setText(QString("wsprnet: %1 spot(s) uploaded%2.").arg(wspr_sent_).arg(wspr_failed_ ? QString(", %1 failed (last: %2)").arg(wspr_failed_).arg(wspr_last_error_) : QString())); return; }
        wspr_last_error_ = reply->error() != QNetworkReply::NoError ? reply->errorString() : QString::fromUtf8(answer.left(80)).trimmed();
        if (++p.tries < 2) { wspr_queue_.append(p); if (!wspr_up_timer_->isActive()) wspr_up_timer_->start(); return; }   // one retry, then let it go
        ++wspr_failed_; wspr_note_->setText(QString("wsprnet: %1 spot(s) uploaded, %2 failed (last: %3).").arg(wspr_sent_).arg(wspr_failed_).arg(wspr_last_error_));
    });
}
