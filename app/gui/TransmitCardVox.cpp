// The Transmit card's VOX and monitor (under the voice controls). VOX listens to the Audio tab's input (with 150 ms of pre-roll, so the first
// syllable is sent) and starts a voice over (TxControl::voice, the same gate, limits and time-out as PTT) while you speak; it is off in every new
// window, never remembered, works only on the Voice screen with transmit ready, and switches itself off after a time-out or a safety stop.
#include "TransmitCard.h"
#include "AudioPanel.h"
#include "PcAudioFeed.h"
#include "ReceiverPanel.h"
#include "fireside/tx_meter.h"
#include <QCheckBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QSettings>
#include <QSpinBox>
#include <QTimer>
#include <QVBoxLayout>

using namespace fireside;
namespace { bool vox_no_settings() { return qEnvironmentVariableIsSet("FIRESIDE_NO_SETTINGS"); } }

void TransmitCard::buildVox() {
    vox_ = new QCheckBox("VOX"); vox_->setChecked(false);
    vox_->setToolTip("Your voice keys the transmitter (Voice screen, transmit ready). Off in every new window. Set the threshold so room noise stays below it; "
                     "switches itself off after a time-out or a safety stop");
    vox_thr_ = new QSpinBox; vox_thr_->setRange(-80, 0); vox_thr_->setValue(-30); vox_thr_->setSuffix(" dBFS"); vox_thr_->setPrefix("above ");
    vox_thr_->setToolTip("VOX keys when the mic (after the input gain) peaks above this. The level beside it shows where you are");
    vox_hang_ = new QSpinBox; vox_hang_->setRange(300, 3000); vox_hang_->setSingleStep(100); vox_hang_->setValue(800); vox_hang_->setSuffix(" ms"); vox_hang_->setPrefix("hang ");
    vox_hang_->setToolTip("How long VOX holds after you stop talking (the gaps between words must be shorter)");
    anti_vox_ = new QCheckBox("Anti-VOX"); anti_vox_->setToolTip("The receive audio from the speaker does not key VOX: the mic must be louder than the speaker (not needed with headphones)");
    vox_lvl_ = new QLabel; vox_lvl_->setMinimumWidth(120); vox_lvl_->setToolTip("The mic level as VOX hears it (green: speech, VOX keys)");
    monitor_ = new QCheckBox("Monitor"); monitor_->setToolTip("Hear your processed audio on the PC's speakers while you transmit, as a receiver would hear it (use headphones)");
    auto* h = new QHBoxLayout; h->setSpacing(8); for (QWidget* w : std::initializer_list<QWidget*>{vox_, vox_thr_, vox_hang_, anti_vox_, vox_lvl_, monitor_}) h->addWidget(w); h->addStretch(1);
    if (auto* vl = qobject_cast<QVBoxLayout*>(voice_part_->layout())) vl->addLayout(h);
    connect(vox_, &QCheckBox::toggled, this, &TransmitCard::voxOn);
    for (auto* b : {vox_thr_, vox_hang_}) connect(b, &QSpinBox::editingFinished, this, [this] { saveVox(); if (vox_in_) { voxOn(false); voxOn(true); } });   // a new setting: listen again with it
    connect(anti_vox_, &QCheckBox::toggled, this, [this] { saveVox(); if (vox_in_) { voxOn(false); voxOn(true); } });
    connect(monitor_, &QCheckBox::toggled, this, [this] { saveVox(); });
    vox_timer_ = new QTimer(this); vox_timer_->setInterval(20); connect(vox_timer_, &QTimer::timeout, this, &TransmitCard::voxTick); vox_timer_->start();
    loadVox();
}
void TransmitCard::loadVox() {
    if (vox_no_settings()) return;
    QSettings q("fireside", "transmit");
    vox_thr_->setValue(q.value("vox_threshold_dbfs", -30).toInt()); vox_hang_->setValue(q.value("vox_hang_ms", 800).toInt());
    anti_vox_->setChecked(q.value("anti_vox", false).toBool()); monitor_->setChecked(q.value("monitor", false).toBool());   // VOX itself: never remembered
}
void TransmitCard::saveVox() {
    if (vox_no_settings()) return;
    QSettings q("fireside", "transmit");
    q.setValue("vox_threshold_dbfs", vox_thr_->value()); q.setValue("vox_hang_ms", vox_hang_->value()); q.setValue("anti_vox", anti_vox_->isChecked()); q.setValue("monitor", monitor_->isChecked());
}
bool TransmitCard::voxScreen() const { return rx_->modeGroup() == 0 && tx_ && tx_->armed() && audio_; }
void TransmitCard::voxOn(bool on) {
    if (!on) {
        if (vox_in_) { if (!worker_running_) vox_in_->set_reading(false); vox_in_.reset(); report("VOX off."); }   // an over now running keeps its own reference and ends on the hang
        vox_lvl_->clear(); return;
    }
    auto refuse = [this](const QString& why) { report("VOX: " + why); QSignalBlocker b(vox_); vox_->setChecked(false); vox_lvl_->clear(); };
    if (!voxScreen()) return refuse("only on the Voice screen with transmit ready.");
    if (audio_->testInputOpen()) return refuse("the Audio tab's input test is running: stop it first.");
    if (worker_running_) return refuse("the transmitter is busy; tick it again after the over.");
    QString err; auto in = audio_->openInput(&err);
    if (!in) return refuse("cannot open the input (" + err + ").");
    vox::VoxConfig c; c.threshold_dbfs = vox_thr_->value(); c.hang_s = vox_hang_->value() / 1000.0; c.anti_vox = anti_vox_->isChecked(); c.anti_gain_db = 0;
    ReceiverPanel* rx = rx_;
    try { vox_in_ = std::make_shared<vox::VoxInput>(std::move(in), c, audio_->chainConfig().mic.mic_gain_db, 0.15, [rx] { return rx->speakerDbfs(); }); }
    catch (const std::exception& e) { return refuse(QString("cannot start (") + e.what() + ")."); }
    vox_kill_ = false;
    report(QString("VOX listening on %1: speech above %2 dBFS keys the transmitter (%3 W PEP), %4 ms hang.").arg(audio_->inputName()).arg(vox_thr_->value()).arg(voice_w_->value(), 0, 'f', 0).arg(vox_hang_->value()));
}
void TransmitCard::voxTick() {
    if (vox_kill_.exchange(false)) {                                    // set by an over that ended on a time-out / safety stop
        std::string why; { std::lock_guard<std::mutex> l(vox_m_); why = vox_kill_why_; }
        { QSignalBlocker b(vox_); vox_->setChecked(false); } vox_in_.reset(); vox_lvl_->clear();
        QTimer::singleShot(400, this, [this, why] { report(QString("VOX switched off: %1. Tick it again to carry on.").arg(QString::fromStdString(why))); });   // after the over's own report
        return;
    }
    if (!vox_in_) return;
    if (vox_in_->failed()) { const QString e = QString::fromStdString(vox_in_->error()); { QSignalBlocker b(vox_); vox_->setChecked(false); } vox_in_.reset(); vox_lvl_->clear(); report("VOX off: the input stopped (" + e + ")."); return; }
    if (!voxScreen() && !worker_running_) { { QSignalBlocker b(vox_); vox_->setChecked(false); } vox_in_.reset(); vox_lvl_->clear(); report("VOX off: left the Voice screen or transmit is no longer ready."); return; }
    const double l = vox_in_->level_dbfs(); const bool sp = vox_in_->on();
    vox_lvl_->setText(QString("<span style='color:%1'>mic %2 dBFS</span>").arg(sp ? "#3ddc84" : "#8b93a7").arg(std::max(-99.0, l), 0, 'f', 0));
    if (sp && !worker_running_ && !ft_pending_ && !cw_over_ && voxScreen() && txm::band_of(rx_->dialHz()) >= 0) startVoice();
}
