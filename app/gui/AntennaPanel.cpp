#include "AntennaPanel.h"
#include <QCheckBox>
#include <QComboBox>
#include <QGridLayout>
#include <QLabel>
#include <QSettings>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QVBoxLayout>
#include "Theme.h"
#include "fireside/rxant.h"

namespace {
const char* const kNames[] = {"ANT1", "ANT2", "ANT3", "RX1IN"};                                // wire order: 1..4
QString shown(const QString& a) { return a == "RX1IN" ? QString("RX1 IN") : a; }
const char* const kLineNames[] = {"TX1 (red)", "TX2 (white)", "TX3 (yellow)"};
}

AntennaPanel::AntennaPanel(QWidget* parent) : QWidget(parent) {
    auto* col = new QVBoxLayout(this); col->setContentsMargins(0, 0, 0, 0); col->setSpacing(12);
    size_t n = 0; const fireside::RxBand* bands = fireside::rx_bands(n);

    auto* top = new QGridLayout; top->setHorizontalSpacing(12); top->setVerticalSpacing(8);
    last_rx_.assign(n, QString()); last_tx_.assign(n, 0);
    status_ = new QLabel; status_->setObjectName("muted"); status_->setWordWrap(true); top->addWidget(status_, 0, 0, 1, 3); top->setColumnStretch(2, 1);
    col->addWidget(theme::makeCard("Antennas by band", top));

    auto* lg = new QGridLayout; lg->setHorizontalSpacing(12); lg->setVerticalSpacing(8);
    for (int i = 0; i < fireside::kTxLines; ++i) {
        auto* box = new QComboBox; box->setMinimumWidth(220);
        box->addItem("Unused", int(fireside::LineRole::Unused)); box->addItem("Band switch", int(fireside::LineRole::Band));
        box->addItem("Amplifier keying (not built yet)", int(fireside::LineRole::Amplifier)); box->addItem("Transverter (not built yet)", int(fireside::LineRole::Transverter));
        if (auto* m = qobject_cast<QStandardItemModel*>(box->model())) { m->item(2)->setEnabled(false); m->item(3)->setEnabled(false); }
        box->setToolTip("Band switch: the line is on while the dial is in a band ticked for it in the table below, on receive and transmit alike "
                        "(an external relay, e.g. an 80 m T-mode relay). It never changes during an over, and every over waits for the relay to settle.");
        auto* st = new QLabel; st->setObjectName("muted");
        lg->addWidget(new QLabel(kLineNames[i]), i, 0); lg->addWidget(box, i, 1); lg->addWidget(st, i, 2);
        line_role_[size_t(i)] = box; line_state_lbl_[size_t(i)] = st;
        connect(box, &QComboBox::currentIndexChanged, this, [this] { linesEdited(); });
    }
    settle_ = new QSpinBox; settle_->setRange(10, 1000); settle_->setValue(50); settle_->setSuffix(" ms");
    settle_->setToolTip("After a line changes, nothing keys until this long has passed: the relay's operate and bounce time (no switching with RF on the contacts).");
    connect(settle_, &QSpinBox::valueChanged, this, [this] { linesEdited(); });
    { auto* sl = new QLabel("Relay settle time"); lg->addWidget(sl, fireside::kTxLines, 0); lg->addWidget(settle_, fireside::kTxLines, 1); }
    lg->setColumnStretch(2, 1);
    auto* lnote = new QLabel("The radio's rear RCA outputs. A band-switch line is switched only while the radio is running and never during an over; "
                             "stopping the radio or closing Fireside turns every used line off, so build the external gear with OFF as its safe position.");
    lnote->setObjectName("muted"); lnote->setWordWrap(true);
    auto* lbox = new QVBoxLayout; lbox->addLayout(lg); lbox->addWidget(lnote);
    col->addWidget(theme::makeCard("Rear TX outputs", lbox));

    auto* grid = new QGridLayout; grid->setHorizontalSpacing(16); grid->setVerticalSpacing(6);
    auto* h1 = new QLabel("Band"), *h2 = new QLabel("Range"), *h3 = new QLabel("Receive antenna"), *h4 = new QLabel("Transmit antenna"); for (auto* h : {h1, h2, h3, h4}) h->setObjectName("dim");
    grid->addWidget(h1, 0, 0); grid->addWidget(h2, 0, 1); grid->addWidget(h3, 0, 2); grid->addWidget(h4, 0, 3);
    for (int j = 0; j < fireside::kTxLines; ++j) { auto* h = new QLabel(QString("TX%1").arg(j + 1)); h->setObjectName("dim"); h->setToolTip(QString("Band switch: %1 is on in the ticked bands").arg(kLineNames[j])); grid->addWidget(h, 0, 4 + j, Qt::AlignHCenter); line_hdr_[size_t(j)] = h; }
    for (size_t i = 0; i < n; ++i) {
        auto* name = new QLabel(bands[i].name); auto* range = new QLabel(QString("%1 - %2 MHz").arg(bands[i].lo_hz / 1e6, 0, 'f', 3).arg(bands[i].hi_hz / 1e6, 0, 'f', 3)); range->setObjectName("muted");
        auto* box = new QComboBox; for (const char* a : kNames) box->addItem(shown(a), a); box->addItem("Last", QString("LAST")); box->setMinimumWidth(140);
        box->setToolTip("The receive antenna on this band: switched to every time the dial comes into the band. Last: the antenna last chosen on this band with the main screen's selector.");
        connect(box, &QComboBox::currentIndexChanged, this, [this] { changed(); });
        auto* tbox = new QComboBox; for (int a = 1; a <= 3; ++a) tbox->addItem(QString("ANT%1").arg(a), a); tbox->addItem("Last", 0); tbox->setMinimumWidth(130);
        tbox->setToolTip("The transmit antenna on this band (all three go through the ATU). Last: the one last chosen on this band with the main screen's selector (ANT1 until then).");
        connect(tbox, &QComboBox::currentIndexChanged, this, [this] { changed(); });
        grid->addWidget(name, int(i) + 1, 0); grid->addWidget(range, int(i) + 1, 1); grid->addWidget(box, int(i) + 1, 2); grid->addWidget(tbox, int(i) + 1, 3);
        band_label_.push_back(name); band_box_.push_back(box); tx_box_.push_back(tbox);
        std::array<QCheckBox*, fireside::kTxLines> row{};
        for (int j = 0; j < fireside::kTxLines; ++j) {
            auto* cb = new QCheckBox; cb->setToolTip(QString("%1 on while the dial is in %2").arg(kLineNames[j], bands[i].name));
            connect(cb, &QCheckBox::toggled, this, [this] { linesEdited(); });
            grid->addWidget(cb, int(i) + 1, 4 + j, Qt::AlignHCenter); row[size_t(j)] = cb;
        }
        line_band_.push_back(row);
    }
    grid->setColumnStretch(4 + fireside::kTxLines, 1);
    auto* note = new QLabel("The radio's band-pass filters follow the frequency by themselves. The choice is applied only while the radio is running; at start-up the antenna for the current band is used. "
                            "Outside the listed bands the antenna stays as it is. RX1 IN is the separate receive-only socket. The transmit antenna is set at the start of each over; "
                            "the ATU sits in front of all three jacks and keeps its tune per band and per antenna. Nothing here transmits.");
    note->setObjectName("muted"); note->setWordWrap(true);
    auto* gbox = new QVBoxLayout; gbox->addLayout(grid); gbox->addWidget(note);
    col->addWidget(theme::makeCard("Bands", gbox)); col->addStretch(1);
    load(); updateStatus(); updateLineColumns(); updateLineStatus();
}

