#pragma once
#include <QWidget>

// Transmit power meter in the S-meter's style: two segmented bars, forward (accent) and reflected (amber), on a square-root watts scale 0..30 W with
// peak hold, and a text line under them (fwd / ref / SWR while keyed; the last over's figures afterwards). Fed by the Transmit card from the PA meter.
class PowerMeter : public QWidget {
    Q_OBJECT
public:
    explicit PowerMeter(QWidget* parent = nullptr);
    void setReading(double fwd_w, double ref_w, double swr, bool keyed);   // swr < 1: unknown
    QSize sizeHint() const override { return {340, 70}; }
    QString text() const { return text_; }
protected:
    void paintEvent(QPaintEvent*) override;
private:
    double fwd_ = 0, ref_ = 0, pk_ = 0, last_fwd_ = 0, last_ref_ = 0, last_swr_ = 0; qint64 pk_t_ = 0; bool keyed_ = false; QString text_;
};
