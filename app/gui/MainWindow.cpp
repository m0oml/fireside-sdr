#include "MainWindow.h"
#include "PskPanel.h"
#include "CatPanel.h"
#include "fireside/tx_meter.h"
#include <QLabel>
#include <QTimer>
#include <QGridLayout>
#include <QButtonGroup>
#include "AppLog.h"
#include <QDateTime>
#include <QFile>
#include <QDir>
#include <QStandardPaths>
#include "QrzClient.h"
#include "PowerMeter.h"
#include "AudioPanel.h"
#include <QApplication>
#include <QComboBox>
#include <QHBoxLayout>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QVBoxLayout>
#include <QTabWidget>
#include <QCloseEvent>
#include <QSettings>
#include <algorithm>
#include "Theme.h"
#include <QMessageBox>
#include "LogPanel.h"
#include "TxPanel.h"
#include "TransmitCard.h"
#include "AntennaPanel.h"
#include "RadioPanel.h"
#include "ReceiverPanel.h"
#include "fireside/cli.h"
#include "fireside/identify.h"
#include "fireside/link.h"

namespace {
struct SimHandle : fireside::BusHandle { fireside::sim::DiceBus& b; explicit SimHandle(fireside::sim::DiceBus& x) : b(x) {} fireside::IBus& bus() override { return b; } bool is_local() const override { return false; } };
}  // namespace
using namespace fireside;

