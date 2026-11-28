#include "RadioPanel.h"
#include <stdexcept>
#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QSettings>
#include <algorithm>
#include <QPushButton>
#include <QThread>
#include <QTimer>
#include "fireside/dsp.h"
#include <QVBoxLayout>
#include "ReceiverPanel.h"
#include "Theme.h"
#include "fireside/firewire.h"
#include "fireside/rxant.h"
using namespace fireside;

RadioPanel::RadioPanel(ReceiverPanel* rx, RadioSession::Opener opener, bool simulated, QWidget* parent)
    : QWidget(parent), rx_(rx), session_(std::move(opener), [](double s) { QThread::msleep(static_cast<unsigned long>(s * 1000)); }), simulated_(simulated) {
    auto* col = new QVBoxLayout(this); col->setContentsMargins(0, 0, 0, 0); col->setSpacing(12);
    auto* r1 = new QHBoxLayout; device_ = new QComboBox; device_->setMinimumWidth(320); refresh_ = new QPushButton("Refresh");
    order_ = new QComboBox; order_->addItems({"byte order: auto", "byte order: big", "byte order: little"});
    ant_ = new QComboBox; ant_->addItems({"ANT1", "ANT2", "ANT3", "RX1IN", "NC"});
    allow_ = new QCheckBox("Allow writes (needed to start the radio)"); allow_->setChecked(false);
    auto_ = new QCheckBox("Start receiving when the app opens"); auto_->setChecked(true);
    auto_->setToolTip("Connect, start the radio (receive only) and open the stream at the last frequency and rate as soon as the app opens, with no question. Transmit then checks the EEPROM by itself.");
    if (simulated) { auto* sim = new QLabel("SIMULATED: no hardware"); sim->setStyleSheet(QString("color: %1; font-weight: 600;").arg(theme::kWarn)); r1->addWidget(sim); }
    ant_->setVisible(false);                                                         // the Antennas tab chooses the antenna now (this box is only the fallback without it)
    r1->addWidget(device_, 1); r1->addWidget(refresh_); r1->addWidget(order_); r1->addWidget(ant_); r1->addWidget(allow_); r1->addWidget(auto_);
    auto* r2 = new QHBoxLayout; connect_ = new QPushButton("Connect (read-only)"); identify_ = new QPushButton("Identify (read-only)");
    start_ = new QPushButton("Start radio (receive only)..."); stop_ = new QPushButton("Stop radio"); disconnect_ = new QPushButton("Disconnect");
    quick_ = new QPushButton("Start receiving"); quick_->setObjectName("primary"); quick_->setToolTip("One click: connect, start the radio (receive only; asks once) and open the live stream, then show the Receiver tab.");
    alsa_ = new QPushButton("ALSA capture..."); alsa_->setToolTip("The old way to open a live source: any ALSA capture card at 48 kHz. Needed only for audio out of the radio's own jacks (the direct 48/96/192 stream plays on the PC).");
    r2->addWidget(quick_); for (auto* b : {connect_, identify_, start_, stop_, disconnect_, alsa_}) r2->addWidget(b);
    r2->addStretch(1);
    status_ = new QLabel; status_->setWordWrap(true); status_->setTextInteractionFlags(Qt::TextSelectableByMouse);
    start_->setObjectName("primary"); status_->setObjectName("muted");
    auto* session = new QVBoxLayout; session->setSpacing(8); session->addLayout(r2); session->addWidget(status_);
    col->addWidget(theme::makeCard("FireWire radio", r1)); col->addWidget(theme::makeCard("Session", session));
    connect(refresh_, &QPushButton::clicked, this, &RadioPanel::refreshDevices);
    connect(connect_, &QPushButton::clicked, this, [this] { connectRadio(); });
    connect(identify_, &QPushButton::clicked, this, [this] { identifyRadio(); });
    connect(start_, &QPushButton::clicked, this, [this] { startRadio(false); });
    connect(quick_, &QPushButton::clicked, this, [this] { quickStart(false); });
    connect(alsa_, &QPushButton::clicked, this, [this] { rx_->openLive(); });
    connect(stop_, &QPushButton::clicked, this, [this] { stopRadio(); });
    connect(disconnect_, &QPushButton::clicked, this, [this] { disconnectRadio(); });
    connect(rx_, &ReceiverPanel::dialChanged, this, &RadioPanel::onDial); connect(rx_, &ReceiverPanel::preampRequested, this, &RadioPanel::onPreamp);
    timer_ = new QTimer(this); timer_->setInterval(20); connect(timer_, &QTimer::timeout, this, &RadioPanel::tick); timer_->start();
    refreshDevices(); loadSettings(); updateButtons();
    for (auto* c : {device_, order_, ant_}) connect(c, &QComboBox::currentIndexChanged, this, [this] { saveSettings(); });
    connect(auto_, &QCheckBox::toggled, this, [this] { saveSettings(); });                 // remembered as you change them: the Radio tab is only needed to change something
}
void RadioPanel::loadSettings() {
    if (simulated_ || qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) return;
    QSettings q("fireside", "radio"); QSignalBlocker a(device_), b(order_), c(ant_);
    const int di = device_->findData(q.value("device").toString()); if (di >= 0) device_->setCurrentIndex(di);                                  // else the radio node chosen by refreshDevices()
    order_->setCurrentIndex(std::clamp(q.value("byte_order", 0).toInt(), 0, order_->count() - 1)); ant_->setCurrentIndex(std::clamp(q.value("antenna", 0).toInt(), 0, ant_->count() - 1));
    auto_->setChecked(q.value("autostart", true).toBool());
    // "Allow writes" is deliberately NOT remembered: every new window starts with writes off; the start question grants them for that session
}
void RadioPanel::saveSettings() {
    if (simulated_ || qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) return;
    QSettings q("fireside", "radio"); q.setValue("device", device_->currentData().toString()); q.setValue("byte_order", order_->currentIndex()); q.setValue("antenna", ant_->currentIndex()); q.setValue("autostart", auto_->isChecked());
}
void RadioPanel::autoStart() {
    if (simulated_ || !auto_->isChecked() || qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) return;
    if (device_->count() == 0) refreshDevices();
    if (device_->count() == 0) { report("No FireWire radio found: switch the radio on, then press Start receiving."); return; }
    if (!before_stream_) { quickStart(true, 0); return; }                                    // the last frequency (receiver settings) and the last stream rate
    if (session_.state() == RadioSession::State::Disconnected) { allow_->setChecked(true); if (!connectRadio()) return; }
    if (!startRadio(true)) return;                                                          // the radio up first (receive only) ...
    before_stream_([this] { if (session_.state() == RadioSession::State::Started) { if (!simulated_) rx_->openLiveAuto(); emit radioStarted(); } });   // ... the EEPROM check, then the stream
}
RadioPanel::~RadioPanel() = default;

