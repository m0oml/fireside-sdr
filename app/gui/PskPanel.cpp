#include "PskPanel.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QRandomGenerator>
#include <QSettings>
#include <QSslSocket>
#include <QStandardPaths>
#include <QTableWidget>
#include <QTimeZone>
#include <QTimer>
#include <QHostInfo>
#include <QUdpSocket>
#include <QVBoxLayout>
#include "AppLog.h"
#include "Theme.h"

namespace pskr = fireside::pskr;

// ---------------------------------------------------------------- PskrClient
PskrClient::PskrClient(QObject* parent) : QObject(parent), ping_(new QTimer(this)), retry_(new QTimer(this)) {
    client_id_ = QString("fireside-%1").arg(QRandomGenerator::global()->generate(), 8, 16, QChar('0'));    // generic: no callsign / name in the client id
    ping_->setInterval(45000); connect(ping_, &QTimer::timeout, this, [this] { if (sock_ && subscribed_) { const auto p = pskr::mqtt::pingreq(); sock_->write(reinterpret_cast<const char*>(p.data()), qint64(p.size())); } });
    retry_->setSingleShot(true); connect(retry_, &QTimer::timeout, this, [this] { if (running_) connectNow(); });
}
PskrClient::~PskrClient() { stop(); }
void PskrClient::setStatus(const QString& s) { if (s != status_) { status_ = s; emit statusChanged(s); } }
void PskrClient::start(const QString& call) {
    const std::string topic = pskr::sender_topic(call.toStdString());
    stop();
    if (topic.empty()) { setStatus(call.isEmpty() ? "no callsign set (Decoder card or Log tab)" : QString("%1 cannot be used on the feed (portable calls with \"/\" are not supported)").arg(call)); return; }
    call_ = call.toUpper(); running_ = true; backoff_s_ = 10; connectNow();
}
void PskrClient::stop() {
    running_ = false; retry_->stop(); ping_->stop(); subscribed_ = false;
    if (sock_) {
        if (sock_->state() == QAbstractSocket::ConnectedState) { const auto d = pskr::mqtt::disconnect(); sock_->write(reinterpret_cast<const char*>(d.data()), qint64(d.size())); sock_->flush(); }
        sock_->disconnect(this); sock_->abort(); sock_->deleteLater(); sock_ = nullptr;
    }
    setStatus("off");
}
void PskrClient::connectNow() {
    if (sock_) { sock_->disconnect(this); sock_->abort(); sock_->deleteLater(); }
    sock_ = new QSslSocket(this); dec_ = pskr::mqtt::Decoder(); subscribed_ = false;
    const bool tls = tls_ && QSslSocket::supportsSsl(); const int port = tls ? port_ : (tls_ ? pskr::kPortPlain : port_);
    connect(sock_, &QSslSocket::readyRead, this, &PskrClient::onReadyRead);
    connect(sock_, &QAbstractSocket::disconnected, this, [this] { onDropped("the feed closed the connection"); });
    connect(sock_, &QAbstractSocket::errorOccurred, this, [this](QAbstractSocket::SocketError) { onDropped(sock_ ? sock_->errorString() : QString("error")); });
    setStatus(QString("connecting to %1:%2%3 ...").arg(host_).arg(port).arg(tls ? " (TLS)" : ""));
    if (tls) { connect(sock_, &QSslSocket::encrypted, this, &PskrClient::onConnected); sock_->connectToHostEncrypted(host_, quint16(port)); }
    else { connect(sock_, &QAbstractSocket::connected, this, &PskrClient::onConnected); sock_->connectToHost(host_, quint16(port)); }
}
void PskrClient::onConnected() {
    const auto c = pskr::mqtt::connect(client_id_.toStdString(), 60); sock_->write(reinterpret_cast<const char*>(c.data()), qint64(c.size()));
}
void PskrClient::onReadyRead() {
    if (!sock_) return;
    const QByteArray b = sock_->readAll(); dec_.feed(reinterpret_cast<const uint8_t*>(b.constData()), size_t(b.size()));
    while (auto p = dec_.next()) {
        switch (p->type) {
            case pskr::mqtt::Type::Connack:
                if (p->connack_code != 0) { onDropped(QString("the feed refused the connection (code %1)").arg(p->connack_code)); return; }
                { const auto s = pskr::mqtt::subscribe(1, pskr::sender_topic(call_.toStdString())); sock_->write(reinterpret_cast<const char*>(s.data()), qint64(s.size())); }
                break;
            case pskr::mqtt::Type::Suback:
                if (p->suback_code == 0x80) { onDropped("the feed refused the subscription"); return; }
                subscribed_ = true; backoff_s_ = 10; ping_->start(); setStatus(QString("live: listening for reports of %1").arg(call_)); break;
            case pskr::mqtt::Type::Publish:
                if (auto s = pskr::parse_spot(p->payload)) { ++received_; emit spot(*s); }
                break;
            default: break;
        }
    }
    if (dec_.broken()) onDropped("garbled data from the feed");
}
void PskrClient::onDropped(const QString& why) {
    if (!running_ || retry_->isActive()) return;
    ping_->stop(); subscribed_ = false;
    if (sock_) { sock_->disconnect(this); sock_->abort(); sock_->deleteLater(); sock_ = nullptr; }
    setStatus(QString("%1; retrying in %2 s").arg(why).arg(backoff_s_));
    retry_->start(backoff_s_ * 1000); backoff_s_ = std::min(backoff_s_ * 2, 300);
}

