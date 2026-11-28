// Calibrate with a load: one press, the operator only attaches a 50 ohm load (or a power attenuator of 20 dB or more) to one antenna jack.
// For each chosen band, in this order (user 2026-11-28, after the same steps were done by hand into a Bird 20 dB Tenuline on ANT3):
//   1. receive only: the receive antenna is switched to the load's jack, USB 2.4 kHz, the noise floor read with the preamp off and on (the load's
//      thermal noise: about -125 .. -135 dBm with the preamp off). A floor far above that means the jack hears an antenna, not a load: the routine
//      stops before anything is transmitted;
//   2. receive only: the S-meter check with the radio's internal generator (TxControl::sig_gen_check, saved per band as the button does);
//   3. Tune at 3 W on the load's jack (the ATU sits in front of all three jacks). A 50 ohm load needs next to no matching: relays above
//      kLoadMaxRelay mean this is not a load, and the routine stops. A tune that does not pass skips the band's transmit steps;
//   4. Measure drive (the 2 W ramp: the band's drive level, now measured into a true 50 ohm);
//   5. steady test tones at 5 W and 15 W for 3 s each: our PA meter beside the tuner's own (the meter's reading on a good load, per band).
// Transmit goes only through TxControl (tune / ramp) and its TxGate, with their usual limits; the band's Antennas-tab choice is not changed (the
// load's jack is passed per step). Stop, any safety stop or a refusal ends the routine. The results go to QSettings "fireside/loadcal" (per band)
// and a report file next to the other settings; drive levels and the S-meter checks are saved where the single buttons save them.
#include "TransmitCard.h"
#include "RadioPanel.h"
#include "ReceiverPanel.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QGridLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#include <fstream>
#include "fireside/siggen.h"
#include "fireside/tx_meter.h"

namespace txm = fireside::txm;
namespace txc = fireside::txc;
namespace atu = fireside::atu;

namespace {
// Where each band is tested: a carrier 1 kHz above these (USB), in the band's CW end, away from the FT8 / WSPR frequencies.
// 60 m: 5.357 (the radio put out no RF, and its generator was not seen, at 5.354: its 60 m transmit is limited to channel frequencies; 5.357 worked).
constexpr double kLoadCalMhz[txm::kBands] = {1.830, 3.530, 5.357, 7.030, 10.120, 14.050, 18.080, 21.050, 24.900, 28.050, 50.050};
constexpr double kLoadNoiseMaxDbm = -105.0;   // preamp ON, 2.4 kHz. The first run into a 20 dB Tenuline (2026-11-28) read -114 .. -129 with the preamp on, but
                                              // -101 .. -122 with it off (the receiver's own noise; 6 m worst): so the preamp-on floor is the one judged
constexpr int kLoadMaxRelay = 12;             // the tuner's inductor / capacitor steps on a 50 ohm load (2026-11-28: at most 5)
QString lc_dir() { return QDir::homePath() + "/.config/fireside"; }
bool no_settings() { return qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS"); }
}  // namespace

struct TransmitCard::LoadCal {
    int ant = 3; std::vector<int> bands; size_t i = 0; int phase = 0;
    double dial0 = 0, filter0 = 0; int mode0 = 0; bool pre0 = false;
    struct Band { double mhz = 0; double noise = NAN, noise_pre = NAN; QString gen, tune, drive; int ind = -1, cap = -1; double k = 0;
                  double swr[2] = {NAN, NAN}, fwd[2] = {NAN, NAN}, rev[2] = {NAN, NAN}, t_fwd[2] = {NAN, NAN}, t_swr[2] = {NAN, NAN}; bool tx_ok = false; };
    std::vector<Band> res; std::vector<double> samples; QTimer* t = nullptr;
};

bool TransmitCard::loadCalRunning() const { return lc_ != nullptr; }

void TransmitCard::onLoadCal() {
    if (lc_) return;
    if (!tx_ || !tx_->armed() || worker_running_ || ft_pending_) { report("Calibrate with a load needs transmit armed and nothing else running."); return; }
    QDialog d(this); d.setWindowTitle("Calibrate with a load");
    auto* v = new QVBoxLayout(&d);
    auto* text = new QLabel("Connect a 50 Ω dummy load, or a power attenuator of 20 dB or more (output open or terminated), to one antenna jack. "
                            "Its power rating must cover 15 W continuous.\n\nFor each band ticked below the routine:\n"
                            "  1. receive only: reads the noise floor of the load (preamp off and on), and stops if the jack sounds like an antenna;\n"
                            "  2. receive only: checks the S-meter with the internal generator;\n"
                            "  3. tunes the ATU at 3 W into the load;\n"
                            "  4. measures the drive level (2 W ramp);\n"
                            "  5. sends steady tones at 5 W and 15 W (3 s each) to record the power meter's reading on a good load.\n\n"
                            "About 45 s per band. Stop ends it at once; any safety stop ends it too. Your Antennas tab is not changed.");
    text->setWordWrap(true); v->addWidget(text);
    auto* jack = new QComboBox; jack->addItems({"ANT1", "ANT2", "ANT3"}); jack->setCurrentIndex(2);
    auto* jr = new QHBoxLayout; jr->addWidget(new QLabel("The load is on:")); jr->addWidget(jack); jr->addStretch(); v->addLayout(jr);
    auto* grid = new QGridLayout; std::vector<QCheckBox*> boxes;
    for (int b = 0; b < txm::kBands; ++b) { auto* c = new QCheckBox(txm::band_name(b)); c->setChecked(true); boxes.push_back(c); grid->addWidget(c, b / 6, b % 6); }
    v->addLayout(grid);
    auto* sure = new QCheckBox("A load (not an antenna) is connected to that jack"); v->addWidget(sure);
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); bb->button(QDialogButtonBox::Ok)->setText("Start"); bb->button(QDialogButtonBox::Ok)->setEnabled(false);
    connect(sure, &QCheckBox::toggled, bb->button(QDialogButtonBox::Ok), &QWidget::setEnabled);
    connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept); connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject); v->addWidget(bb);
    if (d.exec() != QDialog::Accepted) return;
    std::vector<int> bands; for (int b = 0; b < txm::kBands; ++b) if (boxes[size_t(b)]->isChecked()) bands.push_back(b);
    startLoadCal(jack->currentIndex() + 1, bands);
}