void RadioPanel::refreshDevices() {
    const QString prev = device_->currentData().toString();                                  // Refresh keeps your choice if that node is still there
    device_->clear();
    if (simulated_) { device_->addItem("Simulated FLEX-5000A", "simulated"); return; }
    for (const auto& n : list_fw_devices()) {
        if (!n.error.empty()) continue;
        device_->addItem(QString("%1   guid %2   %3").arg(QString::fromStdString(n.path)).arg(n.guid ? QString::number(qulonglong(*n.guid), 16).toUpper() : "-").arg(n.is_local ? "(local controller: not the radio)" : "radio node"),
                         QString::fromStdString(n.path));
    }
    for (int i = 0; i < device_->count(); ++i) if (device_->itemText(i).contains("radio node")) { device_->setCurrentIndex(i); break; }          // the radio, not the local controller (which is listed first)
    if (const int k = device_->findData(prev); !prev.isEmpty() && k >= 0) device_->setCurrentIndex(k);
    if (!device_->count()) report("No /dev/fw* nodes (no FireWire controller, or firewire-ohci not loaded).");
}
void RadioPanel::requestRxAntenna(const QString& name, const QString& why) {
    if (session_.state() != RadioSession::State::Started) return;
    const auto a = fireside::rx_ant_from_name(name.toStdString()); if (!a) return;
    switch (session_.set_rx_antenna(*a)) {
        case fireside::AntResult::Sent: emit rxAntennaChanged(name); emit message(QString("RX antenna %1%2.").arg(name == "RX1IN" ? QString("RX1 IN") : name, why.isEmpty() ? QString() : QString(" (%1)").arg(why))); break;
        case fireside::AntResult::Fault: report("RX antenna switch failed (mailbox fault): the radio may still be on the previous antenna. Press Stop radio, then start again."); break;
        default: break;                                                          // Unchanged: already on it; NotArmed: not running
    }
}
void RadioPanel::setTxLines(const fireside::LineConfig& c) { session_.configure_tx_lines(c); applyTxLines(); }
void RadioPanel::applyTxLines() {
    if (session_.state() == RadioSession::State::Started) {
        switch (session_.apply_tx_lines(rx_->dialHz())) {
            case fireside::TxLines::Result::Fault: report("A rear TX line could not be set (mailbox fault): transmit is refused until the radio is stopped and started again."); break;
            default: break;                                                      // Busy: the over in progress set them itself; Unchanged / Sent: fine
        }
    }
    const auto st = session_.tx_lines_state(); emit txLinesState(st[0], st[1], st[2]);
}
void RadioPanel::report(const QString& m) { last_error_ = m; emit message(m); }
QString RadioPanel::statusText() const { return status_->text(); }