// ---------------------------------------------------------------- PskPanel
namespace {
const qint64 kWindowS[] = {15 * 60, 60 * 60, 6 * 3600, 24 * 3600};
QString ago(qint64 s) { return s < 60 ? QString("%1 s").arg(s) : s < 3600 ? QString("%1 min").arg(s / 60) : QString("%1 h %2 min").arg(s / 3600).arg((s % 3600) / 60); }
}  // namespace
PskPanel::PskPanel(std::function<QString()> my_call, std::function<QString()> my_grid, std::function<QString()> dial_band, QWidget* parent)
    : QWidget(parent), my_call_(std::move(my_call)), my_grid_(std::move(my_grid)), dial_band_(std::move(dial_band)), client_(new PskrClient(this)) {
    now_ = [] { return QDateTime::currentSecsSinceEpoch(); };
    if (!qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) {
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/fireside"; QDir().mkpath(dir); csv_ = dir + "/pskreporter-heard.csv";
    }
    auto* col = new QVBoxLayout(this); col->setContentsMargins(12, 12, 12, 12); col->setSpacing(12);
    {   auto* f = new QFormLayout; f->setHorizontalSpacing(12);
        enable_ = new QCheckBox("Show where I am heard (PSK Reporter live feed)"); enable_->setChecked(true);
        enable_->setToolTip("Connects to mqtt.pskreporter.info and asks only for reports of your callsign as the sender. Nothing of yours is uploaded.");
        ant_ = new QLineEdit; ant_->setPlaceholderText("e.g. end-fed 20 m long, 8 m up"); ant_->setToolTip("Saved with every report in pskreporter-heard.csv, so antennas can be compared later");
        status_ = new QLabel("off"); status_->setObjectName("muted"); status_->setWordWrap(true);
        upload_ = new QCheckBox("Upload my decodes (I appear on the PSK Reporter map as a listening station)");
        upload_->setToolTip("FT8 / FT4 stations decoded live from the radio are reported under your callsign and locator, with frequency, SNR and the antenna note: "
                            "at most once every 5 minutes, each station once per band per hour (PSK Reporter's rules). Never decodes of a recording.");
        up_status_ = new QLabel("off"); up_status_->setObjectName("muted"); up_status_->setWordWrap(true);
        f->addRow(enable_); f->addRow(upload_); f->addRow("Antenna note", ant_); f->addRow("Feed", status_); f->addRow("Uploads", up_status_);
        auto* note = new QLabel("Reports come from other stations' decoding software, usually within a few minutes of your transmission. Only digital modes are reported. "
                                "The feed is live only: reports from before the app opened are not shown.");
        note->setObjectName("muted"); note->setWordWrap(true); f->addRow(note);
        col->addWidget(theme::makeCard("PSK Reporter", f)); }
    {   auto* v = new QVBoxLayout; auto* row = new QHBoxLayout; row->setSpacing(8);
        window_ = new QComboBox; window_->addItems({"Last 15 minutes", "Last hour", "Last 6 hours", "Last 24 hours"}); window_->setCurrentIndex(1);
        this_band_ = new QCheckBox("This band only"); row->addWidget(window_); row->addWidget(this_band_); row->addStretch(1);
        summary_ = new QLabel("Nothing heard yet."); summary_->setWordWrap(true); summary_->setTextInteractionFlags(Qt::TextSelectableByMouse);
        table_ = new QTableWidget(0, 11); table_->setHorizontalHeaderLabels({"Heard by", "Country", "Locator", "km", "Bearing", "Band", "Mode", "Last SNR", "Best SNR", "Reports", "Last heard"});
        table_->verticalHeader()->setVisible(false); table_->setEditTriggers(QAbstractItemView::NoEditTriggers); table_->setSelectionBehavior(QAbstractItemView::SelectRows);
        table_->horizontalHeader()->setStretchLastSection(true); table_->setSortingEnabled(false); table_->setColumnWidth(1, 170);
        v->addLayout(row); v->addWidget(summary_); v->addWidget(table_, 1);
        col->addWidget(theme::makeCard("Heard by", v, true), 1); }
    tick_ = new QTimer(this); tick_->setInterval(5000); connect(tick_, &QTimer::timeout, this, [this] { syncStation(); refresh(); }); tick_->start();
    connect(client_, &PskrClient::spot, this, &PskPanel::onSpot);
    connect(client_, &PskrClient::statusChanged, this, [this](const QString& s) { status_->setText(s); if (!s.startsWith("connecting")) appLog("pskreporter", s); });
    connect(enable_, &QCheckBox::toggled, this, [this](bool on) { save(); if (on) { call_.clear(); syncStation(); } else client_->stop(); });
    connect(ant_, &QLineEdit::editingFinished, this, [this] { save(); });
    connect(window_, &QComboBox::currentIndexChanged, this, [this] { save(); refresh(); }); connect(this_band_, &QCheckBox::toggled, this, [this] { save(); refresh(); });
    up_ = std::make_unique<pskr::Uploader>("", "", "fireside 1.0", QRandomGenerator::global()->generate(), QDateTime::currentSecsSinceEpoch(), QRandomGenerator::global()->generate());
    connect(upload_, &QCheckBox::toggled, this, [this](bool) { save(); up_status_->setText(uploadStatus()); });
    { auto* ut = new QTimer(this); ut->setInterval(5000); connect(ut, &QTimer::timeout, this, &PskPanel::uploadTick); ut->start(); }
    load();
    if (qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) { enable_->setChecked(false); upload_->setChecked(false); }   // tests never reach the real feed or uploader
    up_status_->setText(uploadStatus());
    QTimer::singleShot(1500, this, [this] { syncStation(); });                                  // after the window has its call / grid
}
void PskPanel::syncStation() {
    const QString call = my_call_ ? my_call_().trimmed().toUpper() : QString(), grid = my_grid_ ? my_grid_().trimmed() : QString();
    if (grid != grid_ || call != call_) heard_.set_station(call.toStdString(), grid.toStdString());
    if (call != call_) { call_ = call; if (enable_->isChecked()) client_->start(call); }
    grid_ = grid;
}
void PskPanel::onSpot(const pskr::Spot& s) {
    if (!heard_.add(s)) return;
    if (!csv_.isEmpty()) {
        QFile f(csv_); const bool fresh = !f.exists();
        if (f.open(QIODevice::Append | QIODevice::Text)) {
            double km = -1, brg = -1; for (const auto& h : heard_.since(0)) if (h.receiver == s.receiver && h.band == s.band) { km = h.km; brg = h.bearing; }
            if (fresh) f.write((pskr::csv_header() + "\n").c_str());
            f.write((pskr::csv_line(s, km, brg, ant_->text().toStdString()) + "\n").c_str());
        }
    }
    refresh();
}
int PskPanel::rows() const { return table_->rowCount(); }
QString PskPanel::summaryText() const { return summary_->text(); }
void PskPanel::refresh() {
    const qint64 now = now_(), w = kWindowS[std::clamp(window_->currentIndex(), 0, 3)], t0 = now - w;
    heard_.prune(now - 24 * 3600);
    const std::string band = this_band_->isChecked() && dial_band_ ? dial_band_().toStdString() : std::string();
    const auto rows = heard_.since(t0, band);
    table_->setRowCount(int(rows.size()));
    for (int i = 0; i < int(rows.size()); ++i) {
        const auto& h = rows[size_t(i)];
        auto cell = [&](int c, const QString& t, bool right = false) { auto* it = new QTableWidgetItem(t); if (right) it->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter); table_->setItem(i, c, it); };
        cell(0, QString::fromStdString(h.receiver)); cell(1, QString::fromStdString(h.country)); cell(2, QString::fromStdString(h.grid));
        cell(3, h.km >= 0 ? QString::number(qRound(h.km)) : QString(), true); cell(4, h.bearing >= 0 ? QString("%1°").arg(qRound(h.bearing)) : QString(), true);
        cell(5, QString::fromStdString(h.band)); cell(6, QString::fromStdString(h.mode));
        cell(7, h.last_snr > -99 ? QString("%1 dB").arg(h.last_snr) : QString(), true); cell(8, h.best_snr > -99 ? QString("%1 dB").arg(h.best_snr) : QString(), true);
        cell(9, QString::number(h.count), true);
        cell(10, QString("%1 UTC (%2 ago)").arg(QDateTime::fromSecsSinceEpoch(h.last_t, QTimeZone::UTC).toString("HH:mm")).arg(ago(std::max<qint64>(0, now - h.last_t))));
    }
    const auto sm = heard_.summary(t0, band);
    const QString win = window_->currentText().toLower();
    if (!sm.stations) { summary_->setText(call_.isEmpty() ? "Set your callsign (Decoder card or Log tab)." : QString("Nobody has reported %1 in the %2.").arg(call_, win)); short_.clear(); }
    else {
        QStringList bands; for (const auto& [b, n] : sm.by_band) bands << QString("%1 %2").arg(QString::fromStdString(b)).arg(n);
        summary_->setText(QString("Heard by <b>%1</b> station%2 in <b>%11</b> countr%12 (%3 report%4) in the %5. Furthest: %6%7. Best signal: %8 dB at %9. By band: %10.")
                              .arg(sm.stations).arg(sm.stations == 1 ? "" : "s").arg(sm.spots).arg(sm.spots == 1 ? "" : "s").arg(win)
                              .arg(QString::fromStdString(sm.furthest)).arg(sm.max_km >= 0 ? QString(", %1 km").arg(qRound(sm.max_km)) : QString())
                              .arg(sm.best_snr).arg(QString::fromStdString(sm.best)).arg(bands.join(", ")).arg(sm.countries).arg(sm.countries == 1 ? "y" : "ies"));
        short_ = QString("Heard by %1 (%2)%3").arg(sm.stations).arg(window_->currentText().mid(5)).arg(sm.max_km >= 0 ? QString(", furthest %1 km").arg(qRound(sm.max_km)) : QString());
    }
    emit shortSummaryChanged(short_);
}
void PskPanel::load() {
    if (qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) return;
    loading_ = true; QSettings q("fireside", "pskreporter");
    enable_->setChecked(q.value("enabled", true).toBool()); ant_->setText(q.value("antenna").toString()); upload_->setChecked(q.value("upload", false).toBool());
    window_->setCurrentIndex(q.value("window", 1).toInt()); this_band_->setChecked(q.value("this_band", false).toBool());
    loading_ = false;
}
void PskPanel::save() {
    if (loading_ || qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) return;
    QSettings q("fireside", "pskreporter"); q.setValue("enabled", enable_->isChecked()); q.setValue("upload", upload_->isChecked()); q.setValue("antenna", ant_->text()); q.setValue("window", window_->currentIndex()); q.setValue("this_band", this_band_->isChecked());
}