bool TransmitCard::startLoadCal(int ant, const std::vector<int>& bands) {
    if (lc_ || !tx_ || !tx_->armed() || worker_running_ || ft_pending_ || ant < 1 || ant > 3 || bands.empty()) return false;
    lc_ = std::make_shared<LoadCal>(); lc_->ant = ant; lc_->bands = bands;
    lc_->dial0 = rx_->dialHz(); lc_->filter0 = rx_->filterWidth(); lc_->mode0 = rx_->modeIndex(); lc_->pre0 = radio_->session().preamp_on();
    lc_->t = new QTimer(this); lc_->t->setSingleShot(true);
    lc_report_.clear();
    report(QString("Calibrate with a load on ANT%1: %2 band(s), about %3 min. Stop ends it.").arg(ant).arg(bands.size()).arg((bands.size() * 45 + 59) / 60));
    updateButtons();
    QTimer::singleShot(0, this, [this] { lcStep(); });
    return true;
}

void TransmitCard::lcAfter(int ms) { if (lc_) { auto g = lc_; QTimer::singleShot(ms, this, [this, g] { if (lc_ == g) lcStep(); }); } }

void TransmitCard::lcJob(const QString& what, bool pauses, std::function<QString(txc::TxControl&)> job) {
    auto g = lc_;
    if (worker_running_) { QTimer::singleShot(500, this, [this, g, what, pauses, job] { if (lc_ == g) lcJob(what, pauses, job); }); return; }   // e.g. the band's ATU restore after the dial moved
    after_job_ = [this, g](const QString& res, bool safety) {
        after_job_ = nullptr;
        if (lc_ != g) return;
        if (safety || res.startsWith("UNKEY FAILED")) { lcFinish("stopped by a safety stop: " + res); return; }
        lcAfter(lc_fast_ ? 100 : 800);
    };
    run(what, pauses, std::move(job));
    if (!worker_running_) { after_job_ = nullptr; lcFinish("the transmit worker did not start"); }
}