fireside::LineConfig AntennaPanel::lineConfig() const {
    fireside::LineConfig c; c.settle_s = settle_->value() / 1000.0;
    for (int j = 0; j < fireside::kTxLines; ++j) {
        c.role[size_t(j)] = fireside::LineRole(line_role_[size_t(j)]->currentData().toInt());
        for (size_t i = 0; i < line_band_.size() && i < 32; ++i) if (line_band_[i][size_t(j)]->isChecked()) c.band_mask[size_t(j)] |= 1u << i;
    }
    return c;
}
void AntennaPanel::linesEdited() { save(); updateLineColumns(); updateLineStatus(); emit linesChanged(); }
void AntennaPanel::updateLineColumns() {
    for (int j = 0; j < fireside::kTxLines; ++j) {
        const bool band = fireside::LineRole(line_role_[size_t(j)]->currentData().toInt()) == fireside::LineRole::Band;
        line_hdr_[size_t(j)]->setVisible(band); for (auto& row : line_band_) row[size_t(j)]->setVisible(band);
    }
}
void AntennaPanel::setLineStates(int a, int b, int c) { line_state_ = {{a, b, c}}; updateLineStatus(); }
QString AntennaPanel::lineStatusText() const { QStringList l; for (auto* x : line_state_lbl_) l << x->text(); return l.join(" | "); }
void AntennaPanel::updateLineStatus() {
    for (int j = 0; j < fireside::kTxLines; ++j) {
        const bool band = fireside::LineRole(line_role_[size_t(j)]->currentData().toInt()) == fireside::LineRole::Band;
        QString t;
        if (band) {
            QStringList on; for (size_t i = 0; i < line_band_.size(); ++i) if (line_band_[i][size_t(j)]->isChecked()) on << band_label_[i]->text();
            t = on.isEmpty() ? QString("no band ticked: stays off") : QString("on in ") + on.join(", ");
            const int s = line_state_[size_t(j)]; t += !running_ ? QString(" (radio not running)") : s == 1 ? QString(": ON now") : s == 0 ? QString(": off now") : QString();
        }
        line_state_lbl_[size_t(j)]->setText(t);
    }
}