bool RadioPanel::connectRadio() {
    if (device_->currentIndex() < 0) { report("Select a device first."); return false; }
    SessionOptions o; o.path = device_->currentData().toString().toStdString(); o.allow_write = allow_->isChecked(); o.boot_wait_s = 3.0;   // short: this runs on the GUI thread
    o.byte_order = order_->currentIndex() == 1 ? "big" : order_->currentIndex() == 2 ? "little" : "auto";
    QApplication::setOverrideCursor(Qt::WaitCursor); bool ok = true;
    try { session_.connect(o); last_error_.clear(); emit message(QString("Connected (%1 byte order proven by a firmware read). Nothing was written.").arg(session_.byte_order() == ByteOrder::Big ? "big-endian" : "little-endian")); }
    catch (const std::exception& e) { ok = false; report(QString("Connect failed: ") + e.what()); }
    QApplication::restoreOverrideCursor(); updateButtons(); return ok;
}
void RadioPanel::identifyRadio() {
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try { emit message(QString::fromStdString(session_.identify().describe())); }
    catch (const std::exception& e) { report(QString("Identify failed: ") + e.what()); }
    QApplication::restoreOverrideCursor();
}
bool RadioPanel::confirmStart(double mhz) {
    const QString ant = antenna_for_ ? QString::fromStdString(antenna_for_(mhz * 1e6)) : ant_->currentText();
    const auto r = QMessageBox::question(this, "Start the radio?",
        QString("This writes to the radio (receive only, the transmitter is never keyed):\n\n  INITIALIZE, RX preamp off, leave standby, RX1 antenna %1, tune to %2 MHz (the DDS sits 9 kHz below the dial)\n\n"
                "Stopping later switches MOX off, enters standby and turns PA bias off.\nHave an antenna or a 50-ohm load on %1 and nothing on the TX outputs. Start?").arg(ant).arg(mhz, 0, 'f', 6),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    return r == QMessageBox::Yes;
}
bool RadioPanel::startAtRate(unsigned rate, bool confirmed) {
    if (session_.state() == RadioSession::State::Started) { if (simulated_) return true; const bool ok = rx_->loadLiveDirect(rate); if (ok) emit radioStarted(); return ok; }   // only the stream restarts: the radio keeps its state
    return quickStart(confirmed, rate);
}
bool RadioPanel::quickStart(bool confirmed, unsigned rate) {
    if (session_.state() == RadioSession::State::Started) { if (!simulated_) { if (rate) rx_->loadLiveDirect(rate); else rx_->openLiveAuto(); } emit radioStarted(); return true; }      // already up: just (re)open the stream
    if (!confirmed && !confirmStart(rx_->dialHz() / 1e6)) return false;                                  // the one question; "Yes" also allows writes for this session
    if (session_.state() == RadioSession::State::Connected && !session_.allow_write()) disconnectRadio();   // connected read-only (identify): reconnect with writes
    if (session_.state() == RadioSession::State::Disconnected) {
        if (device_->currentIndex() < 0) refreshDevices();
        allow_->setChecked(true);                                                                       // visible: this session may write (it is unticked again by every new window)
        if (!connectRadio()) return false;
    }
    if (!startRadio(true)) return false;                                                                // already confirmed above
    if (!simulated_) { if (rate) rx_->loadLiveDirect(rate); else rx_->openLiveAuto(); }                  // the simulated radio has no FireWire stream; a real one must not be touched by a test
    emit radioStarted(); return true;
}
bool RadioPanel::startRadio(bool confirmed) {
    if (session_.state() != RadioSession::State::Connected) { report("Connect first."); return false; }
    if (!session_.allow_write()) { report("Writes are disabled for this session: tick \"Allow writes\" and connect again."); return false; }
    const double mhz = rx_->dialHz() / 1e6; const std::string ant = antenna_for_ ? antenna_for_(rx_->dialHz()) : ant_->currentText().toStdString();   // the band's antenna from the Antennas tab
    if (!confirmed && !confirmStart(mhz)) return false;
    QApplication::setOverrideCursor(Qt::WaitCursor); bool ok = true;
    try {
        last_error_.clear(); maybe_started_ = true;
        if (session_.if_offset_hz() != fireside::dsp::kIfOffsetHz) throw std::logic_error("the session's IF offset does not match the receiver's");   // checked before any write; kRadioIfOffsetHz == dsp::kIfOffsetHz is also a unit test
        session_.start(mhz, ant, [this](const std::string& m) { emit message(QString::fromStdString(m)); });
        rx_->setDialControlsRadio(true); rx_->setPreampControlsRadio(true); rx_->lockIfOffset(true);                      // the DSP must match the DDS: no IF offset while the radio is tuned exactly to the dial
        emit rxAntennaChanged(QString::fromStdString(ant)); applyTxLines();
        emit message(QString("Radio is up on %1 MHz (%2). The dial box and clicks on the spectrum now tune the radio. Start the I/Q capture next.").arg(mhz, 0, 'f', 6).arg(QString::fromStdString(ant)));
        // The start-up's antenna switch comes 13 ms after leaving standby and was not always taken (2026-11-28: shown ANT1, received on ANT2 until the
        // antenna was cycled). Once the radio has settled, the antenna it is meant to be on is written again (the same reviewed write).
        QTimer::singleShot(1500, this, [this] {
            if (session_.state() != RadioSession::State::Started) return;
            const auto cur = session_.rx_antenna();
            switch (session_.reassert_rx_antenna()) {
                case fireside::AntResult::Sent: if (cur) emit message(QString("RX antenna %1 sent again now the radio has settled.").arg(*cur == fireside::RxAnt::Rx1In ? QString("RX1 IN") : QString("ANT%1").arg(int(*cur)))); break;
                case fireside::AntResult::Fault: report("RX antenna switch failed (mailbox fault): the radio may be on another antenna. Press Stop radio, then start again."); break;
                default: break;
            }
        });
    } catch (const std::exception& e) {                                              // G11: a partial start may have taken the radio out of standby
        ok = false; report(QString("Start stopped: ") + e.what() + (session_.state() == RadioSession::State::Connected && session_.allow_write() ? " (the radio may be partly started: press Stop radio to put it back in standby)" : ""));
    }
    QApplication::restoreOverrideCursor(); updateButtons(); return ok;
}
bool RadioPanel::stopRadio() {
    QApplication::setOverrideCursor(Qt::WaitCursor); bool ok = true;
    try { session_.shutdown([this](const std::string& m) { emit message(QString::fromStdString(m)); }); last_error_.clear(); maybe_started_ = false; rx_->setDialControlsRadio(false); rx_->setPreampControlsRadio(false); rx_->lockIfOffset(false); emit rxAntennaChanged(QString()); applyTxLines(); const bool closed = rx_->closeLive();   // as PowerSDR: standby, then stop the audio (standby alone keeps receiving)
        emit message(QString("Radio stopped (MOX off, standby, PA bias off).%1 Standby is not power-off: the radio stays powered until you switch it off.").arg(closed ? " Live capture closed." : "")); }
    catch (const std::exception& e) {
        ok = false; report(QString("SHUTDOWN FAILED: ") + e.what() + " (power-cycle the radio if it looks wrong)");
        if (rx_->closeLive()) emit message("Live capture closed anyway (the radio's stream registers put back), so the next start is not refused.");
    }
    QApplication::restoreOverrideCursor(); updateButtons(); return ok;
}
void RadioPanel::disconnectRadio() {
    if (session_.state() == RadioSession::State::Started) { report("Stop the radio before disconnecting."); return; }
    session_.disconnect(); updateButtons();
}
bool RadioPanel::shutdownOnExit() {
    const bool started = session_.state() == RadioSession::State::Started, partial = maybe_started_ && session_.state() == RadioSession::State::Connected;
    if (!started && !partial) return true;
    return stopRadio();                                                                 // also after a failed (partial) start: put it back in standby if the mailbox still answers
}

void RadioPanel::onPreamp(bool on) {
    switch (session_.set_preamp(on)) {
        case PreampResult::Sent: emit message(on ? "Receive preamp ON." : "Receive preamp off."); break;
        case PreampResult::Unchanged: break;
        case PreampResult::NotArmed: rx_->setPreampChecked(false); report("The preamp can only be changed while the radio is started."); break;
        case PreampResult::Fault: rx_->setPreampChecked(session_.preamp_on()); report("Preamp change stopped after a mailbox fault."); break;
    }
}
void RadioPanel::onDial(double hz) {
    if (session_.state() != RadioSession::State::Started) return;                     // before a session the dial is only a label
    switch (session_.tune(hz)) {
        case TuneResult::Rejected: report("Frequency out of range for the receiver (0..65 MHz)."); break;
        case TuneResult::Fault: report(QString("Tuning stopped after a mailbox fault: ") + QString::fromStdString(session_.tuner() ? session_.tuner()->last_error() : "")); break;
        default: break;
    }
    applyTxLines();                                                                     // the band lines follow the dial (idempotent inside a band)
}
void RadioPanel::tick() {
    session_.pump();                                                                   // sends a coalesced pending retune when the rate limit allows
    if (session_.state() == RadioSession::State::Started && session_.tuner() && !session_.tuner()->armed() && !session_.tuner()->last_error().empty() && last_error_.isEmpty())
        report("Tuning stopped after a mailbox fault: " + QString::fromStdString(session_.tuner()->last_error()));
    QString s;
    switch (session_.state()) {
        case RadioSession::State::Disconnected: s = "Not connected."; break;
        case RadioSession::State::Connected: s = QString("Connected (%1). %2").arg(session_.allow_write() ? "writes allowed" : "read-only", "Radio not started."); break;
        case RadioSession::State::Started: {
            s = "RADIO STARTED (receive only)."; if (auto t = session_.tuner()) { const auto c = t->counters(); s += QString(" Tuner: %1 write(s), %2 coalesced, %3 rejected%4.").arg(c.sent).arg(c.coalesced).arg(c.rejected).arg(t->armed() ? "" : "; DISARMED after a fault"); }
            break; }
    }
    if (!last_error_.isEmpty()) s += "  Last problem: " + last_error_;
    status_->setText(s); updateButtons();
}
void RadioPanel::updateButtons() {
    quick_->setEnabled(session_.state() != RadioSession::State::Started); rx_->setRadioRunning(session_.state() == RadioSession::State::Started);
    { const bool st = session_.state() == RadioSession::State::Started; if (st != was_started_) { was_started_ = st; emit runningChanged(st); } }
    const auto st = session_.state(); const bool disc = st == RadioSession::State::Disconnected, conn = st == RadioSession::State::Connected, started = st == RadioSession::State::Started;
    connect_->setEnabled(disc); identify_->setEnabled(!disc); start_->setEnabled(conn && session_.allow_write()); stop_->setEnabled(started || (conn && session_.allow_write()));
    disconnect_->setEnabled(!disc); device_->setEnabled(disc); order_->setEnabled(disc); allow_->setEnabled(disc); ant_->setEnabled(!started);
}
