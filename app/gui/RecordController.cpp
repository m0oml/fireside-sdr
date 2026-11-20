#include "RecordController.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDesktopServices>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QPushButton>
#include <QSettings>
#include <QTextStream>
#include <QUrl>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include "ReceiverPanel.h"

namespace {
QString hms(double s) { const int t = int(s); return QString("%1:%2").arg(t / 60).arg(t % 60, 2, 10, QChar('0')); }
QString mb(uint64_t bytes) { return bytes >= 1000000000ull ? QString("%1 GB").arg(double(bytes) / 1e9, 0, 'f', 2) : QString("%1 MB").arg(double(bytes) / 1e6, 0, 'f', 1); }
QString clean(const QString& s) { QString o; for (QChar c : s) if (c.isLetterOrNumber() || c == '-' || c == '_') o += c; else if (c == ' ') o += '_'; return o.left(40); }
}  // namespace

RecordController::RecordController(ReceiverPanel* rx) : QObject(rx), rx_(rx) {
    folder_ = QDir::homePath() + "/fireside/recordings/grabs"; load();
    rx_->engine().set_taps([this](const fireside::dsp::cf* x, size_t n, double) { iq_.push(reinterpret_cast<const float*>(x), n); },
                           [this](const float* x, size_t n, double) { audio_.push(x, n); });
}
RecordController::~RecordController() { rx_->engine().set_taps(nullptr, nullptr); iq_.stop(); audio_.stop(); }

void RecordController::sourceChanged(bool live, double iq_rate, double audio_rate) {
    const bool was = recording();
    iq_rate_ = iq_rate; audio_rate_ = audio_rate; live_ = live && iq_rate > 0;
    iq_.configure(live_ && want_iq_ ? iq_rate : 0.0, live_ && want_iq_ ? double(replay_s_) : 0.0);          // configure() also finishes a recording: the rate of a file is fixed
    audio_.configure(live_ ? audio_rate : 0.0, live_ && want_audio_ ? double(replay_s_) : 0.0);
    if (was) { emit message(QString("Recording stopped: the stream changed. Saved: %1").arg(QFileInfo(iq_.status().last_path.empty() ? audio_.status().last_path.c_str() : iq_.status().last_path.c_str()).fileName())); }
    emit stateChanged();
}
bool RecordController::recording() const { return iq_.status().recording || audio_.status().recording; }
QString RecordController::recordingText() const {
    const auto a = iq_.status(), b = audio_.status(); if (!a.recording && !b.recording) return QString();
    const auto& s = a.recording ? a : b; QString t = QString("REC %1  %2").arg(hms(s.rec_seconds), mb(a.rec_bytes + b.rec_bytes));
    if (a.dropped_frames || b.dropped_frames) t += "  (DROPPING: disk too slow)"; return t;
}
QString RecordController::baseName(const QString& label) {
    const QString mode = rx_->modeName().toLower(); QString n = QString("%1_%2kHz_%3").arg(QDateTime::currentDateTimeUtc().toString("yyyyMMdd_HHmmss")).arg(std::lround(rx_->dialHz() / 1000.0)).arg(mode);
    const QString l = clean(label); if (!l.isEmpty()) n += "_" + l;
    QString unique = n; for (int k = 2; used_.contains(folder_ + "/" + unique) || QFile::exists(folder_ + "/" + unique + "_iq.wav") || QFile::exists(folder_ + "/" + unique + "_audio.wav"); ++k) unique = QString("%1_%2").arg(n).arg(k);   // two presses in one second must not overwrite each other (a grab's file appears a moment later: remember the names)
    used_.insert(folder_ + "/" + unique); return unique;
}
void RecordController::writeSidecar(const QString& wav, const QString& kind, double rate, double seconds, const QDateTime& end) const {
    QFile f(QFileInfo(wav).path() + "/" + QFileInfo(wav).completeBaseName() + ".txt"); if (!f.open(QIODevice::WriteOnly | QIODevice::Text)) return; QTextStream t(&f);
    // utc = when the last sample was taken (the press of Grab / Stop), start_utc = when the first one was (utc - the exact length), both to the millisecond: FT8/FT4 slots start on 15 s / 7.5 s
    // boundaries, so tools/compare_ft8.py can align a recording without guessing. Accurate to the stream's latency (tens of ms).
    const QString fmt = "yyyy-MM-ddTHH:mm:ss.zzzZ";
    t << "app=fireside kind=" << kind << " rate_hz=" << std::lround(rate) << " seconds=" << QString::number(seconds, 'f', 3) << "\n";
    t << "utc=" << end.toString(fmt) << " start_utc=" << end.addMSecs(-std::llround(seconds * 1000.0)).toString(fmt) << " dial_hz=" << std::lround(rx_->dialHz()) << " mode=" << rx_->modeName() << "\n";
    if (kind == "iq") t << "mirror=" << (rx_->mirrorOn() ? 1 : 0) << " if_offset_hz=" << std::lround(rx_->ifOffsetHz()) << " channels=I,Q raw_radio_iq=1\n";
    t << "antenna=" << rx_->rxAntennaText() << " source=" << rx_->sourceText() << "\n";
    if (!label_.isEmpty()) t << "label=" << label_ << "\n";
}

