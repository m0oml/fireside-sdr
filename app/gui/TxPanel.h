#pragma once
#include <QElapsedTimer>
#include <QFrame>
#include <QImage>
#include <QWidget>
#include <functional>
#include <memory>
#include <thread>
#include <vector>
#include "fireside/tx_render.h"
class QComboBox; class QDoubleSpinBox; class QLabel; class QLineEdit; class QPushButton; class QSpinBox; class QStackedWidget; class QTemporaryDir; class QTimer; class QCheckBox;

// A box that records two paddles from the keyboard. Click it to give it the keyboard focus; Z, comma or left arrow = dit paddle, X, period or right arrow = dah paddle.
// It only records timings (the lamps show what is held); the recording is turned into CW by the renderer. No sound is made here.
class PaddlePad : public QFrame {
public:
    explicit PaddlePad(QWidget* parent = nullptr);
    void paddle(bool dit, bool dah, double t_s);        // set both paddles at time t_s (the key handlers call this; so can a test)
    void press(bool dit, bool dah);                     // as above, timed by the pad's own clock (the first call starts the clock)
    void clear();
    const std::vector<fireside::tx::PaddleEvent>& events() const { return events_; }
    bool full() const { return events_.size() >= 100000 || (!events_.empty() && events_.back().t_s >= 590.0); }
    std::function<void()> onChange;
protected:
    void keyPressEvent(QKeyEvent* e) override;
    void keyReleaseEvent(QKeyEvent* e) override;
    void mousePressEvent(QMouseEvent*) override { setFocus(); }
    void paintEvent(QPaintEvent*) override;
private:
    void key(QKeyEvent* e, bool down);
    std::vector<fireside::tx::PaddleEvent> events_; QElapsedTimer clock_; bool dit_ = false, dah_ = false;
};

// A picture of a rendered signal: time runs left to right, audio frequency up the side (-3 kHz to +3 kHz around the dial, USB above the dial line), brightness is level
// relative to the strongest part (70 dB range). Drawn from the rendered I/Q only.
class RenderPlot : public QWidget {
public:
    explicit RenderPlot(QWidget* parent = nullptr);
    void setSignal(const std::vector<fireside::tx::cf>& iq, double fs);
    void clear();
    bool hasImage() const { return !img_.isNull(); }
    QSize imageSize() const { return img_.size(); }
    QSize minimumSizeHint() const override { return QSize(240, 140); }
protected:
    void paintEvent(QPaintEvent*) override;
private:
    QImage img_; double seconds_ = 0;
};

// OFFLINE transmit section of the Receiver screen. It renders a signal (FT8, FT4, CW, voice from a WAV file, two-tone) into a baseband I/Q buffer and lets you save it as a WAV or look at it
// on the receiver's waterfall. It is NOT connected to the radio: there is no transmit key here, no radio output and no sound output, and a CTest keeps it that way
// (tests/tx_isolation.cmake scans this file: only Qt, Theme.h and the transmit renderer may be included, and words for a transmit key, a radio connection, sound or network classes are forbidden).
class TxPanel : public QWidget {
    Q_OBJECT
public:
    enum Mode { Ft8 = 0, Ft4 = 1, Cw = 2, Voice = 3, TwoTone = 4, Psk31 = 5, Rtty = 6, Keyer = 7 };
    explicit TxPanel(QWidget* parent = nullptr);
    ~TxPanel() override;
    // Programmatic access (the smoke test and screenshots drive the panel through these)
    void setMode(Mode m);
    Mode mode() const;
    void setMessage(const QString& text);
    void setAudioHz(double hz);
    void setSidebandLsb(bool lsb);
    void setCwText(const QString& text);
    void setDigitalText(const QString& text);           // PSK31 / RTTY text
    void setDigitalHz(double hz);                       // PSK31 audio frequency / RTTY mark tone
    void setVoiceFile(const QString& path);
    // Keyer practice: Z / comma / left arrow = dit paddle, X / period / right arrow = dah paddle, recorded on the pad and rendered through the iambic keyer.
    PaddlePad* paddlePad() const { return pad_; }
    RenderPlot* plot() const { return plot_; }
    void setKeyerOptions(double wpm, bool modeB, bool reverse);
    // CW message memories (six slots, kept between runs): recall puts the stored text in the CW text box; store keeps the box's text in a slot.
    static constexpr int kMemories = 6;
    QString memoryText(int slot) const;
    bool setMemory(int slot, const QString& text);      // false if the slot is out of range
    bool recallMemory(int slot);                        // false if out of range or empty
    bool storeMemory(int slot);                         // stores the CW text box
    bool render();                                      // starts a render on a worker thread; false if one is already running
    bool busy() const { return busy_; }
    bool hasResult() const { return !iq_.empty(); }
    const std::vector<fireside::tx::cf>& iq() const { return iq_; }
    QString summary() const;                            // the result line shown under the buttons
    QString imdText() const;                            // two-tone: the measured IM3 / IM5 line, else empty
    QString encodableText() const;                      // FT8/FT4: whether the message can be encoded
    QString slotText() const;                           // FT8/FT4: UTC clock, slot parity and countdown
    bool saveWav(const QString& path);
    QString previewPath();                              // writes the result to a temporary WAV and returns its path ("" on failure)
    static int decoderIndexFor(Mode m);
    int previewToneHz() const;                          // the tone the receiver's decoder should be set to for the last render (0 = none)                 // receiver decoder entry that matches the mode (0 = none)
signals:
    void previewRequested(const QString& wavPath, int decoderIndex, int toneHz);   // toneHz: PSK31 centre / RTTY mark for the decoder, 0 = leave it alone
    void rendered();
private slots:
    void onModeChanged(int i);
    void updateEncodable();
    void updateSlot();
    void onSavePressed();
    void onPreviewPressed();
    void onBrowseVoice();
    void onStorePressed();
    void onPadChanged();
private:
    struct Job { std::vector<fireside::tx::cf> iq; QString error; };
    void finishJob(const std::shared_ptr<Job>& j);
    void saveMemories(); void refreshMemoryButtons();
    void setResultText(const QString& s, bool warn);
    QComboBox *mode_, *side_, *parity_; QStackedWidget* pages_;
    QLineEdit *msg_, *cwText_, *voiceFile_, *digText_; QLabel* digHzLabel_; QDoubleSpinBox *digHz_, *digLevel_; QDoubleSpinBox *hz_, *ftLevel_, *wpm_, *edge_, *cwLevel_, *cwOffset_, *gain_, *ttSeconds_, *ttPeak_;
    QCheckBox* micChain_; PaddlePad* pad_; RenderPlot* plot_; QDoubleSpinBox *keyWpm_, *keyWeight_; QComboBox* keyMode_; QCheckBox* keyReverse_; QLabel* padInfo_; QPushButton* memButtons_[kMemories]; QComboBox* memSlot_; QStringList memories_; QLabel *encodable_, *slot_, *result_, *imd_; QPushButton *render_, *save_, *preview_; QTimer* clock_;
    std::vector<fireside::tx::cf> iq_; bool busy_ = false; fireside::tx::Sideband renderedSide_ = fireside::tx::Sideband::USB; Mode renderedMode_ = Ft8; double renderedHz_ = 0;
    std::thread worker_; std::unique_ptr<QTemporaryDir> tmp_; QString summary_, imd_text_;
};
