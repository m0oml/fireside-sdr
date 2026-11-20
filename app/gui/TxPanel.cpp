#include "TxPanel.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDateTime>
#include <QDir>
#include <QDoubleSpinBox>
#include <QElapsedTimer>
#include <QFrame>
#include <QKeyEvent>
#include <QPainter>
#include <QFileDialog>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMetaObject>
#include <QPushButton>
#include <QSettings>
#include <QStackedWidget>
#include <QTemporaryDir>
#include <QTimer>
#include <QVBoxLayout>
#include <algorithm>
#include <cmath>
#include <exception>
#include <stdexcept>
#include "Theme.h"
#include "fireside/dsp.h"
using namespace fireside::tx;

namespace {
QDoubleSpinBox* dspin(double lo, double hi, double v, double step, int dec, const char* suffix) {
    auto* s = new QDoubleSpinBox; s->setMaximumWidth(150); s->setRange(lo, hi); s->setDecimals(dec); s->setSingleStep(step); s->setValue(v); if (*suffix) s->setSuffix(suffix); return s;
}
QLabel* muted(const QString& t) { auto* l = new QLabel(t); l->setObjectName("muted"); l->setWordWrap(true); return l; }
QString dbfs(double v) { return v > 0 ? QString::number(20.0 * std::log10(v), 'f', 1) : QStringLiteral("-inf"); }
}  // namespace

RenderPlot::RenderPlot(QWidget* parent) : QWidget(parent) { setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding); }
void RenderPlot::clear() { img_ = QImage(); seconds_ = 0; update(); }
namespace {
QRgb heat(float t) {                                                  // 0..1 -> dark blue, blue, cyan, yellow, white
    static const float p[] = {0.f, 0.35f, 0.6f, 0.85f, 1.f}; static const int c[][3] = {{5, 10, 40}, {27, 74, 156}, {47, 208, 232}, {255, 226, 122}, {255, 255, 255}};
    t = std::min(std::max(t, 0.f), 1.f); int i = 0; while (i < 3 && t > p[i + 1]) ++i; const float u = (t - p[i]) / (p[i + 1] - p[i]); int o[3];
    for (int k = 0; k < 3; ++k) o[k] = int(c[i][k] + u * (c[i + 1][k] - c[i][k])); return qRgb(o[0], o[1], o[2]);
}
}  // namespace
void RenderPlot::setSignal(const std::vector<fireside::tx::cf>& iq, double fs) {
    constexpr size_t kFft = 2048, kMaxColumns = 1400; const size_t limit = size_t(120.0 * fs);          // only the first two minutes are drawn
    const size_t n = std::min(iq.size(), limit); if (n < kFft) { clear(); return; }
    fireside::dsp::SpectrumAnalyzer sa(fs, kFft, 1.0f); std::vector<std::vector<float>> frames; sa.on_frame = [&](const std::vector<float>& d) { frames.push_back(d); };
    sa.push(iq.data(), n); if (frames.empty()) { clear(); return; }
    const size_t lo = sa.bin_for_hz(-3000), hi = sa.bin_for_hz(3000), rows = hi - lo + 1, group = (frames.size() + kMaxColumns - 1) / kMaxColumns, cols = (frames.size() + group - 1) / group;
    float peak = -300.f; for (const auto& f : frames) for (size_t k = lo; k <= hi; ++k) peak = std::max(peak, f[k]);
    QImage im(int(cols), int(rows), QImage::Format_RGB32);
    for (size_t c = 0; c < cols; ++c) for (size_t r = 0; r < rows; ++r) {
        float m = -300.f; for (size_t g = c * group; g < std::min(frames.size(), (c + 1) * group); ++g) m = std::max(m, frames[g][lo + r]);
        reinterpret_cast<QRgb*>(im.scanLine(int(rows - 1 - r)))[c] = heat((m - (peak - 70.f)) / 70.f);       // top row = +3 kHz
    }
    img_ = im; seconds_ = double(n) / fs; update();
}
void RenderPlot::paintEvent(QPaintEvent*) {
    QPainter p(this); p.fillRect(rect(), QColor("#0b0e14")); const QRect r(52, 6, width() - 60, height() - 28);
    if (img_.isNull()) { p.setPen(QColor(theme::kMuted)); p.drawText(rect(), Qt::AlignCenter, "Render something to see it here"); return; }
    p.setRenderHint(QPainter::SmoothPixmapTransform); p.drawImage(r, img_); p.setRenderHint(QPainter::SmoothPixmapTransform, false);
    p.setPen(QColor(theme::kBorder)); p.drawRect(r); QFont f = p.font(); f.setPointSizeF(f.pointSizeF() * 0.9); p.setFont(f);
    for (int hz = -3000; hz <= 3000; hz += 1000) { const int y = r.bottom() - int((hz + 3000) / 6000.0 * r.height());
        p.setPen(hz == 0 ? QColor(theme::kAccent) : QColor(255, 255, 255, 40)); if (hz == 0) { p.setPen(QPen(QColor(theme::kAccent), 1, Qt::DashLine)); } p.drawLine(r.left(), y, r.right(), y);
        p.setPen(QColor(theme::kMuted)); p.drawText(QRect(0, y - 8, 46, 16), Qt::AlignRight | Qt::AlignVCenter, hz == 0 ? QStringLiteral("dial") : QStringLiteral("%1 kHz").arg(hz / 1000)); }
    const double step = seconds_ > 60 ? 20 : seconds_ > 20 ? 5 : seconds_ > 8 ? 2 : seconds_ > 3 ? 1 : 0.5;
    for (double t = 0; t <= seconds_ + 1e-9; t += step) { const int x = r.left() + int(t / seconds_ * r.width()); p.setPen(QColor(theme::kMuted)); p.drawLine(x, r.bottom(), x, r.bottom() + 3);
        p.drawText(QRect(x - 24, r.bottom() + 4, 48, 14), Qt::AlignHCenter | Qt::AlignTop, QStringLiteral("%1 s").arg(t, 0, 'g', 3)); }
}