void TransmitCard::lcStep() {
    if (!lc_) return;
    auto& L = *lc_;
    if (!tx_ || !tx_->armed()) { lcFinish("transmit is no longer armed"); return; }
    if (L.i >= L.bands.size()) { lcFinish(""); return; }
    const int band = L.bands[L.i]; const double mhz = kLoadCalMhz[band];
    if (L.res.size() <= L.i) { L.res.emplace_back(); L.res.back().mhz = mhz; }
    auto& R = L.res[L.i];   // used only before anything is scheduled; callbacks index lc_->res[lc_->i] afresh
    txc::Options o; o.rate = simulated_ ? 48000 : txRate(); o.mirror_q = mirror_->isChecked(); o.tx_ant = L.ant; o.tone_hz = 1000;
    auto progress = [this](const txc::Progress& p) { std::lock_guard<std::mutex> l(m_); last_ = p; have_last_ = true; };
    const QString bn = txm::band_name(band);
    auto sample = [this](int n, std::function<void()> then) {               // n readings 100 ms apart of the in-passband level, averaged as power
        lc_->samples.clear();
        auto tick = std::make_shared<std::function<void()>>(); auto g = lc_; auto ticks = std::make_shared<int>(0);
        *tick = [this, n, then, tick, g, ticks] {
            if (lc_ != g) return;
            const double db = rx_->levelForChecks().first; if (db > -199) lc_->samples.push_back(db);
            if (int(lc_->samples.size()) < n && ++*ticks < 3 * n) { QTimer::singleShot(100, this, *tick); return; }   // no level at all (receive not running): give up after 3 n tries
            then();
        };
        QTimer::singleShot(100, this, *tick);
    };
    auto mean_db = [this] { if (lc_->samples.empty()) return double(NAN); double p = 0; for (double d : lc_->samples) p += std::pow(10.0, d / 10); return 10 * std::log10(p / double(lc_->samples.size())); };
    switch (L.phase) {
    case 0: {                                                                   // the band: dial, USB 2.4 kHz, preamp off, receive on the load
        report(QString("Calibrate with a load: %1 (%2 of %3), receive checks on ANT%4 ...").arg(bn).arg(L.i + 1).arg(L.bands.size()).arg(L.ant));
        rx_->setDialHz(mhz * 1e6); rx_->setModeIndex(0); rx_->setFilterWidth(2400);
        radio_->session().set_preamp(false); rx_->setPreampChecked(false);
        QTimer::singleShot(400, this, [this, g = lc_] { if (lc_ == g) radio_->requestRxAntenna(QString("ANT%1").arg(lc_->ant), "load calibration"); });   // after the Antennas tab's own switch for the band
        L.phase = 1; lcAfter(lc_fast_ ? 200 : 2000); return;
    }
    case 1:                                                                     // noise floor, preamp off
        sample(lc_fast_ ? 3 : 20, [this, mean_db] {
            lc_->res[lc_->i].noise = mean_db() + rx_->meterOffsetDb();
            radio_->session().set_preamp(true); rx_->setPreampChecked(true);
            lc_->phase = 2; lcAfter(lc_fast_ ? 200 : 1200);
        });
        return;
    case 2:                                                                     // noise floor, preamp on; then the load check
        sample(lc_fast_ ? 3 : 20, [this, mean_db, bn] {
            auto& R = lc_->res[lc_->i];
            R.noise_pre = mean_db() + rx_->meterOffsetDb();
            radio_->session().set_preamp(false); rx_->setPreampChecked(false);
            if (!std::isfinite(R.noise_pre)) report(QString("Calibrate with a load: no receive level on %1, so the noise floor is not known; the tune's relays still have to look like a load.").arg(bn));
            if (std::isfinite(R.noise_pre) && R.noise_pre > kLoadNoiseMaxDbm) {
                lcFinish(QString("ANT%1 on %2 reads a noise floor of %3 dBm with the preamp on: that is an antenna (or a strong signal), not a load. Nothing was transmitted on this band")
                         .arg(lc_->ant).arg(bn).arg(R.noise_pre, 0, 'f', 1)); return; }
            lc_->phase = 3; lcAfter(lc_fast_ ? 100 : 600);
        });
        return;
    case 3: {                                                                   // the S-meter check with the internal generator (receive only)
        L.phase = 4;
        const double dial = mhz; ReceiverPanel* rx = rx_; RadioPanel* radio = radio_;
        auto read = [rx] { return rx->levelForChecks(); };
        auto preamp = [radio](bool on) { const auto r = radio->session().set_preamp(on); return r == fireside::PreampResult::Sent || r == fireside::PreampResult::Unchanged; };
        auto g = lc_;
        lcJob(QString("Calibrate with a load: %1 S-meter check (internal generator, receive only)").arg(bn), false, [this, dial, read, preamp, band, g](txc::TxControl& t) {
            const auto log = [this](const std::string& m) { const QString q = QString::fromStdString(m); QMetaObject::invokeMethod(this, [this, q] { emit message("S-meter check: " + q); }, Qt::QueuedConnection); };
            const auto r = t.sig_gen_check(dial, 1000.0, read, preamp, false, log);
            const QString s = r.ok ? QString("generator %1 dBFS, meter offset %2 dB%3").arg(r.on_dbfs, 0, 'f', 1).arg(fireside::siggen::kLevelDbm - r.on_dbfs, 0, 'f', 1)
                                         .arg(r.preamp_measured ? QString(", preamp %1 dB").arg(r.preamp_dbfs - r.on_dbfs, 0, 'f', 1) : QString())
                                   : QString("not done: %1").arg(QString::fromStdString(r.why));
            QMetaObject::invokeMethod(this, [this, r, band, g, s] { applyGenCheck(r, band); if (lc_ == g && lc_->i < lc_->res.size()) lc_->res[lc_->i].gen = s; }, Qt::QueuedConnection);
            return QString("Calibrate with a load: S-meter check %1.").arg(s);
        });
        return;
    }
    case 4: {                                                                   // tune at 3 W into the load
        L.phase = 5; auto g = lc_;
        lcJob(QString("Calibrate with a load: %1 tune at 3 W on ANT%2").arg(bn).arg(L.ant), true, [this, mhz, o, progress, g](txc::TxControl& t) {
            const auto r = t.tune(mhz, 3.0, 1.5, o, progress);
            const bool pass = r.outcome && r.outcome->result == atu::Atu::TuneResult::Pass && r.outcome->status.ind && r.outcome->status.cap;
            const int ind = pass ? *r.outcome->status.ind : -1, cap = pass ? *r.outcome->status.cap : -1;
            QString why = QString::fromStdString(r.stop);                         // why there was no pass: the tuner's fail reason, a rejected pass, or our stop
            if (r.outcome && r.outcome->result == atu::Atu::TuneResult::Fail) why = QString("FAIL: %1").arg(atu::fail_text(r.outcome->fail_reason));
            if (!r.pass_rejected.empty()) why = QString::fromStdString(r.pass_rejected);
            if (r.searches > 1) why += QString(", %1 searches").arg(r.searches);
            const QString s = !r.outcome ? "refused: " + QString::fromStdString(r.why_not) : pass ? QString("pass, ind %1 cap %2").arg(ind).arg(cap) : "no pass (" + why + ")";
            QMetaObject::invokeMethod(this, [this, g, s, pass, ind, cap] { if (lc_ != g || lc_->i >= lc_->res.size()) return; auto& B = lc_->res[lc_->i]; B.tune = s; B.ind = ind; B.cap = cap; B.tx_ok = pass; }, Qt::QueuedConnection);
            if (r.unkey_failed) { unkey_failed_ = true; return QString("UNKEY FAILED: ") + QString::fromStdString(r.stop); }
            return QString("Calibrate with a load: tune %1.").arg(s);
        });
        return;
    }
    case 5:                                                                     // judge the tune: a load needs (almost) no matching
        if (!R.tx_ok && R.tune.contains("no RF detected")) { report(QString("Calibrate with a load: %1: the radio put out no RF (%2): its transmit steps are skipped.").arg(bn, R.tune)); L.phase = 9; lcAfter(100); return; }
        if (!R.tx_ok && !R.tune.startsWith("refused")) {                        // the search failed (6 m: it collapses even into a load): a 50 ohm load needs no matching, so straight through
            L.phase = 51; auto g = lc_;
            lcJob(QString("Calibrate with a load: %1 did not tune; ATU set straight through (no RF)").arg(bn), false, [this, g](txc::TxControl& t) {
                std::string msg; const bool ok = t.set_atu_relays(0, 0, true, [&](const std::string& m) { msg = m; });
                QMetaObject::invokeMethod(this, [this, g, ok] { if (lc_ != g || lc_->i >= lc_->res.size()) return; auto& B = lc_->res[lc_->i];
                    B.tx_ok = ok; B.ind = 0; B.cap = 0; B.tune += ok ? "; straight through (ind 0 cap 0)" : "; straight through NOT confirmed"; }, Qt::QueuedConnection);
                return QString("Calibrate with a load: ") + QString::fromStdString(msg) + (ok ? "; the drive ramp's SWR limit (3.0) checks the match." : ".");
            });
            return;
        }
        if (!R.tx_ok) { report(QString("Calibrate with a load: %1 did not tune (%2): its transmit steps are skipped.").arg(bn, R.tune)); L.phase = 9; lcAfter(100); return; }
        if (R.ind > kLoadMaxRelay || R.cap > kLoadMaxRelay) {
            lcFinish(QString("the tuner needed ind %1 cap %2 on %3: far more than a 50 Ω load needs, so ANT%4 is not a load. Stopped").arg(R.ind).arg(R.cap).arg(bn).arg(L.ant)); return; }
        L.phase = 6; lcAfter(100); return;
    case 51:                                                                    // after setting the ATU straight through
        if (!R.tx_ok) { report(QString("Calibrate with a load: %1: the ATU could not be set straight through: its transmit steps are skipped.").arg(bn)); L.phase = 9; lcAfter(100); return; }
        L.phase = 6; lcAfter(100); return;
    case 6: {                                                                   // Measure drive (2 W ramp)
        L.phase = 7; auto g = lc_;
        lcJob(QString("Calibrate with a load: %1 measuring the drive").arg(bn), true, [this, mhz, o, progress, g](txc::TxControl& t) {
            const auto r = t.ramp(mhz, 2.0, o, progress);
            const QString s = r.reached ? QString("%1 W per drive²").arg(r.k, 0, 'f', 0) : "stopped: " + QString::fromStdString(r.stop);
            QMetaObject::invokeMethod(this, [this, g, s, r] { if (lc_ != g || lc_->i >= lc_->res.size()) return; auto& B = lc_->res[lc_->i]; B.drive = s; B.k = r.reached ? r.k : 0; B.tx_ok = B.tx_ok && r.reached; }, Qt::QueuedConnection);
            if (r.unkey_failed) { unkey_failed_ = true; return QString("UNKEY FAILED: ") + QString::fromStdString(r.stop); }
            return QString("Calibrate with a load: drive %1.").arg(s);
        });
        return;
    }
    case 7: case 8: {                                                           // steady tones at 5 W and 15 W: the meters on a good load
        if (!R.tx_ok) { L.phase = 9; lcAfter(100); return; }
        const int k = L.phase - 7; const double w = k == 0 ? 5.0 : 15.0; L.phase += 1; auto g = lc_;
        const double hold = lc_fast_ ? 0.6 : 3.0;
        lcJob(QString("Calibrate with a load: %1 tone at %2 W").arg(bn).arg(w, 0, 'f', 0), true, [this, mhz, o, w, k, progress, g, hold](txc::TxControl& t) {
            const auto r = t.ramp(mhz, w, o, progress, {}, hold);
            double f = 0, rv = 0, sw = 0, af = 0, as = 0; int n = 0, na = 0;
            for (const auto& c : r.compare) { f += c.fwd_w; rv += c.rev_w; sw += c.swr; ++n; if (c.atu_fwd_w && c.atu_swr) { af += *c.atu_fwd_w; as += *c.atu_swr; ++na; } }
            const bool ok = r.reached && n > 0;
            QMetaObject::invokeMethod(this, [this, g, k, ok, f, rv, sw, af, as, n, na] {
                if (lc_ != g || lc_->i >= lc_->res.size()) return; auto& B = lc_->res[lc_->i];
                if (ok) { B.fwd[k] = f / n; B.rev[k] = rv / n; B.swr[k] = sw / n; if (na) { B.t_fwd[k] = af / na; B.t_swr[k] = as / na; } } else B.tx_ok = false; }, Qt::QueuedConnection);
            if (r.unkey_failed) { unkey_failed_ = true; return QString("UNKEY FAILED: ") + QString::fromStdString(r.stop); }
            return ok ? QString("Calibrate with a load: %1 W tone, our SWR %2, tuner %3.").arg(w, 0, 'f', 0).arg(sw / n, 0, 'f', 2).arg(na ? QString::number(as / na, 'f', 2) : QString("no report"))
                      : QString("Calibrate with a load: %1 W tone stopped: %2.").arg(w, 0, 'f', 0).arg(QString::fromStdString(r.stop));
        });
        return;
    }
    default:                                                                    // the next band
        L.i += 1; L.phase = 0; lcAfter(200); return;
    }
}

