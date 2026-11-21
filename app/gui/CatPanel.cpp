#include "CatPanel.h"
#include <algorithm>
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSettings>
#include <QSpinBox>
#include <QTcpServer>
#include <QTcpSocket>
#include <QTimer>
#include <QVBoxLayout>
#include "ReceiverPanel.h"

struct CatPanel::Rx : fireside::rigctl::Backend {
    ReceiverPanel* rx; std::function<bool()> key, busy;
    double freq() const override { return rx->dialHz(); }
    int mode_index() const override { return rx->modeIndex(); }
    double width() const override { return rx->filterWidth(); }
    bool digital() const override { return rx->decoderIndex() >= 2; }                 // PSK31, RTTY, FT8, FT4
    bool keyed() const override { return key && key(); }
    bool tx_busy() const override { return keyed() || (busy && busy()); }
    void set_freq(double hz) override { rx->setDialHz(hz); }                           // the one tuning path (dialChanged -> RadioPanel -> Tuner)
    void set_mode(int i, double w) override { if (rx->modeIndex() != i) rx->setModeIndex(i); if (w > 0) rx->setFilterWidth(w); }
};

struct CatPanel::Fwd : fireside::rigctl::PttRequest {
    CatPanel* p; explicit Fwd(CatPanel* c) : p(c) {}
    bool request_ptt(int client, bool on) override { return p->ptt_ ? p->ptt_->request_ptt(client, on) : !on; }
    void heartbeat(int client) override { if (p->ptt_) p->ptt_->heartbeat(client); }
};

static bool no_settings() { return qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS"); }

CatPanel::CatPanel(ReceiverPanel* rx, std::function<bool()> keyed, std::function<bool()> tx_busy, QWidget* parent)
    : QWidget(parent), backend_(new Rx), server_(new QTcpServer(this)), fwd_(new Fwd(this)) {
    backend_->rx = rx; backend_->key = std::move(keyed); backend_->busy = std::move(tx_busy);
    auto* col = new QVBoxLayout(this); col->setContentsMargins(0, 0, 0, 0); col->setSpacing(6);
    auto* row = new QHBoxLayout; row->setSpacing(8);
    enable_ = new QCheckBox("CAT server for other programs (Hamlib NET rigctl)");
    enable_->setToolTip("Lets WSJT-X, JTDX, fldigi, GridTracker, WaveLogGate and other Hamlib programs read and set the frequency and mode. This computer only (127.0.0.1). They can key the radio only while \"Let other programs key\" is ticked on the Transmit card.");
    port_ = new QSpinBox; port_->setRange(1024, 65535); port_->setValue(fireside::rigctl::kDefaultPort); port_->setPrefix("port ");
    row->addWidget(enable_); row->addWidget(port_); row->addStretch(1); col->addLayout(row);
    status_ = new QLabel; status_->setObjectName("muted"); status_->setWordWrap(true); col->addWidget(status_);
    auto* help = new QLabel(QString("In the other program pick the rig \"Hamlib NET rigctl\" (model 2) with the address 127.0.0.1:%1, PTT method CAT, Split None. "
                                    "Its transmit requests are refused unless \"Let other programs key\" is ticked on the Transmit card (never remembered). "
                                    "Audio: it records \"Monitor of Fireside RX audio\" and plays into \"Fireside TX audio\" (Audio tab).").arg(fireside::rigctl::kDefaultPort));
    help->setObjectName("muted"); help->setWordWrap(true); col->addWidget(help);
    connect(server_, &QTcpServer::newConnection, this, &CatPanel::onNewConnection);
    connect(enable_, &QCheckBox::toggled, this, [this](bool on) { if (on) startServer(port_->value()); else stopServer(); save(); });
    connect(port_, &QSpinBox::editingFinished, this, [this] { if (enable_->isChecked() && port() != port_->value()) { stopServer(); startServer(port_->value()); } save(); });
    tick_ = new QTimer(this); tick_->setInterval(1000); connect(tick_, &QTimer::timeout, this, &CatPanel::updateStatus); tick_->start();
    if (!no_settings()) {
        QSettings q("fireside", "cat"); loading_ = true;
        port_->setValue(std::clamp(q.value("port", fireside::rigctl::kDefaultPort).toInt(), 1024, 65535));
        enable_->setChecked(q.value("enabled", true).toBool());                       // toggled -> startServer
        loading_ = false;
    }
    updateStatus();
}
CatPanel::~CatPanel() { stopServer(); }

void CatPanel::save() { if (loading_ || no_settings()) return; QSettings q("fireside", "cat"); q.setValue("enabled", enable_->isChecked()); q.setValue("port", port_->value()); }

bool CatPanel::startServer(int port) {
    if (server_->isListening()) stopServer();
    error_.clear();
    if (!server_->listen(QHostAddress::LocalHost, quint16(port))) { error_ = QString("Cannot listen on 127.0.0.1:%1 (%2)").arg(port).arg(server_->errorString()); updateStatus(); return false; }
    { QSignalBlocker b(enable_); enable_->setChecked(true); }
    updateStatus(); return true;
}
void CatPanel::stopServer() {
    for (auto& [s, sess] : sessions_) { release(s); s->disconnect(this); s->abort(); s->deleteLater(); }
    sessions_.clear(); ids_.clear(); server_->close(); updateStatus();
}
bool CatPanel::listening() const { return server_->isListening(); }
int CatPanel::port() const { return server_->isListening() ? int(server_->serverPort()) : 0; }

void CatPanel::onNewConnection() {
    while (QTcpSocket* s = server_->nextPendingConnection()) {
        const int id = next_id_++; ids_[s] = id;
        sessions_[s] = std::make_unique<fireside::rigctl::Session>(*backend_, fwd_.get(), id);
        connect(s, &QTcpSocket::readyRead, this, [this, s] { onReadyRead(s); });
        connect(s, &QTcpSocket::disconnected, this, [this, s] { release(s); sessions_.erase(s); ids_.erase(s); s->deleteLater(); updateStatus(); });
    }
    updateStatus();
}
void CatPanel::onReadyRead(QTcpSocket* s) {
    auto it = sessions_.find(s); if (it == sessions_.end()) return;
    fireside::rigctl::Session& sess = *it->second;
    const uint64_t r0 = sess.refused_ptt(), c0 = sess.commands();
    const std::string out = sess.feed(s->readAll().toStdString());
    if (!out.empty()) s->write(out.data(), qint64(out.size()));
    commands_ += sess.commands() - c0; if (sess.commands() != c0) last_ = QString::fromStdString(sess.last_command());
    if (sess.refused_ptt() != r0) { refused_ptt_ += sess.refused_ptt() - r0; updateStatus(); }
    if (sess.closed()) s->disconnectFromHost();
}

void CatPanel::release(QTcpSocket* s) { auto it = ids_.find(s); if (ptt_ && it != ids_.end()) ptt_->request_ptt(it->second, false); }   // gone: never left keyed
QString CatPanel::statusText() const { return status_->text(); }
void CatPanel::updateStatus() {
    QString t;
    if (!error_.isEmpty()) t = error_;
    else if (!server_->isListening()) t = "Off";
    else {
        t = QString("Listening on 127.0.0.1:%1, %2 program%3 connected").arg(server_->serverPort()).arg(sessions_.size()).arg(sessions_.size() == 1 ? "" : "s");
        if (commands_) t += QString(", %1 commands (last: %2)").arg(commands_).arg(last_);
        if (refused_ptt_) t += QString(". Transmit requests refused: %1 (see the Transmit card's status)").arg(refused_ptt_);
    }
    if (status_->text() != t) status_->setText(t);
}