PaddlePad::PaddlePad(QWidget* parent) : QFrame(parent) { setFocusPolicy(Qt::StrongFocus); setMinimumHeight(96); setFrameShape(QFrame::NoFrame); setToolTip("Click here, then use Z (dit) and X (dah), or comma and period, or the left and right arrows. Squeeze both for iambic keying."); }
void PaddlePad::paddle(bool dit, bool dah, double t_s) {
    if (full()) return; if (dit == dit_ && dah == dah_) return;
    if (!events_.empty() && t_s < events_.back().t_s) t_s = events_.back().t_s;
    dit_ = dit; dah_ = dah; events_.push_back({t_s, dit, dah}); update(); if (onChange) onChange();
}
void PaddlePad::press(bool dit, bool dah) { if (events_.empty()) { if (!dit && !dah) return; clock_.start(); } paddle(dit, dah, double(clock_.nsecsElapsed()) / 1e9); }
void PaddlePad::clear() { events_.clear(); dit_ = dah_ = false; update(); if (onChange) onChange(); }
void PaddlePad::key(QKeyEvent* e, bool down) {
    if (e->isAutoRepeat()) { e->accept(); return; }
    const int k = e->key(); const bool d = k == Qt::Key_Z || k == Qt::Key_Comma || k == Qt::Key_Left, h = k == Qt::Key_X || k == Qt::Key_Period || k == Qt::Key_Right;
    if (!d && !h) { e->ignore(); return; }
    press(d ? down : dit_, h ? down : dah_); e->accept();
}
void PaddlePad::keyPressEvent(QKeyEvent* e) { key(e, true); }
void PaddlePad::keyReleaseEvent(QKeyEvent* e) { key(e, false); }
void PaddlePad::paintEvent(QPaintEvent* e) {
    QFrame::paintEvent(e); QPainter p(this); p.setRenderHint(QPainter::Antialiasing); p.fillRect(rect().adjusted(1, 1, -1, -1), QColor(theme::kControl)); if (hasFocus()) { p.setPen(QPen(QColor(theme::kAccent), 2)); p.setBrush(Qt::NoBrush); p.drawRoundedRect(rect().adjusted(1, 1, -1, -1), 6, 6); }
    const QRect r = rect().adjusted(10, 10, -10, -28); const int w = r.width() / 2 - 6;
    for (int i = 0; i < 2; ++i) { const bool on = i == 0 ? dit_ : dah_; const QRect b(r.left() + i * (w + 12), r.top(), w, r.height());
        p.setPen(QPen(on ? QColor(theme::kWarn) : palette().mid().color(), 2)); p.setBrush(on ? QBrush(QColor(theme::kWarn).darker(160)) : QBrush(Qt::NoBrush)); p.drawRoundedRect(b, 8, 8);
        p.setPen(palette().text().color()); p.drawText(b, Qt::AlignCenter, i == 0 ? "dit  (Z)" : "dah  (X)"); }
    if (!hasFocus()) { p.setPen(QColor(theme::kMuted)); p.drawText(rect().adjusted(0, 0, 0, -6), Qt::AlignBottom | Qt::AlignHCenter, "click here to use the keyboard"); } else { p.setPen(QColor(theme::kMuted)); p.drawText(rect().adjusted(0, 0, 0, -6), Qt::AlignBottom | Qt::AlignHCenter, "Z = dit    X = dah    (comma / period and the arrow keys also work)"); }
}

