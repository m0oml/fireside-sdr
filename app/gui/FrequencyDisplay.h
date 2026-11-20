#pragma once
#include <QWidget>

// The tuning readout: "07.100.000" in large digits. Scroll the wheel over a digit to change it by that digit's place value, or click the upper/lower half of a digit
// to step it up/down. Leading zeros are dimmed. Emits valueChanged only for changes (like a spin box).
class FrequencyDisplay : public QWidget {
    Q_OBJECT
public:
    explicit FrequencyDisplay(QWidget* parent = nullptr);
    double hz() const { return double(hz_); }
    void setHz(double hz);                                   // clamps to the range; emits valueChanged if it changed
    void setRangeHz(double lo, double hi) { lo_ = qint64(lo); hi_ = qint64(hi); setHz(double(hz_)); }
    void setLabelOnly(bool labelOnly) { label_only_ = labelOnly; update(); }   // false = "tunes the radio": the accent colour changes so it is obvious
    QSize sizeHint() const override { return {330, 64}; }
    QSize minimumSizeHint() const override { return {300, 60}; }
    int digitAt(const QPoint& p) const;                      // place index (0 = Hz) under a point, -1 if none (exposed for tests)
signals:
    void valueChanged(double hz);
protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void leaveEvent(QEvent*) override { hover_ = -1; update(); }
private:
    qint64 hz_ = 7100000, lo_ = 100000, hi_ = 65000000; bool label_only_ = true; int hover_ = -1;
    static constexpr int kDigits = 8;
    struct Cell { int x, w; int place; };
    void layoutCells(std::vector<Cell>& cells, int& sepY) const;
    void bump(int place, int dir);
};