QString AntennaPanel::antennaFor(double hz) const {          // the band's fixed antenna; Last: the one last chosen there; with none (or outside the bands): stay as it is
    const int b = fireside::rx_band_index(hz);
    if (b >= 0) { const QString a = band_box_[size_t(b)]->currentData().toString(); if (a != "LAST") return a; if (!last_rx_[size_t(b)].isEmpty()) return last_rx_[size_t(b)]; }
    return active_.isEmpty() ? QString(last_wanted_.isEmpty() ? "ANT1" : last_wanted_) : active_;
}
QString AntennaPanel::lastRxFor(double hz) const { const int b = fireside::rx_band_index(hz); return b >= 0 ? last_rx_[size_t(b)] : QString(); }
int AntennaPanel::txAntennaFor(double hz) const {
    const int b = fireside::rx_band_index(hz);
    if (b >= 0) { const int a = tx_box_[size_t(b)]->currentData().toInt(); if (a >= 1 && a <= 3) return a; const int l = last_tx_[size_t(b)]; return l >= 1 && l <= 3 ? l : 1; }
    return last_tx_other_ >= 1 && last_tx_other_ <= 3 ? last_tx_other_ : 1;
}
void AntennaPanel::setTxAntennaForDial(double hz, int ant) {           // the main screen's selector: a fixed band takes the new antenna; a Last band remembers it
    if (ant < 1 || ant > 3) return;
    const int b = fireside::rx_band_index(hz);
    if (b < 0) { last_tx_other_ = ant; save(); emit choiceChanged(); return; }
    QComboBox* box = tx_box_[size_t(b)];
    if (box->currentData().toInt() == 0) { last_tx_[size_t(b)] = ant; save(); emit choiceChanged(); return; }
    const int i = box->findData(ant); if (i >= 0) box->setCurrentIndex(i);                    // -> changed(): saved, the selector redraws
}
QString AntennaPanel::bandNameFor(double hz) const { const int b = fireside::rx_band_index(hz); size_t n = 0; const fireside::RxBand* bands = fireside::rx_bands(n); return b >= 0 ? QString(bands[b].name) : QString("other frequencies"); }

void AntennaPanel::onDial(double hz) { dial_hz_ = hz; evaluate(false); }
void AntennaPanel::changed() {
    emit choiceChanged(); save(); evaluate(true); }