TxPanel::TxPanel(QWidget* parent) : QWidget(parent) {
    auto* root = new QVBoxLayout(this); root->setContentsMargins(0, 0, 0, 0); root->setSpacing(12);

    auto* top = new QHBoxLayout; top->setSpacing(12);
    { auto* g = new QGridLayout; g->setHorizontalSpacing(10); g->setVerticalSpacing(8);
      mode_ = new QComboBox; for (const char* n : {"FT8", "FT4", "CW", "Voice (from a WAV file)", "Two-tone test", "PSK31", "RTTY", "CW keyer practice"}) mode_->addItem(n);
      side_ = new QComboBox; side_->addItem("USB"); side_->addItem("LSB");
      g->addWidget(new QLabel("Mode"), 0, 0); g->addWidget(mode_, 0, 1); g->addWidget(new QLabel("Sideband"), 1, 0); g->addWidget(side_, 1, 1);
      auto* warn = new QLabel("Offline: renders a file. Not connected to the radio; no transmit key, no sound, no device."); warn->setWordWrap(true); warn->setStyleSheet(QString("color: %1;").arg(theme::kWarn)); g->addWidget(warn, 2, 0, 1, 2);
      g->addWidget(muted("Baseband I/Q, dial at 0 Hz: USB above it, LSB below. Limited to -1 dBFS."), 3, 0, 1, 2); g->setColumnStretch(1, 1);
      top->addWidget(theme::makeCard("Transmit", g), 3); }

    pages_ = new QStackedWidget;
    { auto* w = new QWidget; auto* g = new QGridLayout(w); g->setContentsMargins(0, 0, 0, 0); g->setHorizontalSpacing(10); g->setVerticalSpacing(8);   // FT8 / FT4
      msg_ = new QLineEdit("CQ N0CALL AA00");   // the offline lab only renders files: an example call, not anyone's
       msg_->setMaxLength(40); hz_ = dspin(200, 3000, 1000, 10, 0, " Hz"); ftLevel_ = dspin(-40, -1, -6, 1, 0, " dBFS"); parity_ = new QComboBox; parity_->addItem("Even slots (:00 :30)"); parity_->addItem("Odd slots (:15 :45)");
      encodable_ = new QLabel; slot_ = new QLabel; slot_->setObjectName("muted");
      g->addWidget(new QLabel("Message"), 0, 0); g->addWidget(msg_, 0, 1, 1, 3); g->addWidget(encodable_, 1, 1, 1, 3);
      g->addWidget(new QLabel("Audio frequency"), 2, 0); g->addWidget(hz_, 2, 1); g->addWidget(new QLabel("Level"), 2, 2); g->addWidget(ftLevel_, 2, 3);
      g->addWidget(new QLabel("Slot"), 3, 0); g->addWidget(parity_, 3, 1, 1, 3); g->addWidget(slot_, 4, 0, 1, 4); g->setColumnStretch(1, 1); pages_->addWidget(w); }
    { auto* w = new QWidget; auto* g = new QGridLayout(w); g->setContentsMargins(0, 0, 0, 0); g->setHorizontalSpacing(10); g->setVerticalSpacing(8);   // CW
      cwText_ = new QLineEdit("CQ DE M0OML"); wpm_ = dspin(5, 60, 20, 1, 0, " wpm"); edge_ = dspin(1, 20, 5, 1, 0, " ms"); cwLevel_ = dspin(-40, -1, -6, 1, 0, " dBFS"); cwOffset_ = dspin(-3000, 3000, 0, 10, 0, " Hz");
      g->addWidget(new QLabel("Text"), 0, 0); g->addWidget(cwText_, 0, 1, 1, 3);
      g->addWidget(new QLabel("Speed"), 1, 0); g->addWidget(wpm_, 1, 1); g->addWidget(new QLabel("Edge shaping"), 1, 2); g->addWidget(edge_, 1, 3);
      g->addWidget(new QLabel("Level"), 2, 0); g->addWidget(cwLevel_, 2, 1); g->addWidget(new QLabel("Offset"), 2, 2); g->addWidget(cwOffset_, 2, 3);
      { auto* mr = new QHBoxLayout; mr->setSpacing(6); mr->addWidget(new QLabel("Memories")); for (int i = 0; i < kMemories; ++i) { memButtons_[i] = new QPushButton(QString("M%1").arg(i + 1)); const int slot = i; connect(memButtons_[i], &QPushButton::clicked, this, [this, slot] { recallMemory(slot); }); mr->addWidget(memButtons_[i]); }
        memSlot_ = new QComboBox; for (int i = 0; i < kMemories; ++i) memSlot_->addItem(QString("M%1").arg(i + 1)); auto* st = new QPushButton("Store text in"); connect(st, &QPushButton::clicked, this, &TxPanel::onStorePressed); mr->addSpacing(12); mr->addWidget(st); mr->addWidget(memSlot_); mr->addStretch(1); g->addLayout(mr, 4, 0, 1, 4); }
      g->addWidget(muted("Offset 0 puts the carrier on the dial; the receiver's CW mode makes the beat note. Edge shaping of 5 ms keeps key clicks about 60 dB down."), 3, 0, 1, 4); g->setColumnStretch(1, 1); pages_->addWidget(w); }
    { auto* w = new QWidget; auto* g = new QGridLayout(w); g->setContentsMargins(0, 0, 0, 0); g->setHorizontalSpacing(10); g->setVerticalSpacing(8);   // voice
      voiceFile_ = new QLineEdit; voiceFile_->setPlaceholderText("a WAV file with speech (any rate, channel 0 is used)"); auto* br = new QPushButton("Browse..."); gain_ = dspin(-40, 40, 0, 1, 0, " dB"); micChain_ = new QCheckBox("Mic chain (filters, compressor, limiter)"); micChain_->setChecked(true);
      g->addWidget(new QLabel("Audio file"), 0, 0); g->addWidget(voiceFile_, 0, 1); g->addWidget(br, 0, 2); g->addWidget(new QLabel("Mic gain"), 1, 0); g->addWidget(gain_, 1, 1); g->addWidget(micChain_, 2, 1, 1, 2); g->setColumnStretch(1, 1);
      connect(br, &QPushButton::clicked, this, &TxPanel::onBrowseVoice); pages_->addWidget(w); }
    { auto* w = new QWidget; auto* g = new QGridLayout(w); g->setContentsMargins(0, 0, 0, 0); g->setHorizontalSpacing(10); g->setVerticalSpacing(8);   // two-tone
      ttSeconds_ = dspin(1, 60, 4, 1, 0, " s"); ttPeak_ = dspin(-40, -1, -6, 1, 0, " dBFS");
      g->addWidget(new QLabel("Length"), 0, 0); g->addWidget(ttSeconds_, 0, 1); g->addWidget(new QLabel("Peak"), 1, 0); g->addWidget(ttPeak_, 1, 1);
      g->addWidget(muted("Two equal tones (600 and 1500 Hz). The result shows the measured third- and fifth-order distortion in dB below the tones."), 2, 0, 1, 2); g->setColumnStretch(1, 1); pages_->addWidget(w); }
    { auto* w = new QWidget; auto* g = new QGridLayout(w); g->setContentsMargins(0, 0, 0, 0); g->setHorizontalSpacing(10); g->setVerticalSpacing(8);   // PSK31 / RTTY
      digText_ = new QLineEdit("CQ CQ DE M0OML"); digText_->setMaxLength(500); digHz_ = dspin(200, 3000, 1000, 10, 0, " Hz"); digLevel_ = dspin(-40, -1, -6, 1, 0, " dBFS"); digHzLabel_ = new QLabel("Audio frequency");
      g->addWidget(new QLabel("Text"), 0, 0); g->addWidget(digText_, 0, 1, 1, 3); g->addWidget(digHzLabel_, 1, 0); g->addWidget(digHz_, 1, 1); g->addWidget(new QLabel("Level"), 1, 2); g->addWidget(digLevel_, 1, 3);
      g->addWidget(muted("PSK31: 31.25 baud BPSK with a 2 s lead-in. RTTY: 45.45 baud, 170 Hz shift, ITA2, the tone is the mark; the space is 170 Hz above it."), 2, 0, 1, 4); g->setColumnStretch(1, 1); pages_->addWidget(w); }
    { auto* w = new QWidget; auto* g = new QGridLayout(w); g->setContentsMargins(0, 0, 0, 0); g->setHorizontalSpacing(10); g->setVerticalSpacing(8);   // keyer practice
      pad_ = new PaddlePad; keyWpm_ = dspin(5, 60, 20, 1, 0, " wpm"); keyWeight_ = dspin(25, 75, 50, 1, 0, " %"); keyMode_ = new QComboBox; keyMode_->addItem("Iambic B"); keyMode_->addItem("Iambic A"); keyReverse_ = new QCheckBox("Swap paddles"); padInfo_ = muted("Nothing recorded.");
      auto* clr = new QPushButton("Clear recording"); connect(clr, &QPushButton::clicked, this, [this] { pad_->clear(); });
      g->addWidget(pad_, 0, 0, 1, 4); g->addWidget(new QLabel("Speed"), 1, 0); g->addWidget(keyWpm_, 1, 1); g->addWidget(new QLabel("Weight"), 1, 2); g->addWidget(keyWeight_, 1, 3);
      g->addWidget(new QLabel("Keyer"), 2, 0); g->addWidget(keyMode_, 2, 1); g->addWidget(keyReverse_, 2, 2); g->addWidget(clr, 2, 3); g->addWidget(padInfo_, 3, 0, 1, 4);
      g->addWidget(muted("Practice only: play the paddles, press Render, then Preview on the receiver to hear it as a beat note (the receiver's own audio output plays it, this tab makes no sound). Level, edge shaping and offset are the CW page's."), 4, 0, 1, 4); g->setColumnStretch(1, 1); pages_->addWidget(w);
      pad_->onChange = [this] { onPadChanged(); }; }
    for (int k = 0; k < pages_->count(); ++k) if (auto* gl = qobject_cast<QGridLayout*>(pages_->widget(k)->layout())) for (int i = 0; i < gl->count(); ++i) if (auto* wd = gl->itemAt(i)->widget()) if (qobject_cast<QDoubleSpinBox*>(wd) || qobject_cast<QPushButton*>(wd) || wd == keyMode_) gl->setAlignment(wd, Qt::AlignLeft);   // fixed-width boxes sit at the left of their column
    for (int k = 0; k < pages_->count(); ++k) if (auto* gl = qobject_cast<QGridLayout*>(pages_->widget(k)->layout())) { gl->setColumnStretch(3, 1); gl->setRowStretch(gl->rowCount(), 1); }   // two even columns, spare height at the bottom (not between rows)
    { auto* v = new QVBoxLayout; v->addWidget(pages_); top->addWidget(theme::makeCard("Settings", v), 6); }

    { auto* v = new QVBoxLayout; v->setSpacing(8); auto* row = new QHBoxLayout; render_ = new QPushButton("Render"); render_->setObjectName("primary"); save_ = new QPushButton("Save WAV..."); preview_ = new QPushButton("Preview on the receiver");
      save_->setEnabled(false); preview_->setEnabled(false); row->addWidget(render_); row->addWidget(save_); row->addWidget(preview_); row->addStretch(1); v->addLayout(row);
      result_ = new QLabel("Nothing rendered yet."); result_->setWordWrap(true); imd_ = new QLabel; imd_->setObjectName("muted"); imd_->setWordWrap(true); v->addWidget(result_); v->addWidget(imd_);
      plot_ = new RenderPlot; v->addWidget(plot_, 1);
      top->addWidget(theme::makeCard("Result", v, true), 7); }
    root->addLayout(top);

    clock_ = new QTimer(this); clock_->setInterval(250);
    connect(mode_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &TxPanel::onModeChanged);
    connect(msg_, &QLineEdit::textChanged, this, &TxPanel::updateEncodable); connect(parity_, QOverload<int>::of(&QComboBox::currentIndexChanged), this, &TxPanel::updateSlot);
    connect(clock_, &QTimer::timeout, this, &TxPanel::updateSlot);
    connect(render_, &QPushButton::clicked, this, [this] { render(); }); connect(save_, &QPushButton::clicked, this, &TxPanel::onSavePressed); connect(preview_, &QPushButton::clicked, this, &TxPanel::onPreviewPressed);
    memories_ = QStringList{"CQ DE M0OML", "M0OML", "R R 5NN TU", "QRZ?", "AGN PSE", "73 TU E E"};
    if (!qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) { QSettings q("fireside", "txmemory"); for (int i = 0; i < kMemories; ++i) if (q.contains(QString("m%1").arg(i))) memories_[i] = q.value(QString("m%1").arg(i)).toString().left(200); }
    refreshMemoryButtons(); updateEncodable(); onModeChanged(0);
}
TxPanel::~TxPanel() { if (worker_.joinable()) worker_.join(); }

