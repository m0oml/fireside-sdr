#pragma once
#include <QImage>
#include <QWidget>
#include <deque>
#include <vector>
#include "fireside/display.h"

class WaterfallWidget : public QWidget {
    Q_OBJECT
public:
    explicit WaterfallWidget(QWidget* parent = nullptr);
    void pushRow(const std::vector<float>& db, fireside::display::Scale scale);   // newest row at the top; older rows scroll down
    void setPalette(fireside::display::Palette p);                                // recolours the whole history at once
    void setFloorOffsetDb(float db);                                          // > 0 darkens the noise floor, < 0 brightens it (history is recoloured too)
    void setSpan(double sampleRateHz) { span_hz_ = sampleRateHz; update(); }       // for the tuning line and passband overlay
    void setPassband(double loHz, double hiHz) { pb_lo_ = loHz; pb_hi_ = hiHz; update(); }   // offsets from the dial; display only, never draggable here
    void setView(double lo, double hi);                                       // zoom: fractions 0..1 of the full span (history is redrawn at the new zoom)
    fireside::display::Palette palette() const { return palette_; }
    size_t rows() const { return rows_; }
    const QImage& image() const { return img_; }
    QSize minimumSizeHint() const override { return {320, 120}; }
signals:
    void wheelSteps(int steps);                                               // mouse wheel: +-1 per notch, x10 with Shift (towards you = up)
    void zoomRequested(int notches, double anchor);                           // Ctrl+wheel: + = in, about `anchor` (fraction of the FULL span)
    void clickedAt(double fraction);                                          // left click: 0..1 across the FULL span (0.5 = the dial), zoom already applied
protected:
    void paintEvent(QPaintEvent*) override;
    void wheelEvent(QWheelEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
private:
    struct Row { std::vector<float> db; fireside::display::Scale scale; };     // db: the whole frame, so zooming can redraw history sharply
    QImage img_; size_t rows_ = 0; fireside::display::Palette palette_ = fireside::display::Palette::Classic; float floor_off_ = 0; std::deque<Row> hist_; int wheel_acc_ = 0; double view_lo_ = 0, view_hi_ = 1, span_hz_ = 48000, pb_lo_ = 200, pb_hi_ = 2800;
    void paintRow(int y, const Row& r);
};