void AntennaPanel::evaluate(bool force) {
    const QString want = antennaFor(dial_hz_);
    if (force || want != last_wanted_) emit antennaWanted(want, bandNameFor(dial_hz_));
    last_wanted_ = want; updateStatus();
}
void AntennaPanel::setRadioRunning(bool on) { running_ = on; if (!on) active_.clear(); updateStatus(); updateLineStatus(); }
void AntennaPanel::setActiveAntenna(const QString& name) { active_ = name; updateStatus(); emit choiceChanged(); }
void AntennaPanel::setAntennaForDial(double hz, const QString& ant) {   // the main screen's selector: switches now; a fixed band takes it, a Last band remembers it
    if (QString(ant) != "ANT1" && ant != "ANT2" && ant != "ANT3" && ant != "RX1IN") return;
    const int b = fireside::rx_band_index(hz);
    if (b >= 0) {
        QComboBox* box = band_box_[size_t(b)];
        if (box->currentData().toString() == "LAST") last_rx_[size_t(b)] = ant;
        else { const int i = box->findData(ant); if (i < 0) return; QSignalBlocker blk(box); box->setCurrentIndex(i); }
    }
    save(); last_wanted_ = ant; updateStatus(); emit antennaWanted(ant, bandNameFor(hz)); emit choiceChanged();
}
void AntennaPanel::updateStatus() {
    const QString want = antennaFor(dial_hz_), band = bandNameFor(dial_hz_);
    QString s = QString("Dial in %1: %2 is chosen").arg(band, shown(want));
    if (!running_) s += ". The radio is not running: the antenna is set when it starts.";
    else if (!active_.isEmpty()) s += active_ == want ? QString(". The radio is using it.") : QString(". The radio is using %1.").arg(shown(active_));
    status_->setText(s);
}
void AntennaPanel::save() const {
    if (qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) return;
    QSettings q("fireside", "antennas"); q.setValue("version", 2);
    size_t n = 0; const fireside::RxBand* bands = fireside::rx_bands(n);
    for (size_t i = 0; i < n; ++i) { q.setValue(QString("band/") + bands[i].id, band_box_[i]->currentData().toString()); q.setValue(QString("tx/") + bands[i].id, tx_box_[i]->currentData().toInt());
                                     q.setValue(QString("last_rx/") + bands[i].id, last_rx_[i]); q.setValue(QString("last_tx/") + bands[i].id, last_tx_[i]); }
    q.setValue("last_tx_other", last_tx_other_);
    for (int j = 0; j < fireside::kTxLines; ++j) {
        q.setValue(QString("lines/role%1").arg(j + 1), QString(fireside::line_role_id(fireside::LineRole(line_role_[size_t(j)]->currentData().toInt()))));
        for (size_t i = 0; i < n; ++i) q.setValue(QString("lines/tx%1/").arg(j + 1) + bands[i].id, line_band_[i][size_t(j)]->isChecked());
    }
    q.setValue("lines/settle_ms", settle_->value());
}
void AntennaPanel::load() {
    if (qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) return;
    QSettings q("fireside", "antennas");
    // Version 1 had a "Default" entry (and a default antenna): such bands become Last, remembering the old default as their last antenna.
    const bool v1 = q.value("version", 1).toInt() < 2;
    const QString old_rx = q.value("default", "ANT1").toString(); const int old_tx = q.value("tx_default", 1).toInt();
    size_t n = 0; const fireside::RxBand* bands = fireside::rx_bands(n);
    for (size_t i = 0; i < n; ++i) {
        QString r = q.value(QString("band/") + bands[i].id, "LAST").toString(); if (r.isEmpty()) r = "LAST";
        last_rx_[i] = q.value(QString("last_rx/") + bands[i].id, v1 ? old_rx : QString()).toString();
        { QSignalBlocker b(band_box_[i]); const int k = band_box_[i]->findData(r); band_box_[i]->setCurrentIndex(k >= 0 ? k : band_box_[i]->findData(QString("LAST"))); }
        const int t = q.value(QString("tx/") + bands[i].id, 0).toInt();
        last_tx_[i] = q.value(QString("last_tx/") + bands[i].id, v1 ? old_tx : 0).toInt();
        { QSignalBlocker b(tx_box_[i]); const int k = tx_box_[i]->findData(t >= 1 && t <= 3 ? t : 0); tx_box_[i]->setCurrentIndex(k >= 0 ? k : 0); }
    }
    last_tx_other_ = q.value("last_tx_other", v1 ? old_tx : 1).toInt();
    for (int j = 0; j < fireside::kTxLines; ++j) {
        QSignalBlocker rb(line_role_[size_t(j)]);
        auto role = fireside::line_role_from_id(q.value(QString("lines/role%1").arg(j + 1), "unused").toString().toStdString());
        if (role != fireside::LineRole::Band) role = fireside::LineRole::Unused;                          // placeholders are not selectable
        line_role_[size_t(j)]->setCurrentIndex(line_role_[size_t(j)]->findData(int(role)));
        for (size_t i = 0; i < n; ++i) { QSignalBlocker cb(line_band_[i][size_t(j)]); line_band_[i][size_t(j)]->setChecked(q.value(QString("lines/tx%1/").arg(j + 1) + bands[i].id, false).toBool()); }
    }
    { QSignalBlocker sb(settle_); settle_->setValue(q.value("lines/settle_ms", 50).toInt()); }
}