void TransmitCard::lcFinish(const QString& why) {
    if (!lc_) return;
    auto L = lc_; lc_.reset(); after_job_ = nullptr;
    if (tx_ && worker_running_) tx_->abort();
    // back to where the operator was: dial (the Antennas tab picks its antenna), mode, filter, preamp, the band's own ATU relays. After a job still
    // running (Stop during a step) has ended: the S-meter check puts the preamp back as it found it, which must not undo ours.
    auto restore = std::make_shared<std::function<void()>>();
    *restore = [this, L, restore] {
        if (worker_running_) { QTimer::singleShot(100, this, *restore); return; }
        rx_->setDialHz(L->dial0); rx_->setModeIndex(L->mode0); rx_->setFilterWidth(L->filter0);
        radio_->session().set_preamp(L->pre0); rx_->setPreampChecked(L->pre0);
        if (rx_ant_for_) { const QString a = rx_ant_for_(L->dial0); if (!a.isEmpty()) QTimer::singleShot(400, this, [this, a] { radio_->requestRxAntenna(a, "after load calibration"); }); }
        QTimer::singleShot(1500, this, [this] { const int b = txm::band_of(rx_->dialHz());
            if (b >= 0 && auto_restore_->isChecked() && tx_ && tx_->armed() && !worker_running_ && tx_->memory().recall(b, txAnt())) onRestore(); });
    };
    (*restore)();
    // the report, and what is kept per band
    const QString utc = QDateTime::currentDateTimeUtc().toString(Qt::ISODate);
    QStringList lines; lines << QString("Load calibration %1, load on ANT%2%3").arg(utc).arg(L->ant).arg(why.isEmpty() ? QString() : ": " + why);
    lines << "band   MHz      noise dBm (pre on)  S-meter check                     tune                drive            5 W: ours SWR / tuner   15 W: ours fwd SWR / tuner fwd SWR";
    auto f2 = [](double v, int p = 2) { return std::isfinite(v) ? QString::number(v, 'f', p) : QString("-"); };
    QSettings* q = no_settings() ? nullptr : new QSettings("fireside", "loadcal");
    for (size_t i = 0; i < L->res.size(); ++i) {
        const auto& B = L->res[i]; const int band = L->bands[i];
        lines << QString("%1 %2  %3 (%4)  %5  %6  %7  %8 / %9   %10 W %11 / %12 W %13").arg(txm::band_name(band), -5).arg(B.mhz, 7, 'f', 3).arg(f2(B.noise, 1)).arg(f2(B.noise_pre, 1))
                    .arg(B.gen.isEmpty() ? QString("-") : B.gen, -32).arg(B.tune.isEmpty() ? QString("-") : B.tune, -18).arg(B.drive.isEmpty() ? QString("-") : B.drive, -16)
                    .arg(f2(B.swr[0])).arg(f2(B.t_swr[0])).arg(f2(B.fwd[1], 1)).arg(f2(B.swr[1])).arg(f2(B.t_fwd[1], 1)).arg(f2(B.t_swr[1]));
        if (q && (std::isfinite(B.noise) || B.k > 0 || std::isfinite(B.swr[1]))) {
            q->beginGroup(txm::band_name(band)); q->setValue("utc", utc); q->setValue("jack", L->ant); q->setValue("mhz", B.mhz);
            auto put = [q](const char* key, double v) { if (std::isfinite(v)) q->setValue(key, v); };
            put("noise_dbm", B.noise); put("noise_preamp_dbm", B.noise_pre); if (B.k > 0) q->setValue("drive_k", B.k);
            put("swr_5w", B.swr[0]); put("fwd_5w", B.fwd[0]); put("rev_5w", B.rev[0]); put("tuner_swr_5w", B.t_swr[0]); put("tuner_fwd_5w", B.t_fwd[0]);
            put("swr_15w", B.swr[1]); put("fwd_15w", B.fwd[1]); put("rev_15w", B.rev[1]); put("tuner_swr_15w", B.t_swr[1]); put("tuner_fwd_15w", B.t_fwd[1]);
            q->endGroup();
        }
    }
    delete q;
    lc_report_ = lines.join("\n");
    if (!no_settings()) { QDir().mkpath(lc_dir()); std::ofstream(QString(lc_dir() + "/load-calibration.txt").toStdString()) << lc_report_.toStdString() << "\n"; }
    for (const auto& l : lines) emit message("Transmit: " + l);
    const QString end = why.isEmpty() ? QString("Calibrate with a load: done on %1 band(s). Results saved (Transmit log, load-calibration.txt).").arg(L->res.size())
                                      : QString("Calibrate with a load ended: %1.").arg(why);
    report(end); updateButtons(); updateMemoryLabel();
    if (auto_confirm_ < 0) QMessageBox::information(this, "Calibrate with a load", end + "\n\n" + lc_report_);
}