void RecordController::grab() {
    if (!live_) { emit message("Nothing to grab: open the live stream first."); return; }
    if (!QDir().mkpath(folder_)) { emit message("Cannot create " + folder_); return; }
    const QDateTime now = QDateTime::currentDateTimeUtc(); const QString base = baseName(label_); QStringList saved; double secs = 0;
    if (want_iq_ && iq_.buffered_seconds() > 0) { const QString p = folder_ + "/" + base + "_iq.wav"; const double s = std::min<double>(grab_s_, iq_.buffered_seconds()); if (iq_.grab(grab_s_, p.toStdString())) { writeSidecar(p, "iq", iq_rate_, s, now); saved << QFileInfo(p).fileName(); secs = s; recent_.prepend(p); } }
    if (want_audio_ && audio_.buffered_seconds() > 0) { const QString p = folder_ + "/" + base + "_audio.wav"; const double s = std::min<double>(grab_s_, audio_.buffered_seconds()); if (audio_.grab(grab_s_, p.toStdString())) { writeSidecar(p, "audio", audio_rate_, s, now); saved << QFileInfo(p).fileName(); if (secs == 0) secs = s; recent_.prepend(p); } }
    last_summary_ = saved.isEmpty() ? QString("Nothing buffered yet (or a grab is still being written).") : QString("Grabbed the last %1 s: %2 (in %3)").arg(secs, 0, 'f', 0).arg(saved.join(", "), folder_);
    emit message(last_summary_);
}
void RecordController::toggleRecord() {
    if (recording()) {
        const QDateTime now = QDateTime::currentDateTimeUtc();                                                       // the moment of the press: the end of the data
        const QString a = QString::fromStdString(iq_.status().path), b = QString::fromStdString(audio_.status().path);
        iq_.stop(); audio_.stop();                                                                                  // waits for the writer: the length below is final
        const auto st = !a.isEmpty() ? iq_.status() : audio_.status(); const QString what = QStringList{a.isEmpty() ? QString() : QFileInfo(a).fileName(), b.isEmpty() ? QString() : QFileInfo(b).fileName()}.join(a.isEmpty() || b.isEmpty() ? "" : ", ");
        QString err = QString::fromStdString(iq_.status().error); if (err.isEmpty()) err = QString::fromStdString(audio_.status().error);
        last_summary_ = QString("Recording saved (%1 s): %2%3").arg(st.rec_seconds, 0, 'f', 0).arg(what, err.isEmpty() ? QString() : QString(" [%1]").arg(err));
        if (!a.isEmpty()) writeSidecar(a, "iq", iq_rate_, iq_.status().rec_seconds, now); if (!b.isEmpty()) writeSidecar(b, "audio", audio_rate_, audio_.status().rec_seconds, now);
        emit message(last_summary_); emit stateChanged(); return;
    }
    if (!live_) { emit message("Nothing to record: open the live stream first."); return; }
    if (!want_iq_ && !want_audio_) { emit message("Recording options: tick I/Q and/or audio (the \"...\" button)."); return; }
    if (!QDir().mkpath(folder_)) { emit message("Cannot create " + folder_); return; }
    const QString base = baseName(label_); bool ok = true; QString a, b;
    if (want_iq_) { a = folder_ + "/" + base + "_iq.wav"; ok = iq_.start(a.toStdString(), preroll_s_); }
    if (ok && want_audio_) { b = folder_ + "/" + base + "_audio.wav"; ok = audio_.start(b.toStdString(), preroll_s_); }
    if (!ok) { iq_.stop(); audio_.stop(); emit message("Cannot start recording in " + folder_); return; }
    if (!a.isEmpty()) recent_.prepend(a); if (!b.isEmpty()) recent_.prepend(b);
    emit message(QString("Recording%1... (press again to stop)").arg(preroll_s_ ? QString(" with the last %1 s").arg(preroll_s_) : QString())); emit stateChanged();
}