// ---------------------------------------------------------------- uploading my decodes
void PskPanel::onDecodeLine(const QString& line, double dial_hz, const QString& mode) {
    if (!upload_->isChecked()) return;
    syncStation(); up_->set_station(call_.toStdString(), grid_.toStdString()); up_->set_antenna(ant_->text().trimmed().toStdString());
    if (auto r = pskr::report_from_line(line.toStdString(), dial_hz, mode.toStdString(), now_(), call_.toStdString())) up_->add(*r);
    up_status_->setText(uploadStatus());
}
QString PskPanel::uploadStatus() const {
    if (!upload_->isChecked()) return "off: nothing of yours is sent";
    if (call_.isEmpty() || grid_.isEmpty()) return "waiting for your callsign and locator (Decoder card / Log tab)";
    QString s = QString("%1 report%2 sent in %3 datagram%4").arg(up_->sent_reports()).arg(up_->sent_reports() == 1 ? "" : "s").arg(up_->datagrams()).arg(up_->datagrams() == 1 ? "" : "s");
    if (last_upload_) s += QString(", last at %1 UTC").arg(QDateTime::fromSecsSinceEpoch(last_upload_, QTimeZone::UTC).toString("HH:mm"));
    if (up_->pending()) s += QString("; %1 waiting (next in about %2 min)").arg(up_->pending()).arg(std::max<qint64>(0, (up_->next_send() - now_() + 59) / 60));
    if (up_errors_) s += QString("; %1 send error(s)").arg(up_errors_);
    return s;
}
void PskPanel::uploadTick() {
    if (!upload_->isChecked()) return;
    up_->set_station(call_.toStdString(), grid_.toStdString()); up_->set_antenna(ant_->text().trimmed().toStdString());
    const qint64 now = now_(); if (up_->due(now)) sendDatagram(now);
    up_status_->setText(uploadStatus());
}
void PskPanel::flushUploads() { if (upload_->isChecked() && up_->pending()) sendDatagram(now_()); }
void PskPanel::sendDatagram(int64_t now) {
    if (!udp_) { udp_ = new QUdpSocket(this); udp_->bind(QHostAddress::AnyIPv4, 0); }      // one socket: the same source port for every datagram (PSK Reporter asks)
    if (up_addr_.isNull()) {                                                              // the real collector, looked up once
        const QHostInfo hi = QHostInfo::fromName(pskr::kUploadHost);
        for (const auto& a : hi.addresses()) if (a.protocol() == QAbstractSocket::IPv4Protocol) { up_addr_ = a; break; }
        up_port_ = quint16(pskr::kUploadPort);
        if (up_addr_.isNull()) { ++up_errors_; appLog("pskreporter", QString("upload: cannot resolve %1").arg(pskr::kUploadHost)); return; }
    }
    const auto d = up_->build(now); if (d.empty()) return;
    const qint64 n = udp_->writeDatagram(reinterpret_cast<const char*>(d.data()), qint64(d.size()), up_addr_, up_port_);
    if (n != qint64(d.size())) { ++up_errors_; appLog("pskreporter", "upload: send failed: " + udp_->errorString()); }
    else { last_upload_ = now; appLog("pskreporter", QString("uploaded: %1 reports so far, datagram %2 (%3 bytes)").arg(up_->sent_reports()).arg(up_->datagrams()).arg(d.size())); }
}
