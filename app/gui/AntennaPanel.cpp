#include "AntennaPanel.h"
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QSettings>
#include <QVBoxLayout>
#include "Theme.h"
#include "fireside/rxant.h"

namespace {
const char* const kNames[] = {"ANT1", "ANT2", "ANT3", "RX1IN"};                                // wire order: 1..4
QString shown(const QString& a) { return a == "RX1IN" ? QString("RX1 IN") : a; }
}

AntennaPanel::AntennaPanel(QWidget* parent) : QWidget(parent) {
    auto* col = new QVBoxLayout(this); col->setContentsMargins(0, 0, 0, 0); col->setSpacing(12);
    size_t n = 0; const fireside::RxBand* bands = fireside::rx_bands(n);

    auto* top = new QGridLayout; top->setHorizontalSpacing(12); top->setVerticalSpacing(8);
    auto_ = new QCheckBox("Switch the receive antenna automatically when the dial changes band"); auto_->setChecked(true);
    auto_->setToolTip("On: moving the dial into another band switches the radio's RX antenna to that band's choice below (a relay click). Off: the antenna chosen at start-up stays.");
    default_ = new QComboBox; for (const char* a : kNames) default_->addItem(shown(a), a); default_->setToolTip("Used for bands set to \"Default\" and for every frequency outside the listed bands (broadcast, 4 m etc.)");
    auto* dl = new QLabel("Default antenna"); dl->setObjectName("dim");
    top->addWidget(auto_, 0, 0, 1, 3); top->addWidget(dl, 1, 0); top->addWidget(default_, 1, 1); top->setColumnStretch(2, 1);
    tx_default_ = new QComboBox; for (int a = 1; a <= 3; ++a) tx_default_->addItem(QString("ANT%1%2").arg(a).arg(a == 1 ? " (ATU)" : ""), a);
    tx_default_->setToolTip("Transmit antenna for bands set to \"Default\" and outside the listed bands. Only ANT1 goes through the ATU.");
    { auto* tl = new QLabel("Default transmit antenna"); top->addWidget(tl, 3, 0); top->addWidget(tx_default_, 3, 1); }
    status_ = new QLabel; status_->setObjectName("muted"); status_->setWordWrap(true); top->addWidget(status_, 2, 0, 1, 3);
    col->addWidget(theme::makeCard("Antennas by band", top));

    auto* grid = new QGridLayout; grid->setHorizontalSpacing(16); grid->setVerticalSpacing(6);
    auto* h1 = new QLabel("Band"), *h2 = new QLabel("Range"), *h3 = new QLabel("Receive antenna"), *h4 = new QLabel("Transmit antenna"); for (auto* h : {h1, h2, h3, h4}) h->setObjectName("dim");
    grid->addWidget(h1, 0, 0); grid->addWidget(h2, 0, 1); grid->addWidget(h3, 0, 2); grid->addWidget(h4, 0, 3);
    for (size_t i = 0; i < n; ++i) {
        auto* name = new QLabel(bands[i].name); auto* range = new QLabel(QString("%1 - %2 MHz").arg(bands[i].lo_hz / 1e6, 0, 'f', 3).arg(bands[i].hi_hz / 1e6, 0, 'f', 3)); range->setObjectName("muted");
        auto* box = new QComboBox; box->addItem("Default", QString()); for (const char* a : kNames) box->addItem(shown(a), a); box->setMinimumWidth(140);
        connect(box, &QComboBox::currentIndexChanged, this, [this] { changed(); });
        auto* tbox = new QComboBox; tbox->addItem("Default", 0); for (int a = 1; a <= 3; ++a) tbox->addItem(QString("ANT%1%2").arg(a).arg(a == 1 ? " (ATU)" : ""), a); tbox->setMinimumWidth(130);
        connect(tbox, &QComboBox::currentIndexChanged, this, [this] { changed(); });
        grid->addWidget(name, int(i) + 1, 0); grid->addWidget(range, int(i) + 1, 1); grid->addWidget(box, int(i) + 1, 2); grid->addWidget(tbox, int(i) + 1, 3);
        band_label_.push_back(name); band_box_.push_back(box); tx_box_.push_back(tbox);
    }
    grid->setColumnStretch(4, 1);
    auto* note = new QLabel("The radio's band-pass filters follow the frequency by themselves. The choice is applied only while the radio is running; at start-up the antenna for the current band is used. "
                            "RX1 IN is the separate receive-only socket. The transmit antenna is set at the start of each over; only ANT1 goes through the ATU "
                            "(Tune and Restore ATU are refused on a band set to ANT2 / ANT3). Nothing here transmits.");
    note->setObjectName("muted"); note->setWordWrap(true);
    auto* gbox = new QVBoxLayout; gbox->addLayout(grid); gbox->addWidget(note);
    col->addWidget(theme::makeCard("Bands", gbox)); col->addStretch(1);
    connect(auto_, &QCheckBox::toggled, this, [this] { changed(); });
    connect(default_, &QComboBox::currentIndexChanged, this, [this] { changed(); });
    connect(tx_default_, &QComboBox::currentIndexChanged, this, [this] { changed(); });
    load(); updateStatus();
}