void RecordController::setOptions(int replay_s, int grab_s, int preroll_s, bool iq, bool audio) {
    replay_s_ = replay_s; grab_s_ = std::min(grab_s, replay_s); preroll_s_ = std::min(preroll_s, replay_s); want_iq_ = iq; want_audio_ = audio; save();
    if (live_) sourceChanged(true, iq_rate_, audio_rate_);                                                        // new buffer size / contents
}
void RecordController::save() {
    if (qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) return;
    QSettings q("fireside", "record"); q.setValue("replay_s", replay_s_); q.setValue("grab_s", grab_s_); q.setValue("preroll_s", preroll_s_); q.setValue("iq", want_iq_); q.setValue("audio", want_audio_); q.setValue("folder", folder_); q.setValue("label", label_);
}
void RecordController::load() {
    if (qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) return;
    QSettings q("fireside", "record"); replay_s_ = std::clamp(q.value("replay_s", 60).toInt(), 10, 180); grab_s_ = std::clamp(q.value("grab_s", 30).toInt(), 5, replay_s_); preroll_s_ = std::clamp(q.value("preroll_s", 5).toInt(), 0, replay_s_);
    want_iq_ = q.value("iq", true).toBool(); want_audio_ = q.value("audio", true).toBool(); folder_ = q.value("folder", folder_).toString(); label_ = q.value("label").toString();
}

void RecordController::showOptions(QWidget* parent) {
    QDialog d(parent); d.setWindowTitle("Recording options"); auto* col = new QVBoxLayout(&d); auto* form = new QFormLayout;
    auto combo = [](std::initializer_list<int> v, int cur, const char* unit) { auto* c = new QComboBox; int idx = 0, i = 0; for (int x : v) { c->addItem(QString("%1 %2").arg(x).arg(unit), x); if (x == cur) idx = i; ++i; } c->setCurrentIndex(idx); return c; };
    auto* replay = combo({15, 30, 60, 120, 180}, replay_s_, "s"); auto* grab = combo({10, 15, 30, 60, 120}, grab_s_, "s"); auto* pre = combo({0, 2, 5, 10, 15, 30}, preroll_s_, "s");
    replay->setToolTip("How much of the live stream is kept in memory so you can save what just happened. The I/Q needs about 1.5 MB per second at 192 kHz (a minute = 92 MB).");
    auto* iq = new QCheckBox("I/Q of the whole band (for replay and analysis)"); iq->setChecked(want_iq_); auto* au = new QCheckBox("Audio (what you hear, 48 kHz)"); au->setChecked(want_audio_);
    auto* label = new QLineEdit(label_); label->setPlaceholderText("optional, added to the file names (e.g. CW-7026)");
    auto* fold = new QLineEdit(folder_); auto* browse = new QPushButton("Browse..."); auto* open = new QPushButton("Open folder"); auto* fr = new QHBoxLayout; fr->addWidget(fold, 1); fr->addWidget(browse); fr->addWidget(open);
    form->addRow("Replay buffer keeps", replay); form->addRow("Grab saves the last", grab); form->addRow("Rec starts with the last", pre); form->addRow("Save", iq); form->addRow("", au); form->addRow("Label", label); form->addRow("Folder", fr);
    col->addLayout(form);
    auto* list = new QListWidget; list->setMaximumHeight(120); for (const QString& f : recent_) list->addItem(f); auto* openrx = new QPushButton("Open the selected I/Q file in the receiver");
    col->addWidget(new QLabel("Saved this session (newest first):")); col->addWidget(list); col->addWidget(openrx);
    connect(browse, &QPushButton::clicked, &d, [&] { const QString p = QFileDialog::getExistingDirectory(&d, "Recording folder", fold->text()); if (!p.isEmpty()) fold->setText(p); });
    connect(open, &QPushButton::clicked, &d, [&] { QDir().mkpath(fold->text()); QDesktopServices::openUrl(QUrl::fromLocalFile(fold->text())); });
    connect(openrx, &QPushButton::clicked, &d, [&] { auto* it = list->currentItem(); if (it && it->text().endsWith("_iq.wav") && rx_->loadFile(it->text())) d.accept(); else if (it) QDesktopServices::openUrl(QUrl::fromLocalFile(it->text())); });
    auto* bb = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel); col->addWidget(bb); connect(bb, &QDialogButtonBox::accepted, &d, &QDialog::accept); connect(bb, &QDialogButtonBox::rejected, &d, &QDialog::reject);
    if (d.exec() != QDialog::Accepted) return;
    folder_ = fold->text().trimmed().isEmpty() ? folder_ : fold->text().trimmed(); label_ = label->text().trimmed();
    setOptions(replay->currentData().toInt(), grab->currentData().toInt(), pre->currentData().toInt(), iq->isChecked(), au->isChecked());
}