MainWindow::MainWindow(bool enableAudio, bool threaded, bool simulate, QWidget* parent) : QMainWindow(parent) {
    setWindowTitle("Fireside SDR");
    auto* central = new QWidget; central->setObjectName("root"); auto* col = new QVBoxLayout(central); col->setContentsMargins(12, 12, 12, 12); col->setSpacing(12); auto* row = new QHBoxLayout;
    devices_ = new QComboBox; devices_->setMinimumWidth(360);
    auto* refresh = new QPushButton("Refresh"); auto* ident = new QPushButton("Identify (read-only)");
    row->addWidget(devices_, 1); row->addWidget(refresh); row->addWidget(ident);
    log_ = new QPlainTextEdit; log_->setReadOnly(true);
    log_->setMaximumBlockCount(2000);
    RadioSession::Opener opener;
    if (simulate) {
        sim_ = std::make_unique<fireside::sim::DiceBus>(); sim_radio_ = std::make_unique<fireside::sim::RadioModel>(*sim_);   // tuner, PA detector, EEPROM: practise transmit with nothing on the air
        opener = [this](const std::string&) { return std::unique_ptr<BusHandle>(new SimHandle(*sim_)); };
    }
    else opener = fireside::real_env().open_bus;
    receiver_ = new ReceiverPanel(enableAudio, threaded);
    radio_ = new RadioPanel(receiver_, opener, simulate);
    connect(radio_, &RadioPanel::message, this, [this](const QString& m) { log_->appendPlainText(m); appLog("radio", m); });
    connect(radio_, &RadioPanel::radioStarted, this, [this] { tabs_->setCurrentWidget(receiver_); });
    antennas_ = new AntennaPanel;                                                            // which RX antenna on which band: switches while the radio runs
    radio_->setAntennaProvider([this](double hz) { return antennas_->antennaFor(hz).toStdString(); });
    connect(receiver_, &ReceiverPanel::dialChanged, antennas_, &AntennaPanel::onDial);
    connect(antennas_, &AntennaPanel::antennaWanted, radio_, &RadioPanel::requestRxAntenna);
    connect(radio_, &RadioPanel::runningChanged, antennas_, &AntennaPanel::setRadioRunning);
    connect(radio_, &RadioPanel::rxAntennaChanged, this, [this](const QString& a) { antennas_->setActiveAntenna(a); receiver_->setRxAntennaText(a); });
    antennas_->onDial(receiver_->dialHz());
    connect(receiver_, &ReceiverPanel::rateRequested, this, [this](unsigned rate) { radio_->startAtRate(rate, false); });
    connect(receiver_, &ReceiverPanel::stopRadioRequested, this, [this] { radio_->stopRadio(); });
    auto* logbox = new QVBoxLayout; logbox->addWidget(log_);
    col->addWidget(radio_);
    if (QWidget* adv = receiver_->takeAdvancedWidget()) { auto* al = new QVBoxLayout; al->addWidget(adv); col->addWidget(theme::makeCard("Receive I/Q (advanced)", al)); }
    device_col_ = col; col->addWidget(theme::makeCard("MIDI (read-only identify)", row)); col->addWidget(theme::makeCard("Log", logbox, true), 1);
    tabs_ = new QTabWidget;
    tabs_->addTab(receiver_, "Radio"); tabs_->addTab(antennas_, "Antennas"); tabs_->addTab(central, "Device"); log_panel_ = new LogPanel(receiver_); receiver_->setLookupWidget(log_panel_->takeLookupWidget()); receiver_->setLogFormWidget(log_panel_->takeFormWidget()); connect(log_panel_, &LogPanel::activeChanged, receiver_, &ReceiverPanel::setLogActive); tabs_->addTab(log_panel_, "Log"); audio_panel_ = new AudioPanel(radio_); audio_panel_->setReceiveOutputWidget(receiver_->takeOutputWidget()); tabs_->addTab(audio_panel_, "Audio"); psk_panel_ = new PskPanel([this] { return receiver_->myCall(); }, [this] { const QString g = log_panel_->myGrid().trimmed(); return g.size() >= 4 || !tx_card_ ? g : tx_card_->myGrid4(); },
        [this] { return QString(fireside::txm::band_name(fireside::txm::band_of(receiver_->dialHz()))); });
    tabs_->addTab(psk_panel_, "PSK Reporter"); connect(psk_panel_, &PskPanel::shortSummaryChanged, receiver_, &ReceiverPanel::setHeardNote); connect(receiver_, &ReceiverPanel::ftxLine, psk_panel_, &PskPanel::onDecodeLine); connect(audio_panel_, &AudioPanel::message, this, [this](const QString& m) { log_->appendPlainText(m); appLog("audio", m); });
    tx_panel_ = new TxPanel;                                                                 // transmit lives on the Receiver screen, under the decoder (a transceiver is one display)
    tx_card_ = new TransmitCard(radio_, receiver_, simulate);                                // the radio's transmit (ATU, power, SWR): through the session only
    if (sim_radio_) tx_card_->setSimulatedRadio(sim_radio_.get());
    connect(tx_card_, &TransmitCard::message, this, [this](const QString& m) {
        log_->appendPlainText(m); appLog("transmit", m);
        if (qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) return;                          // also ~/.local/share/fireside/transmit.log (UTC time stamp), to read back later
        const QString dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation) + "/fireside"; QDir().mkpath(dir);
        QFile f(dir + "/transmit.log"); if (f.open(QIODevice::Append | QIODevice::Text)) f.write((QDateTime::currentDateTimeUtc().toString("yyyy-MM-dd HH:mm:ss.zzz ") + m + "\n").toUtf8()); });
    tx_card_->setParent(receiver_); tx_card_->hide();                                       // only the owner of the pieces below
    receiver_->setTransmitParts(tx_card_->digitalPart(), tx_card_->voicePart());              // the message controls beside the decoder (Digital), the voice controls under the waterfall (Voice)
    receiver_->setTransmitWidget(tx_panel_);                                                 // the offline TX lab: under the decoder, behind its button
    connect(tx_panel_, &TxPanel::previewRequested, this, [this](const QString& wav, int decoder, int tone) { if (receiver_->loadFile(wav)) { receiver_->setDecoderIndex(decoder); if (tone > 0) receiver_->setDecoderTone(tone); showReceiver(); } });   // look at the rendered file on the waterfall: the receiver only reads it
    connect(receiver_, &ReceiverPanel::decodedCall, log_panel_, &LogPanel::checkWorked); connect(log_panel_, &LogPanel::workedKnown, receiver_, &ReceiverPanel::markWorked);
    connect(receiver_, &ReceiverPanel::callerDecoded, log_panel_, &LogPanel::callerDecoded);
    if (tx_card_) {   // click to reply + auto sequence + auto log
        tx_card_->setMyGridSource([this] { return log_panel_->myGrid(); });
        connect(receiver_, &ReceiverPanel::decodedLineActivated, tx_card_, &TransmitCard::replyTo);
        connect(receiver_, &ReceiverPanel::callerDecoded, tx_card_, &TransmitCard::heardLine);
        connect(tx_card_, &TransmitCard::logQso, log_panel_, &LogPanel::logAuto);
        connect(tx_card_, &TransmitCard::qsoPartner, receiver_, &ReceiverPanel::setQsoPartner);
        connect(tx_card_, &TransmitCard::qsoEnded, receiver_, &ReceiverPanel::endQso);
        connect(tx_card_, &TransmitCard::txSent, receiver_, &ReceiverPanel::addQsoTx);
        receiver_->setTransmitMeters(tx_card_->powerMeter(), tx_card_->statusLabel(), tx_card_->armButton()); receiver_->setTxLamp(tx_card_->lampLabel()); audio_panel_->addKeyingWidget(tx_card_->otherProgramsPart());   // the power meter joins the signal meter, the TX light by the frequency, the CAT tick on the Audio tab
        {   // antenna selector (main screen), as buttons: RX ANT1..3 / RX1 IN, TX ANT1..3 for the band the dial is in (the Antennas table remembers them)
            auto* w = new QWidget; auto* g = new QGridLayout(w); g->setContentsMargins(0, 0, 0, 0); g->setHorizontalSpacing(4); g->setVerticalSpacing(4);
            auto* rxg = new QButtonGroup(w); auto* txg = new QButtonGroup(w); rxg->setExclusive(true); txg->setExclusive(true);
            auto* rl = new QLabel("RX"); rl->setObjectName("muted"); auto* tl = new QLabel("TX"); tl->setObjectName("muted"); g->addWidget(rl, 0, 0); g->addWidget(tl, 1, 0);
            const char* rx_names[4] = {"ANT1", "ANT2", "ANT3", "RX1IN"}; const char* rx_text[4] = {"ANT1", "ANT2", "ANT3", "RX1 IN"};
            for (int i = 0; i < 4; ++i) { auto* b = new QPushButton(rx_text[i]); b->setCheckable(true); b->setProperty("ant", rx_names[i]); b->setCursor(Qt::PointingHandCursor);
                b->setToolTip(i == 3 ? "Receive on the RX1 IN socket (receive only), for this band" : QString("Receive on %1 for this band").arg(rx_text[i])); rxg->addButton(b, i); g->addWidget(b, 0, i + 1); }
            for (int i = 0; i < 3; ++i) { auto* b = new QPushButton(QString("ANT%1").arg(i + 1)); b->setCheckable(true); b->setCursor(Qt::PointingHandCursor);
                b->setToolTip(i == 0 ? "Transmit on ANT1, through the ATU, for this band" : QString("Transmit on ANT%1 for this band: NOT through the ATU (Tune is refused)").arg(i + 1)); txg->addButton(b, i + 1); g->addWidget(b, 1, i + 1); }
            auto* note = new QLabel(" "); note->setObjectName("muted"); g->addWidget(note, 1, 4, 1, 2);
            g->addWidget(tx_card_->atuPart(), 2, 0, 1, 6);   // Tune ATU and the Tuner settings button (the rest is in a window)   // the tuner lives with the antennas
            receiver_->setAntennaWidget(w);
            tx_card_->setTxAntennaSource([this](double hz) { return antennas_->txAntennaFor(hz); });
            connect(rxg, &QButtonGroup::idClicked, this, [this, rxg](int id) { antennas_->setAntennaForDial(receiver_->dialHz(), rxg->button(id)->property("ant").toString()); });
            connect(txg, &QButtonGroup::idClicked, this, [this](int id) { antennas_->setTxAntennaForDial(receiver_->dialHz(), id); });
            auto redraw = [this, rxg, txg, note] {
                const QString want = antennas_->antennaFor(receiver_->dialHz());
                for (auto* b : rxg->buttons()) { QSignalBlocker bl(b); b->setChecked(b->property("ant").toString() == want); }
                if (auto* t = txg->button(antennas_->txAntennaFor(receiver_->dialHz()))) { QSignalBlocker bl(t); t->setChecked(true); }
                const QString a = antennas_->activeAntenna(); const int tx = antennas_->txAntennaFor(receiver_->dialHz());
                note->setText(tx == 1 ? "via the ATU" : "no ATU"); note->setToolTip(QString("%1%2").arg(tx == 1 ? "TX through the ATU" : "TX without the ATU").arg(a.isEmpty() ? QString() : "   RX now: " + (a == "RX1IN" ? QString("RX1 IN") : a)));
            };
            connect(antennas_, &AntennaPanel::choiceChanged, this, redraw); connect(antennas_, &AntennaPanel::choiceChanged, tx_card_, [this] { tx_card_->refreshButtons(); });
            connect(receiver_, &ReceiverPanel::dialChanged, this, redraw); redraw();
        }
        tx_card_->setAudioPanel(audio_panel_);
        radio_->setBeforeStream([this](std::function<void()> go) { tx_card_->armThen(std::move(go)); });   // start-up: EEPROM check, then receive
        log_panel_->setTxPowerSource([this] { return tx_card_->recentTxPowerW(); });   // TX_PWR in the log: what our last over really put out
    }
    {   // CAT for other programs (Hamlib NET rigctl on 127.0.0.1): dial and mode through the receiver's own tuning path; PTT requests go to the Transmit card, which decides
        cat_ = new CatPanel(receiver_, [this] { return tx_card_ && tx_card_->keyedNow(); }, [this] { return tx_card_ && (tx_card_->busy() || tx_card_->sendQueued()); });
        auto* cl = new QVBoxLayout; cl->addWidget(cat_); device_col_->insertWidget(device_col_->count() - 2, theme::makeCard("CAT (other programs)", cl));
        if (tx_card_) cat_->setPttRequest(tx_card_->pttRequest());              // the only way a CAT client can ask to key: the Transmit card decides
    }
    {   // other programs: receive audio to the virtual device, and the "External program" switch
        if (audio_panel_) audio_panel_->setReceiver(receiver_);
        connect(receiver_->externalBox(), &QCheckBox::toggled, this, [this](bool on) { applyExternal(on); });
        if (!qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) {
            QSettings q("fireside", "external");
            if (q.value("on", false).toBool()) {
                ext_prev_decoder_ = q.value("prev_decoder", 0).toInt(); ext_prev_seq_ = q.value("prev_autoseq", true).toBool();
                ext_prev_upload_ = q.value("prev_upload", false).toBool(); ext_prev_data_ = q.value("prev_data_out", false).toBool();
                applyExternal(true, true);
            }
        }
    }
    // QRZ.com: callers first, then (if ticked on the Log tab) every decoded station; the answer goes after their decoder lines
    connect(receiver_, &ReceiverPanel::decodedCall, log_panel_->qrz(), [this](const QString& c) { log_panel_->qrz()->wantCached(c); });      // everyone: from the cache only
    connect(receiver_, &ReceiverPanel::cqDecoded, log_panel_->qrz(), [this](const QString& c) { log_panel_->qrz()->want(c, false); });     // calling CQ: looked up (callers to me: below, first)
    connect(receiver_, &ReceiverPanel::callerDecoded, log_panel_->qrz(), [this](const QString& line) {
        const auto m = fireside::logbook::parse_ftx_message(fireside::logbook::ftx_line_text(line.toStdString())); if (m.ok) log_panel_->qrz()->want(QString::fromStdString(m.from), true); });
    connect(log_panel_->qrz(), &QrzClient::infoReady, receiver_, [this](const QString& call, const fireside::qrz::Info& i) {
        receiver_->showCallInfo(call, QString::fromStdString(fireside::qrz::short_text(i, log_panel_->myGrid().toStdString(), receiver_->heardGrid(call).toStdString()))); /* distance from the grid they sent */ });   // someone answering me: highlighted, filled in and looked up
    connect(receiver_, &ReceiverPanel::decodedLineActivated, this, [this](const QString& line) { log_panel_->fillFromDecode(line); });   // the quick entry sits on the Receiver tab: nothing to switch to
    {   // the red box for safety stops, above every tab: persists 5 s (a new stop restarts it)
        auto* wrap = new QWidget; auto* vl = new QVBoxLayout(wrap); vl->setContentsMargins(0, 0, 0, 0); vl->setSpacing(0);
        trip_ = new QLabel; trip_->setVisible(false); trip_->setAlignment(Qt::AlignCenter); trip_->setWordWrap(true); trip_->setObjectName("tripbox");
        trip_->setStyleSheet("QLabel#tripbox { background: #b71c1c; color: #ffffff; font-size: 16pt; font-weight: bold; padding: 10px 16px; border: 2px solid #ff8a80; }");
        trip_->setTextInteractionFlags(Qt::NoTextInteraction);
        trip_timer_ = new QTimer(this); trip_timer_->setSingleShot(true); connect(trip_timer_, &QTimer::timeout, this, [this] { trip_->setVisible(false); });
        vl->addWidget(trip_); vl->addWidget(tabs_, 1); setCentralWidget(wrap);
        if (tx_card_) connect(tx_card_, &TransmitCard::safetyTrip, this, &MainWindow::showTrip);
    }
    resize(1400, 860);
    if (!qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) { QSettings q("fireside", "window"); if (q.contains("geometry")) restoreGeometry(q.value("geometry").toByteArray()); tabs_->setCurrentIndex(std::clamp(q.value("tab", 0).toInt(), 0, 1)); }
    connect(refresh, &QPushButton::clicked, this, &MainWindow::refreshDevices);
    connect(ident, &QPushButton::clicked, this, &MainWindow::identify);
    log_->appendPlainText(QString("fireside_core: %1 opcodes. The MIDI Identify button only reads. The FireWire session above writes only after you tick \"Allow writes\", connect and confirm Start; the transmitter is never keyed.").arg(unsigned(fireside::kOpCount)));
    refreshDevices();
}
void MainWindow::refreshDevices() {
    devices_->clear();
    for (const auto& d : fireside::list_midi_devices()) devices_->addItem(QString::fromStdString(d.card + "   " + d.path), QString::fromStdString(d.path));
    if (!devices_->count()) log_->appendPlainText("No ALSA rawmidi devices found (radio off, not bound to snd-dice, or no FireWire card yet).");
}
void MainWindow::showTrip(const QString& reason) {
    trip_->setText("TRANSMITTER STOPPED: " + reason); trip_->setVisible(true); trip_->raise(); trip_timer_->start(5000);
    appLog("transmit", "SAFETY STOP shown: " + reason);
}
MainWindow::~MainWindow() { if (cat_) cat_->setPttRequest(nullptr); }   // the Transmit card may go first: CAT must not reach it then

