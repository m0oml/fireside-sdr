#pragma once
#include <QWidget>

// Segmented S-meter with peak hold. The input is dBFS; the S-unit and dBm scale uses a placeholder offset until the radio's input level is calibrated, and the app labels it "uncal".
class SMeter : public QWidget {
    Q_OBJECT
public:
    explicit SMeter(QWidget* parent = nullptr) : QWidget(parent) { setMinimumSize(260, 46); }
    void setLevel(double dbfs);                              // call as often as you like; the peak marker decays by itself
    double level() const { return level_; }
    // Until the radio is calibrated, dBm = dBFS + kUncalOffsetDb (a placeholder, NOT a measurement). S9 = -73 dBm, 6 dB per S-unit, as on HF receivers.
    static constexpr double kUncalOffsetDb = -10.0;
    static double dbm(double dbfs) { return dbfs + kUncalOffsetDb; }
    void setOffsetDb(double off) { if (off != offset_) { offset_ = off; update(); } }   // dBm = dBFS + offset: the placeholder, or the radio's factory RX level table for the band
    double offsetDb() const { return offset_; }
    static QString sText(double dbm);                        // "S7", "S9+20"
    QSize sizeHint() const override { return {300, 52}; }
protected:
    void paintEvent(QPaintEvent*) override;
private:
    double level_ = -120, peak_ = -120; qint64 peak_t_ = 0; double offset_ = kUncalOffsetDb;
};
