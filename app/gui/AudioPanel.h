#pragma once
// The Audio tab: where receive audio plays (the receiver's named output box lives here), where transmit audio comes from (the radio's mic
// socket = input ch7 of the duplex stream, or a PC capture device), the mic gain, an input level meter, and the voice processing
// (high / low-pass, downward expander, leveler, compressor, 10-band graphic EQ). Settings in QSettings "fireside/audio".
// It never keys anything: TransmitCard asks it for the chain settings and an opened input when PTT goes down.
#include <QWidget>
#include <functional>
#include <memory>
#include "fireside/tx_control.h"
#include "fireside/tx_dsp.h"
#include "fireside/vox.h"
#include "fireside/dsp.h"
class SpectrumWidget; class WaterfallWidget; class QCheckBox; class QGridLayout; class QComboBox; class QDoubleSpinBox; class QLabel; class QProgressBar; class QPushButton; class QSlider; class QSpinBox; class QTimer;
class RadioPanel; class ReceiverPanel;

class AudioPanel : public QWidget {
    Q_OBJECT
public:
    AudioPanel(RadioPanel* radio, QWidget* parent = nullptr);
    ~AudioPanel() override;
    void setReceiveOutputWidget(QWidget* w);                       // ReceiverPanel's output box
    fireside::tx::TxChainConfig chainConfig() const;                    // the voice processing as set here
    bool radioMic() const;                                          // true: the radio's mic socket; false: a PC capture device
    std::unique_ptr<fireside::txc::IAudioIn> openInput(QString* error); // the chosen input, opened (48 kHz mono); nullptr + error if it cannot be
    void applyMicGain();                                            // sends the radio's mic gain (when the radio is started)
    QString inputName() const;
    // The feedback test (no RF): "Test input" with "Hear it processed" ticked plays the mic through the transmit processing on the PC's speakers.
    QPushButton* testButton() const { return test_; } QCheckBox* hearBox() const { return hear_; } bool feedbackRunning() const { return loop_ != nullptr; }
    void setPcAudioEnabled(bool on) { pc_audio_ = on; }    // false (smoke tests): the feedback test never opens a sound device
    const std::vector<float>& pictureFrameForTests() const { return pic_last_; }   // the Audio tab's picture: the last spectrum frame (dB, 0 = the set PEP)
    double feedbackOutDbfs() const { return loop_ ? loop_->out_dbfs() : -200; }
    bool testInputOpen() const { return test_in_ != nullptr || loop_ != nullptr; }
    void setInputFactoryForTests(std::function<std::unique_ptr<fireside::txc::IAudioIn>()> f) { test_factory_ = std::move(f); }
    // Other programs (WSJT-X, fldigi ...): the virtual devices of system/53-fireside-virtual-audio.conf.
    void addKeyingWidget(QWidget* w);                              // the CAT keying tick and its power (the Transmit card owns them)
    void setReceiver(ReceiverPanel* rx);                            // where "Send receive audio to Fireside RX audio" goes
    QCheckBox* dataOutBox() const { return data_out_; }
    static bool externalInputExists();                              // "Fireside TX audio (input)" is there (the loopback's capture side)
    std::unique_ptr<fireside::txc::IAudioIn> openExternalInput(QString* error);   // what other programs play into Fireside TX audio (48 kHz mono)
    void setExternalInputFactoryForTests(std::function<std::unique_ptr<fireside::txc::IAudioIn>()> f) { ext_factory_ = std::move(f); }
signals:
    void message(const QString& text);
private slots:
    void onTestInput(bool on);
    void tick();
private:
    RadioPanel* radio_;
    QComboBox* source_; QSlider* mic_gain_; QLabel* mic_gain_lbl_; QSpinBox* pc_gain_; QProgressBar* level_; QPushButton* test_; QLabel* in_note_;
    QSpinBox *hp_, *lp_; QCheckBox *exp_on_, *lev_on_, *comp_on_, *eq_on_; QDoubleSpinBox *exp_thr_, *exp_ratio_, *lev_target_, *lev_max_, *comp_thr_, *comp_ratio_, *comp_makeup_;
    QSlider* eq_[10]; QLabel* eq_val_[10]; QLayout* rx_box_ = nullptr;
    std::shared_ptr<fireside::txo::IqTap> pic_tap_ = std::make_shared<fireside::txo::IqTap>(); std::unique_ptr<fireside::dsp::SpectrumAnalyzer> pic_an_; std::vector<std::vector<float>> pic_frames_;
    std::vector<float> pic_last_; std::vector<std::complex<float>> pic_buf_; QCheckBox* hum_on_ = nullptr; QComboBox* hum_hz_ = nullptr; QLabel* pic_peak_ = nullptr; float pic_pk_ = 0; QSlider* test_vol_ = nullptr; SpectrumWidget* pic_spec_ = nullptr; WaterfallWidget* pic_wf_ = nullptr; void drawPicture(); void setPicturePassband();
    std::unique_ptr<fireside::txc::IAudioIn> test_in_; QTimer* timer_; float test_peak_ = 0;
    QCheckBox* hear_ = nullptr; QProgressBar* out_level_ = nullptr; std::unique_ptr<fireside::vox::MicLoop> loop_; class PcAudioFeed* feed_ = nullptr; QTimer* restart_ = nullptr; bool pc_audio_ = true;
    bool loading_ = false; std::function<std::unique_ptr<fireside::txc::IAudioIn>()> test_factory_, ext_factory_;
    ReceiverPanel* rx_ = nullptr; QCheckBox* data_out_ = nullptr; QGridLayout* other_grid_ = nullptr; QSpinBox* data_level_ = nullptr; QLabel* ext_note_ = nullptr; void applyDataOut();
    void load(); void save(); void resetProcessing();
};