QString AntennaPanel::antennaFor(double hz) const {
    const int b = fireside::rx_band_index(hz);
    if (b >= 0) { const QString a = band_box_[size_t(b)]->currentData().toString(); if (!a.isEmpty()) return a; }
    return default_->currentData().toString();
}
int AntennaPanel::txAntennaFor(double hz) const {
    const int b = fireside::rx_band_index(hz);
    if (b >= 0) { const int a = tx_box_[size_t(b)]->currentData().toInt(); if (a >= 1 && a <= 3) return a; }
    const int d = tx_default_->currentData().toInt(); return d >= 1 && d <= 3 ? d : 1;
}
void AntennaPanel::setTxAntennaForDial(double hz, int ant) {
    if (ant < 1 || ant > 3) return;
    const int b = fireside::rx_band_index(hz); QComboBox* box = b >= 0 ? tx_box_[size_t(b)] : tx_default_;
    const int i = box->findData(ant); if (i >= 0) box->setCurrentIndex(i);                    // -> changed(): saved, the selector redraws
}
QString AntennaPanel::bandNameFor(double hz) const { const int b = fireside::rx_band_index(hz); size_t n = 0; const fireside::RxBand* bands = fireside::rx_bands(n); return b >= 0 ? QString(bands[b].name) : QString("other frequencies"); }
bool AntennaPanel::autoSwitch() const { return auto_->isChecked(); }

void AntennaPanel::onDial(double hz) { dial_hz_ = hz; evaluate(false); }
void AntennaPanel::changed() {
    emit choiceChanged(); save(); evaluate(true); }
void AntennaPanel::evaluate(bool force) {
    const QString want = antennaFor(dial_hz_);
    if (auto_->isChecked() && (force || want != last_wanted_)) emit antennaWanted(want, bandNameFor(dial_hz_));
    last_wanted_ = want; updateStatus();
}
void AntennaPanel::setRadioRunning(bool on) { running_ = on; if (!on) active_.clear(); updateStatus(); }
void AntennaPanel::setActiveAntenna(const QString& name) { active_ = name; updateStatus(); emit choiceChanged(); }
void AntennaPanel::setAntennaForDial(double hz, const QString& ant) {
    const int b = fireside::rx_band_index(hz); QComboBox* box = b >= 0 ? band_box_[size_t(b)] : default_;
    const int i = box->findData(ant); if (i < 0) return;
    { QSignalBlocker blk(box); box->setCurrentIndex(i); }
    save(); updateStatus(); last_wanted_ = ant; emit antennaWanted(ant, bandNameFor(hz)); emit choiceChanged();   // an explicit choice switches even with automatic switching off
}
void AntennaPanel::updateStatus() {
    const QString want = antennaFor(dial_hz_), band = bandNameFor(dial_hz_);
    QString s = QString("Dial in %1: %2 is chosen").arg(band, shown(want));
    if (!running_) s += ". The radio is not running: the antenna is set when it starts.";
    else if (!active_.isEmpty()) s += active_ == want ? QString(". The radio is using it.") : QString(". The radio is using %1%2.").arg(shown(active_), auto_->isChecked() ? "" : " (automatic switching is off)");
    status_->setText(s);
}
void AntennaPanel::save() const {
    if (qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) return;
    QSettings q("fireside", "antennas"); q.setValue("auto", auto_->isChecked()); q.setValue("default", default_->currentData().toString());
    size_t n = 0; const fireside::RxBand* bands = fireside::rx_bands(n); for (size_t i = 0; i < n; ++i) { q.setValue(QString("band/") + bands[i].id, band_box_[i]->currentData().toString()); q.setValue(QString("tx/") + bands[i].id, tx_box_[i]->currentData().toInt()); }
    q.setValue("tx_default", tx_default_->currentData().toInt());
}
void AntennaPanel::load() {
    if (qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) return;
    QSettings q("fireside", "antennas"); QSignalBlocker a(auto_), d(default_); auto_->setChecked(q.value("auto", true).toBool());
    auto pick = [](QComboBox* box, const QString& v) { const int i = box->findData(v); box->setCurrentIndex(i >= 0 ? i : 0); };
    if (q.contains("default")) pick(default_, q.value("default").toString());
    else { const int idx = QSettings("fireside", "radio").value("antenna", 0).toInt(); default_->setCurrentIndex(idx >= 0 && idx <= 3 ? idx : 0); }      // first run: the antenna the Radio tab used to remember
    size_t n = 0; const fireside::RxBand* bands = fireside::rx_bands(n);
    for (size_t i = 0; i < n; ++i) { QSignalBlocker b(band_box_[i]); pick(band_box_[i], q.value(QString("band/") + bands[i].id).toString());
                                     QSignalBlocker t(tx_box_[i]); const int ti = tx_box_[i]->findData(q.value(QString("tx/") + bands[i].id, 0).toInt()); tx_box_[i]->setCurrentIndex(ti >= 0 ? ti : 0); }
    { QSignalBlocker t(tx_default_); const int ti = tx_default_->findData(q.value("tx_default", 1).toInt()); tx_default_->setCurrentIndex(ti >= 0 ? ti : 0); }
}