void MainWindow::applyExternal(bool on, bool restoring) {
    QCheckBox* seq = tx_card_ ? tx_card_->autoSeqBox() : nullptr; QCheckBox* up = psk_panel_ ? psk_panel_->uploadBox() : nullptr;
    QCheckBox* data = audio_panel_ ? audio_panel_->dataOutBox() : nullptr;
    if (on) {
        if (!restoring) { ext_prev_decoder_ = receiver_->decoderIndex(); ext_prev_seq_ = seq && seq->isChecked(); ext_prev_upload_ = up && up->isChecked(); ext_prev_data_ = data && data->isChecked(); }
        receiver_->setExternalMode(true);
        if (seq) { seq->setChecked(false); seq->setEnabled(false); }
        if (up) { up->setChecked(false); up->setEnabled(false); }
        if (data) data->setChecked(true);
        log_->appendPlainText("External program: our decoder, auto sequence + log and PSK Reporter uploads are off; receive audio goes to \"Fireside RX audio\".");
    } else {
        receiver_->setExternalMode(false);
        if (ext_prev_decoder_ > 0) receiver_->setDecoderIndex(ext_prev_decoder_);
        if (seq) { seq->setEnabled(true); seq->setChecked(ext_prev_seq_); }
        if (up) { up->setEnabled(true); up->setChecked(ext_prev_upload_); }
        if (data) data->setChecked(ext_prev_data_);
        log_->appendPlainText("External program off: decoder, auto sequence + log and PSK Reporter uploads are back as they were.");
    }
    if (qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) return;
    QSettings q("fireside", "external"); q.setValue("on", on); q.setValue("prev_decoder", ext_prev_decoder_); q.setValue("prev_autoseq", ext_prev_seq_);
    q.setValue("prev_upload", ext_prev_upload_); q.setValue("prev_data_out", ext_prev_data_);
}
void MainWindow::closeEvent(QCloseEvent* e) {
    if (!qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) { QSettings q("fireside", "window"); q.setValue("geometry", saveGeometry()); q.setValue("tab", tabs_->currentIndex()); }
    if (log_panel_) log_panel_->saveSettings();
    if (receiver_) receiver_->saveSettings();
    if (psk_panel_) psk_panel_->flushUploads();                                        // what is waiting goes now (as WSJT-X does on exit)                                          // now, while the Device tab's IF offset / Loop file still exist
    if (radio_ && !radio_->shutdownOnExit())                                           // a started radio is shut down (reviewed sequence) before we exit
        QMessageBox::warning(this, "Radio shutdown failed", "The radio did not confirm the shutdown (MOX off, standby, PA bias off). Check it, and power-cycle it if anything looks wrong.");
    QMainWindow::closeEvent(e);
}
void MainWindow::showRadio() { tabs_->setCurrentIndex(2); }                     // the Device tab (FireWire, session)
void MainWindow::showReceiver() { tabs_->setCurrentWidget(receiver_); }
QString MainWindow::deviceSummary() const { return QString("%1 MIDI device(s)").arg(devices_->count()); }

void MainWindow::identify() {
    if (devices_->currentIndex() < 0) { log_->appendPlainText("Select a device first."); return; }
    const std::string path = devices_->currentData().toString().toStdString();
    QApplication::setOverrideCursor(Qt::WaitCursor);
    try {
        fireside::RawMidi midi(path); fireside::MidiLink link(midi); fireside::Flex fx(link);          // default Safety: read-only
        fireside::IdentifyOptions o; o.boot_wait_s = 3.0;
        log_->appendPlainText(QString::fromStdString(fireside::identify(fx, o).describe()));
    } catch (const std::exception& e) { log_->appendPlainText(QString("Identify failed: ") + e.what()); }
    QApplication::restoreOverrideCursor();
}
