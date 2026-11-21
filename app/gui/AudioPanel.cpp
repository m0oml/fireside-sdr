#include "AudioPanel.h"
#include "ReceiverPanel.h"
#include <QCheckBox>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>
#include <cmath>
#ifdef FIRESIDE_QT_AUDIO
#include <QAudioFormat>
#include <QAudioSource>
#include <QMediaDevices>
#endif
#include "RadioPanel.h"
#include "Theme.h"
#include "fireside/audio_fifo.h"
#include "fireside/fw_iso.h"

using namespace fireside;
namespace {
bool no_settings() { return qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS"); }
// The radio's mic socket: input ch7 of the duplex stream (same clock as the radio: no drift).
struct RadioMicIn : txc::IAudioIn {
    std::unique_ptr<AudioTap> tap; float gain;
    RadioMicIn(std::unique_ptr<AudioTap> t, double gain_db) : tap(std::move(t)), gain(float(std::pow(10.0, gain_db / 20))) {}
    size_t read(float* x, size_t n, double t) override { const size_t k = tap->read(x, n, t); for (size_t i = 0; i < k; ++i) x[i] *= gain; return k; }
    std::string describe() const override { return tap->describe(); }
};
#ifdef FIRESIDE_QT_AUDIO
// A PC capture device -> 48 kHz mono float through a drift-correcting FIFO (the PC's sound card runs on its own clock).
struct PcAudioIn : QObject, txc::IAudioIn {
    DriftFifo fifo; QAudioSource* src = nullptr; QIODevice* io = nullptr; QString name; float gain; int ch = 1; QAudioFormat::SampleFormat sf = QAudioFormat::Int16;
    PcAudioIn(const QAudioDevice& dev, double gain_db) : name(dev.description()), gain(float(std::pow(10.0, gain_db / 20))) {
        QAudioFormat f; f.setSampleRate(48000); f.setChannelCount(1); f.setSampleFormat(QAudioFormat::Int16);
        if (!dev.isFormatSupported(f)) { f.setChannelCount(2); if (!dev.isFormatSupported(f)) throw std::runtime_error("the device cannot capture 48 kHz 16-bit"); }
        ch = f.channelCount(); sf = f.sampleFormat();
        src = new QAudioSource(dev, f, this); src->setBufferSize(48000 * 2 * ch / 25);   // 40 ms
        io = src->start(); if (!io) throw std::runtime_error("cannot open " + name.toStdString());
        QObject::connect(io, &QIODevice::readyRead, this, [this] {
            const QByteArray b = io->readAll(); const auto* s = reinterpret_cast<const int16_t*>(b.constData()); const size_t frames = size_t(b.size()) / size_t(2 * ch);
            std::vector<float> m(frames); for (size_t i = 0; i < frames; ++i) { float v = 0; for (int c = 0; c < ch; ++c) v += float(s[i * size_t(ch) + size_t(c)]) / 32768.0f; m[i] = gain * v / float(ch); }
            fifo.push(m.data(), m.size());
        });
    }
    ~PcAudioIn() override { if (src) src->stop(); }
    size_t read(float* x, size_t n, double t) override { return fifo.read(x, n, t); }
    std::string describe() const override { return "PC: " + name.toStdString(); }
};
#endif
QDoubleSpinBox* dspin(double lo, double hi, double v, double step, const char* suffix, int dec = 0) { auto* s = new QDoubleSpinBox; s->setRange(lo, hi); s->setDecimals(dec); s->setSingleStep(step); s->setValue(v); s->setSuffix(suffix); return s; }
}  // namespace

AudioPanel::AudioPanel(RadioPanel* radio, QWidget* parent) : QWidget(parent), radio_(radio) {
    auto* outer = new QVBoxLayout(this); outer->setContentsMargins(16, 16, 16, 16); outer->setSpacing(12);
    auto* top = new QHBoxLayout; top->setSpacing(12);
    // receive output
    { auto* l = new QVBoxLayout; rx_box_ = l; auto* n = new QLabel("Where the receive audio plays. The radio's outputs need the direct stream (48 / 96 / 192)."); n->setObjectName("muted"); n->setWordWrap(true); l->addWidget(n); l->addStretch(1);
      top->addWidget(theme::makeCard("Receive audio", l), 2); }
    // transmit input
    { auto* g = new QGridLayout; g->setHorizontalSpacing(10); g->setVerticalSpacing(8);
      source_ = new QComboBox; source_->addItem("Radio: mic socket (front)", QString("radio"));
#ifdef FIRESIDE_QT_AUDIO
      for (const auto& d : QMediaDevices::audioInputs()) source_->addItem("PC: " + d.description(), d.id());
#endif
      source_->setToolTip("Transmit audio: the radio's front mic socket (arrives on the radio's input channel 7, needs the direct stream), or a PC capture device");
      mic_gain_ = new QSlider(Qt::Horizontal); mic_gain_->setRange(-40, 0); mic_gain_->setValue(0); mic_gain_lbl_ = new QLabel("0 dB");
      mic_gain_->setToolTip("The radio's mic input gain (its codec): 0 dB at most, as PowerSDR. Applied when the radio is started.");
      pc_gain_ = new QSpinBox; pc_gain_->setRange(-20, 30); pc_gain_->setValue(0); pc_gain_->setSuffix(" dB"); pc_gain_->setToolTip("Extra digital gain on the input before the processing");
      level_ = new QProgressBar; level_->setRange(0, 600); level_->setValue(0); level_->setFormat("input -");
      level_->setStyleSheet("QProgressBar{background:#151b24;border:1px solid #2a3442;border-radius:4px;color:#d8dee8;text-align:center;min-height:18px}"
                            "QProgressBar::chunk{background:#2e7d32;border-radius:3px}");
      test_ = new QPushButton("Test input"); test_->setCheckable(true); test_->setToolTip("Open the input and show its level (nothing transmits)");
      in_note_ = new QLabel(" "); in_note_->setObjectName("muted"); in_note_->setWordWrap(true);
      int r = 0;
      g->addWidget(new QLabel("Source"), r, 0); g->addWidget(source_, r++, 1, 1, 2);
      g->addWidget(new QLabel("Radio mic gain"), r, 0); g->addWidget(mic_gain_, r, 1); g->addWidget(mic_gain_lbl_, r++, 2);
      g->addWidget(new QLabel("Input gain"), r, 0); g->addWidget(pc_gain_, r++, 1);
      g->addWidget(test_, r, 0); g->addWidget(level_, r++, 1, 1, 2);
      g->addWidget(in_note_, r++, 0, 1, 3); g->setColumnStretch(1, 1);
      top->addWidget(theme::makeCard("Transmit input", g), 3); }
    outer->addLayout(top);
    // other programs: the virtual devices
    { auto* g = new QGridLayout; other_grid_ = g; g->setHorizontalSpacing(10); g->setVerticalSpacing(6); int r = 0;
      data_out_ = new QCheckBox("Send receive audio to \"Fireside RX audio\"");
      data_out_->setToolTip("A second copy of the receive audio, at a fixed level (not the Volume slider or Mute), on the virtual device other programs record from");
      data_level_ = new QSpinBox; data_level_->setRange(-40, 0); data_level_->setValue(-12); data_level_->setSuffix(" dB"); data_level_->setToolTip("Level of that copy");
      ext_note_ = new QLabel(" "); ext_note_->setObjectName("muted"); ext_note_->setWordWrap(true);
      g->addWidget(data_out_, r, 0); g->addWidget(new QLabel("level"), r, 1); g->addWidget(data_level_, r++, 2);
      g->addWidget(ext_note_, r++, 0, 1, 3); g->setColumnStretch(0, 1);
      connect(data_out_, &QCheckBox::toggled, this, [this] { applyDataOut(); save(); });
      connect(data_level_, qOverload<int>(&QSpinBox::valueChanged), this, [this] { applyDataOut(); save(); });
      outer->addWidget(theme::makeCard("Other programs (WSJT-X, JS8Call, fldigi ...)", g)); }
    // processing
    auto* mid = new QHBoxLayout; mid->setSpacing(12);
    { auto* g = new QGridLayout; g->setHorizontalSpacing(10); g->setVerticalSpacing(6); int r = 0;
      hp_ = new QSpinBox; hp_->setRange(50, 400); hp_->setSuffix(" Hz"); lp_ = new QSpinBox; lp_->setRange(2000, 4000); lp_->setSingleStep(100); lp_->setSuffix(" Hz");
      g->addWidget(new QLabel("High-pass"), r, 0); g->addWidget(hp_, r, 1); g->addWidget(new QLabel("Low-pass"), r, 2); g->addWidget(lp_, r++, 3);
      exp_on_ = new QCheckBox("Downward expander"); exp_thr_ = dspin(-70, -20, -45, 1, " dBFS"); exp_ratio_ = dspin(1, 10, 2.5, 0.5, " : 1", 1);
      exp_on_->setToolTip("Below the threshold the level falls by (ratio - 1) dB per dB: room noise and hum drop away between words, speech is untouched");
      g->addWidget(exp_on_, r, 0); g->addWidget(exp_thr_, r, 1); g->addWidget(new QLabel("ratio"), r, 2); g->addWidget(exp_ratio_, r++, 3);
      lev_on_ = new QCheckBox("Leveler"); lev_target_ = dspin(-30, -1, -6, 1, " dBFS"); lev_max_ = dspin(0, 30, 20, 1, " dB max");
      lev_on_->setToolTip("A slow automatic gain: a soft or a loud voice (or mic) reaches the compressor at the same level");
      g->addWidget(lev_on_, r, 0); g->addWidget(lev_target_, r, 1); g->addWidget(new QLabel("gain"), r, 2); g->addWidget(lev_max_, r++, 3);
      comp_on_ = new QCheckBox("Compressor"); comp_thr_ = dspin(-40, 0, -18, 1, " dBFS"); comp_ratio_ = dspin(1, 20, 3, 0.5, " : 1", 1); comp_makeup_ = dspin(0, 24, 12, 1, " dB make-up");
      comp_on_->setToolTip("Raises the average power: speech peaks then drive into the final limiter, so the power setting is the peak (PEP) power");
      g->addWidget(comp_on_, r, 0); g->addWidget(comp_thr_, r, 1); g->addWidget(comp_ratio_, r, 2); g->addWidget(comp_makeup_, r++, 3);
      auto* reset = new QPushButton("Defaults"); reset->setToolTip("PowerSDR-like voice processing"); g->addWidget(reset, r++, 0);
      connect(reset, &QPushButton::clicked, this, [this] { resetProcessing(); save(); });
      mid->addWidget(theme::makeCard("Processing (transmit)", g), 3); }
    { auto* v = new QVBoxLayout; eq_on_ = new QCheckBox("Equaliser on"); v->addWidget(eq_on_);
      auto* h = new QHBoxLayout; h->setSpacing(4);
      static const char* names[10] = {"32", "63", "125", "250", "500", "1k", "2k", "4k", "8k", "16k"};
      for (int i = 0; i < 10; ++i) {
          auto* c = new QVBoxLayout; c->setSpacing(2); eq_val_[i] = new QLabel("0"); eq_val_[i]->setAlignment(Qt::AlignHCenter);
          eq_[i] = new QSlider(Qt::Vertical); eq_[i]->setRange(-15, 15); eq_[i]->setMinimumHeight(140); eq_[i]->setToolTip(QString("%1 Hz, -15..+15 dB").arg(names[i]));
          auto* nl = new QLabel(names[i]); nl->setAlignment(Qt::AlignHCenter); nl->setObjectName("muted");
          c->addWidget(eq_val_[i]); c->addWidget(eq_[i], 1, Qt::AlignHCenter); c->addWidget(nl); h->addLayout(c);
          connect(eq_[i], &QSlider::valueChanged, this, [this, i](int val) { eq_val_[i]->setText(QString::number(val)); save(); });
      }
      v->addLayout(h); auto* flat = new QPushButton("Flat"); v->addWidget(flat, 0, Qt::AlignLeft);
      connect(flat, &QPushButton::clicked, this, [this] { for (auto* s : eq_) s->setValue(0); });
      mid->addWidget(theme::makeCard("10-band equaliser (transmit)", v), 4); }
    outer->addLayout(mid); outer->addStretch(1);
    timer_ = new QTimer(this); timer_->setInterval(100); connect(timer_, &QTimer::timeout, this, &AudioPanel::tick); timer_->start();
    connect(test_, &QPushButton::toggled, this, &AudioPanel::onTestInput);
    connect(mic_gain_, &QSlider::valueChanged, this, [this](int v) { mic_gain_lbl_->setText(QString("%1 dB").arg(v)); });
    connect(mic_gain_, &QSlider::sliderReleased, this, [this] { save(); applyMicGain(); });
    connect(source_, &QComboBox::activated, this, [this] { if (test_->isChecked()) { test_->setChecked(false); } save(); });
    for (auto* w : {hp_, lp_, pc_gain_}) connect(w, qOverload<int>(&QSpinBox::valueChanged), this, [this] { save(); });
    for (auto* w : {exp_thr_, exp_ratio_, lev_target_, lev_max_, comp_thr_, comp_ratio_, comp_makeup_}) connect(w, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { save(); });
    for (auto* w : {exp_on_, lev_on_, comp_on_, eq_on_}) connect(w, &QCheckBox::toggled, this, [this] { save(); });
    connect(radio_, &RadioPanel::runningChanged, this, [this](bool on) { if (on) QTimer::singleShot(500, this, [this] { applyMicGain(); }); });
    resetProcessing(); load();
}
AudioPanel::~AudioPanel() = default;

void AudioPanel::setReceiveOutputWidget(QWidget* w) { if (rx_box_ && w) static_cast<QVBoxLayout*>(rx_box_)->insertWidget(0, w); }
void AudioPanel::resetProcessing() {
    loading_ = true; const auto d = tx::voice_chain_defaults(); const auto& m = d.mic;
    hp_->setValue(int(m.highpass_hz)); lp_->setValue(int(m.lowpass_hz));
    exp_on_->setChecked(m.expander); exp_thr_->setValue(m.exp_cfg.threshold_dbfs); exp_ratio_->setValue(m.exp_cfg.ratio);
    lev_on_->setChecked(m.leveler); lev_target_->setValue(m.lev_cfg.target_dbfs); lev_max_->setValue(m.lev_cfg.max_gain_db);
    comp_on_->setChecked(m.compressor); comp_thr_->setValue(m.comp_cfg.threshold_dbfs); comp_ratio_->setValue(m.comp_cfg.ratio); comp_makeup_->setValue(m.comp_cfg.makeup_db);
    loading_ = false;
}
tx::TxChainConfig AudioPanel::chainConfig() const {
    auto c = tx::voice_chain_defaults(); auto& m = c.mic;
    m.mic_gain_db = double(pc_gain_->value());                         // the input gain, applied once: in the chain
    m.highpass_hz = hp_->value(); m.lowpass_hz = lp_->value();
    m.expander = exp_on_->isChecked(); m.exp_cfg.threshold_dbfs = exp_thr_->value(); m.exp_cfg.ratio = exp_ratio_->value();
    m.leveler = lev_on_->isChecked(); m.lev_cfg.target_dbfs = lev_target_->value(); m.lev_cfg.max_gain_db = lev_max_->value();
    m.compressor = comp_on_->isChecked(); m.comp_cfg.threshold_dbfs = comp_thr_->value(); m.comp_cfg.ratio = comp_ratio_->value(); m.comp_cfg.makeup_db = comp_makeup_->value();
    if (eq_on_->isChecked()) { double g[10]; for (int i = 0; i < 10; ++i) g[i] = eq_[i]->value(); m.eq = tx::graphic_eq10(g); }
    c.ssb.hi_hz = std::min(2900.0, double(lp_->value()) - 100);
    return c;
}
bool AudioPanel::radioMic() const { return source_->currentData().toString() == "radio"; }
QString AudioPanel::inputName() const { return source_->currentText(); }
std::unique_ptr<txc::IAudioIn> AudioPanel::openInput(QString* err) {
    if (test_factory_) return test_factory_();                          // tests: a generated voice
    try {
        if (radioMic()) return std::make_unique<RadioMicIn>(open_fw_iso_input(7), 0.0);
#ifdef FIRESIDE_QT_AUDIO
        for (const auto& d : QMediaDevices::audioInputs()) if (d.id() == source_->currentData().toByteArray()) return std::make_unique<PcAudioIn>(d, 0.0);
        if (err) *err = "that PC capture device is gone"; return nullptr;
#else
        if (err) *err = "built without Qt audio"; return nullptr;
#endif
    } catch (const std::exception& e) { if (err) *err = QString::fromUtf8(e.what()); return nullptr; }
}
void AudioPanel::setReceiver(ReceiverPanel* rx) { rx_ = rx; applyDataOut(); }
void AudioPanel::applyDataOut() {
    QString note;
    if (rx_) {
        if (!rx_->setDataOutput(data_out_->isChecked(), data_level_->value())) note = "\"Fireside RX audio\" not found: run system/setup-system.sh (it adds the two virtual devices), then restart this app. ";
        else if (data_out_->isChecked()) note = "Receive audio is going to \"Fireside RX audio\": in the other program record from \"Monitor of Fireside RX audio\". ";
    }
    note += externalInputExists() ? "Transmit audio from other programs: they play into \"Fireside TX audio\" (we record \"Fireside TX audio (input)\")."
                                  : "No \"Fireside TX audio\" device: other programs cannot send audio to transmit (run system/setup-system.sh).";
    ext_note_->setText(note);
}
bool AudioPanel::externalInputExists() {
#ifdef FIRESIDE_QT_AUDIO
    for (const auto& d : QMediaDevices::audioInputs()) if (d.description().contains("Fireside TX audio", Qt::CaseInsensitive)) return true;
#endif
    return false;
}
std::unique_ptr<txc::IAudioIn> AudioPanel::openExternalInput(QString* err) {
    if (ext_factory_) return ext_factory_();                            // tests
    try {
#ifdef FIRESIDE_QT_AUDIO
        for (const auto& d : QMediaDevices::audioInputs()) if (d.description().contains("Fireside TX audio", Qt::CaseInsensitive)) return std::make_unique<PcAudioIn>(d, 0.0);
        if (err) *err = "no \"Fireside TX audio\" device (run system/setup-system.sh)"; return nullptr;
#else
        if (err) *err = "built without Qt audio"; return nullptr;
#endif
    } catch (const std::exception& e) { if (err) *err = QString::fromUtf8(e.what()); return nullptr; }
}
void AudioPanel::applyMicGain() {
    if (!radioMic() || radio_->simulated()) return;                      // only the radio's mic socket uses it (the simulated radio has no codec)
    std::string e; if (radio_->session().set_mic_gain_db(mic_gain_->value(), &e)) emit message(QString("Audio: radio mic gain %1 dB").arg(mic_gain_->value()));
}
void AudioPanel::onTestInput(bool on) {
    test_in_.reset(); level_->setValue(0); level_->setFormat("input -");
    if (!on) { in_note_->setText(" "); return; }
    QString err; test_in_ = openInput(&err);
    if (!test_in_) { in_note_->setText("Cannot open the input: " + err); QSignalBlocker b(test_); test_->setChecked(false); return; }
    in_note_->setText(QString("Listening to %1 (nothing transmits)").arg(QString::fromStdString(test_in_->describe())));
}
void AudioPanel::tick() {
    if (!test_in_) return;
    std::vector<float> b(4800); float pk = 0;
    try { for (int i = 0; i < 4; ++i) { const size_t n = test_in_->read(b.data(), b.size(), 0.0); if (!n) break; for (size_t k = 0; k < n; ++k) pk = std::max(pk, std::fabs(b[k])); } }
    catch (const std::exception& e) { in_note_->setText(QString("Input stopped: ") + e.what()); test_in_.reset(); QSignalBlocker bl(test_); test_->setChecked(false); return; }
    pk *= float(std::pow(10.0, pc_gain_->value() / 20.0));                // as the chain sees it (after the input gain)
    test_peak_ = std::max(pk, test_peak_ * 0.8f);
    const double db = 20 * std::log10(test_peak_ + 1e-9); level_->setValue(int(std::clamp((db + 60) * 10, 0.0, 600.0))); level_->setFormat(QString("input %1 dBFS").arg(db, 0, 'f', 0));
}
void AudioPanel::load() {
    if (no_settings()) return;
    loading_ = true; QSettings q("fireside", "audio");
    const QString src = q.value("source", "radio").toString(); for (int i = 0; i < source_->count(); ++i) if (source_->itemData(i).toString() == src || source_->itemData(i).toByteArray() == src.toUtf8()) source_->setCurrentIndex(i);
    mic_gain_->setValue(q.value("mic_gain_db", 0).toInt()); pc_gain_->setValue(q.value("input_gain_db", 0).toInt());
    hp_->setValue(q.value("highpass", hp_->value()).toInt()); lp_->setValue(q.value("lowpass", lp_->value()).toInt());
    exp_on_->setChecked(q.value("exp_on", exp_on_->isChecked()).toBool()); exp_thr_->setValue(q.value("exp_thr", exp_thr_->value()).toDouble()); exp_ratio_->setValue(q.value("exp_ratio", exp_ratio_->value()).toDouble());
    lev_on_->setChecked(q.value("lev_on", lev_on_->isChecked()).toBool()); lev_target_->setValue(q.value("lev_target", lev_target_->value()).toDouble()); lev_max_->setValue(q.value("lev_max", lev_max_->value()).toDouble());
    comp_on_->setChecked(q.value("comp_on", comp_on_->isChecked()).toBool()); comp_thr_->setValue(q.value("comp_thr", comp_thr_->value()).toDouble()); comp_ratio_->setValue(q.value("comp_ratio", comp_ratio_->value()).toDouble()); comp_makeup_->setValue(q.value("comp_makeup", comp_makeup_->value()).toDouble());
    eq_on_->setChecked(q.value("eq_on", false).toBool()); for (int i = 0; i < 10; ++i) eq_[i]->setValue(q.value(QString("eq%1").arg(i), 0).toInt());
    data_out_->setChecked(q.value("data_out", false).toBool()); data_level_->setValue(q.value("data_level_db", -12).toInt());
    loading_ = false;
}
void AudioPanel::save() {
    if (loading_ || no_settings()) return;
    QSettings q("fireside", "audio");
    q.setValue("source", source_->currentData().toString().isEmpty() ? QString::fromUtf8(source_->currentData().toByteArray()) : source_->currentData().toString());
    q.setValue("mic_gain_db", mic_gain_->value()); q.setValue("input_gain_db", pc_gain_->value()); q.setValue("highpass", hp_->value()); q.setValue("lowpass", lp_->value());
    q.setValue("exp_on", exp_on_->isChecked()); q.setValue("exp_thr", exp_thr_->value()); q.setValue("exp_ratio", exp_ratio_->value());
    q.setValue("lev_on", lev_on_->isChecked()); q.setValue("lev_target", lev_target_->value()); q.setValue("lev_max", lev_max_->value());
    q.setValue("comp_on", comp_on_->isChecked()); q.setValue("comp_thr", comp_thr_->value()); q.setValue("comp_ratio", comp_ratio_->value()); q.setValue("comp_makeup", comp_makeup_->value());
    q.setValue("eq_on", eq_on_->isChecked()); for (int i = 0; i < 10; ++i) q.setValue(QString("eq%1").arg(i), eq_[i]->value());
    q.setValue("data_out", data_out_->isChecked()); q.setValue("data_level_db", data_level_->value());
}

void AudioPanel::addKeyingWidget(QWidget* w) {                      // "Let other programs key (CAT)" and its power, from the Transmit card: it sits with the other-program audio
    if (!other_grid_) return; auto* l = new QLabel("Keying by the other program (CAT)"); l->setObjectName("muted");
    const int r = other_grid_->rowCount(); other_grid_->addWidget(l, r, 0, 1, 3); other_grid_->addWidget(w, r + 1, 0, 1, 3);
}