void TxPanel::setMode(Mode m) { mode_->setCurrentIndex(int(m)); }
TxPanel::Mode TxPanel::mode() const { return Mode(mode_->currentIndex()); }
void TxPanel::setMessage(const QString& t) { msg_->setText(t); }
void TxPanel::setAudioHz(double hz) { hz_->setValue(hz); }
void TxPanel::setSidebandLsb(bool lsb) { side_->setCurrentIndex(lsb ? 1 : 0); }
void TxPanel::setCwText(const QString& t) { cwText_->setText(t); }
QString TxPanel::memoryText(int slot) const { return slot >= 0 && slot < kMemories ? memories_[slot] : QString(); }
bool TxPanel::setMemory(int slot, const QString& text) { if (slot < 0 || slot >= kMemories) return false; memories_[slot] = text.left(200); saveMemories(); refreshMemoryButtons(); return true; }
bool TxPanel::recallMemory(int slot) { if (memoryText(slot).isEmpty()) return false; cwText_->setText(memories_[slot]); return true; }
bool TxPanel::storeMemory(int slot) { return setMemory(slot, cwText_->text()); }
void TxPanel::setKeyerOptions(double wpm, bool modeB, bool reverse) { keyWpm_->setValue(wpm); keyMode_->setCurrentIndex(modeB ? 0 : 1); keyReverse_->setChecked(reverse); }
void TxPanel::onPadChanged() { const auto& e = pad_->events(); padInfo_->setText(e.empty() ? QStringLiteral("Nothing recorded.") : QStringLiteral("%1 paddle changes over %2 s%3.").arg(e.size()).arg(e.back().t_s, 0, 'f', 1).arg(pad_->full() ? QStringLiteral(" (recording is full)") : QString())); }
void TxPanel::onStorePressed() { storeMemory(memSlot_->currentIndex()); }
void TxPanel::saveMemories() { if (qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS")) return; QSettings q("fireside", "txmemory"); for (int i = 0; i < kMemories; ++i) q.setValue(QString("m%1").arg(i), memories_[i]); }
void TxPanel::refreshMemoryButtons() { for (int i = 0; i < kMemories; ++i) { memButtons_[i]->setToolTip(memories_[i].isEmpty() ? QStringLiteral("empty") : memories_[i]); memButtons_[i]->setEnabled(!memories_[i].isEmpty()); } }
void TxPanel::setDigitalText(const QString& t) { digText_->setText(t); }
void TxPanel::setDigitalHz(double hz) { digHz_->setValue(hz); }
void TxPanel::setVoiceFile(const QString& p) { voiceFile_->setText(p); }
QString TxPanel::summary() const { return summary_; }
QString TxPanel::imdText() const { return imd_text_; }
QString TxPanel::encodableText() const { return encodable_->text(); }
QString TxPanel::slotText() const { return slot_->text(); }
int TxPanel::decoderIndexFor(Mode m) { return m == Ft8 ? 4 : m == Ft4 ? 5 : m == Cw ? 1 : m == Psk31 ? 2 : m == Rtty ? 3 : m == Keyer ? 1 : 0; }
int TxPanel::previewToneHz() const { return (renderedMode_ == Psk31 || renderedMode_ == Rtty) ? int(std::lround(renderedHz_)) : 0; }

void TxPanel::onModeChanged(int i) {
    static const int kPage[] = {0, 0, 1, 2, 3, 4, 4, 5};                         // FT8 and FT4 share a page, PSK31 and RTTY share one
    pages_->setCurrentIndex(kPage[std::clamp(i, 0, 7)]);
    for (int k = 0; k < pages_->count(); ++k) pages_->widget(k)->setSizePolicy(QSizePolicy::Preferred, k == pages_->currentIndex() ? QSizePolicy::Preferred : QSizePolicy::Ignored);   // the tallest page must not set the height of the others
    if (i == Psk31) { digHzLabel_->setText("Audio frequency"); digHz_->setValue(1000); } else if (i == Rtty) { digHzLabel_->setText("Mark tone"); digHz_->setValue(2125); }
    if (i < 2) { clock_->start(); updateSlot(); updateEncodable(); } else clock_->stop();
}
void TxPanel::updateEncodable() {
    const bool ok = ft_message_ok(msg_->text().toStdString());
    encodable_->setText(ok ? "This message can be sent." : "This message cannot be encoded (FT8/FT4 messages are 13 characters of free text or a standard exchange).");
    encodable_->setStyleSheet(ok ? QString() : QString("color: %1;").arg(theme::kWarn));
}
void TxPanel::updateSlot() {
    const DigiMode dm = mode() == Ft4 ? DigiMode::FT4 : DigiMode::FT8; const double now = double(QDateTime::currentMSecsSinceEpoch()) / 1000.0;
    const Parity want = parity_->currentIndex() == 0 ? Parity::Even : Parity::Odd; const TxPlan p = plan_tx(now, dm, want, default_start_offset_s(dm));
    const Slot cur = slot_containing(now, dm);
    const QString curName = cur.parity == Parity::Even ? QStringLiteral("even") : QStringLiteral("odd"), wantName = want == Parity::Even ? QStringLiteral("even") : QStringLiteral("odd");
    slot_->setText(QStringLiteral("UTC %1   current slot: %2   next %3 slot starts in %4 s (the signal would begin 0.5 s later)").arg(QDateTime::currentDateTimeUtc().toString("HH:mm:ss"), curName, wantName).arg(p.slot.start_utc - now, 0, 'f', 1));
}
void TxPanel::onBrowseVoice() { const QString p = QFileDialog::getOpenFileName(this, "Choose a WAV file", QString(), "WAV files (*.wav)"); if (!p.isEmpty()) voiceFile_->setText(p); }

void TxPanel::setResultText(const QString& s, bool warn) { summary_ = s; result_->setText(s); result_->setStyleSheet(warn ? QString("color: %1;").arg(theme::kWarn) : QString()); }

bool TxPanel::render() {
    if (busy_) return false;
    if (worker_.joinable()) worker_.join();
    busy_ = true; render_->setEnabled(false); save_->setEnabled(false); preview_->setEnabled(false); setResultText("Rendering...", false); imd_text_.clear(); imd_->clear(); plot_->clear();
    const Mode m = mode(); const Sideband sb = side_->currentIndex() == 1 ? Sideband::LSB : Sideband::USB; renderedMode_ = m; renderedSide_ = sb;
    const std::string text = msg_->text().toStdString(), cw = cwText_->text().toStdString(), file = voiceFile_->text().toStdString();
    const double hz = hz_->value(), ftLevel = ftLevel_->value(), wpm = wpm_->value(), edge = edge_->value(), cwLevel = cwLevel_->value(), off = cwOffset_->value(), gain = gain_->value(), tts = ttSeconds_->value(), ttp = ttPeak_->value();
    const std::string dtext = digText_->text().toStdString(); const double dhz = digHz_->value(), dlevel = digLevel_->value(); renderedHz_ = dhz;
    const std::vector<PaddleEvent> paddles = pad_->events(); const double kwpm = keyWpm_->value(), kweight = keyWeight_->value(); const IambicMode kmode = keyMode_->currentIndex() == 0 ? IambicMode::B : IambicMode::A; const bool krev = keyReverse_->isChecked();
    const bool mic = micChain_->isChecked();
    worker_ = std::thread([=] {
        auto job = std::make_shared<Job>();
        try {
            switch (m) {
                case Ft8: case Ft4: job->iq = render_ft(text, hz, m == Ft4, sb, ftLevel); break;
                case Cw: job->iq = render_cw(cw, wpm, edge, cwLevel, off); break;
                case Voice: if (file.empty()) throw std::runtime_error("choose an audio file first"); job->iq = render_ssb(read_audio_wav(file), sb, mic, gain); break;
                case Psk31: job->iq = render_psk31(dtext, dhz, sb, dlevel); break;
                case Rtty: job->iq = render_rtty(dtext, dhz, sb, dlevel); break;
                case Keyer: if (paddles.empty()) throw std::runtime_error("record some paddle input first"); job->iq = render_paddles(paddles, kwpm, kmode, krev, kweight, edge, cwLevel, off); break;
                case TwoTone: job->iq = render_two_tone(tts, ttp, sb); break;
            }
        } catch (const std::exception& e) { job->error = QString::fromUtf8(e.what()); }
        QMetaObject::invokeMethod(this, [this, job] { finishJob(job); }, Qt::QueuedConnection);
    });
    return true;
}
void TxPanel::finishJob(const std::shared_ptr<Job>& j) {
    busy_ = false; render_->setEnabled(true);
    if (!j->error.isEmpty()) { iq_.clear(); setResultText("Could not render: " + j->error, true); emit rendered(); return; }
    iq_ = j->iq; const double pk = peak_magnitude(iq_); plot_->setSignal(iq_, kRenderRate);
    const QString sideName = renderedSide_ == Sideband::USB ? QStringLiteral("USB") : QStringLiteral("LSB");
    setResultText(QStringLiteral("%1 s, %2 samples, peak %3 dBFS (limited to -1 dBFS), %4. Baseband I/Q at 48 kHz, dial at 0 Hz.").arg(double(iq_.size()) / kRenderRate, 0, 'f', 2).arg(iq_.size()).arg(dbfs(pk), sideName), false);
    if (renderedMode_ == TwoTone) {
        try { const ImdResult r = measure_imd(iq_, renderedSide_); imd_text_ = QString("Distortion, dB below the tones: third order %1 dB, fifth order %2 dB. Products outside 150-2800 Hz are not counted.").arg(r.im3_dbc(), 0, 'f', 0).arg(r.im5_dbc(), 0, 'f', 0); }
        catch (const std::exception& e) { imd_text_ = QString("Distortion not measured: ") + e.what(); }
        imd_->setText(imd_text_);
    }
    save_->setEnabled(true); preview_->setEnabled(true); emit rendered();
}
bool TxPanel::saveWav(const QString& path) {
    if (iq_.empty()) return false;
    try { write_iq_wav(path.toStdString(), iq_); } catch (const std::exception& e) { setResultText(QString("Could not save: ") + e.what(), true); return false; }
    return true;
}
QString TxPanel::previewPath() {
    if (iq_.empty()) return QString();
    if (!tmp_) tmp_ = std::make_unique<QTemporaryDir>();
    if (!tmp_->isValid()) return QString();
    const QString p = tmp_->filePath("tx_preview.wav"); return saveWav(p) ? p : QString();
}
void TxPanel::onSavePressed() {
    const QString p = QFileDialog::getSaveFileName(this, "Save rendered I/Q", QDir::homePath() + "/tx_render.wav", "WAV files (*.wav)");
    if (!p.isEmpty() && saveWav(p)) setResultText(summary_ + QString("  Saved to %1.").arg(p), false);
}
void TxPanel::onPreviewPressed() { const QString p = previewPath(); if (!p.isEmpty()) emit previewRequested(p, decoderIndexFor(renderedMode_), previewToneHz()); }
