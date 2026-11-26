#pragma once
#include <QWidget>
#include <vector>
#include "fireside/display.h"

// Spectrum trace: fft-shifted dBFS bins centred on the dial. Pure display; all maths lives in fireside/display.h.
class SpectrumWidget : public QWidget {
    Q_OBJECT
public:
    explicit SpectrumWidget(QWidget* parent = nullptr);
    void setFrame(const std::vector<float>& db);           // smoothed per bin by the averaging factor (setAveraging)
    void setSpan(double sampleRateHz) { span_hz_ = sampleRateHz; avg_.clear(); update(); }
    double spanHz() const { return span_hz_; }
    void setDial(double dialHz) { if (dialHz != dial_hz_) avg_.clear(); dial_hz_ = dialHz; update(); }   // a retune moves the picture: restart the average
    void setAveraging(float alpha) { alpha_ = alpha > 0.f && alpha <= 1.f ? alpha : 1.f; avg_.clear(); }   // 1 = off; smaller = steadier (per-frame weight of a new frame)
    float averaging() const { return alpha_; }
    // Zoom: the part of the full span shown, as fractions 0..1 of it (0 = left edge, 0.5 = the dial). Shared with the waterfall by the panel.
    void setView(double lo, double hi) { view_lo_ = lo; view_hi_ = hi; update(); }
    double viewLo() const { return view_lo_; }
    double viewHi() const { return view_hi_; }
    void setPassband(double loHz, double hiHz) { pb_lo_ = loHz; pb_hi_ = hiHz; update(); }
    void setScale(fireside::display::Scale s) { scale_ = s; update(); }
    void setNotchOffsets(const std::vector<double>& rfOffsetsHz) { notches_ = rfOffsetsHz; update(); }   // where the manual notches sit on the spectrum (offsets from the dial)
    void setBandPlan(bool on) { bandplan_ = on; update(); }
    void setUnit(const QString& u) { unit_ = u; update(); }   // the readout's unit ("dBFS"; "dBm TX" while the transmit picture is shown)
    bool bandPlan() const { return bandplan_; }
    QSize minimumSizeHint() const override { return {320, 140}; }
signals:
    void wheelSteps(int steps);                   // mouse wheel: +-1 per notch, x10 with Shift (wheel towards you = up in frequency)
    void zoomRequested(int notches, double anchor);   // Ctrl+wheel: + = zoom in, about `anchor` (fraction of the FULL span under the mouse)
    void offsetClicked(double offsetHz);          // click on the trace: offset from the centre (dial) in Hz
    void notchMoved(int index, double offsetHz);       // a notch marker was dragged
    void notchToggled(double offsetHz);               // Alt+click: add a notch here, or remove the one under the click
    void passbandShifted(double deltaHz);             // Ctrl+drag inside the passband: slide the whole passband
    void passbandEdgeDragged(double loHz, double hiHz);   // the user dragged an edge of the shaded passband (offsets from the dial, lo < hi)
protected:
    void paintEvent(QPaintEvent*) override;
    void mouseMoveEvent(QMouseEvent*) override;
    void mousePressEvent(QMouseEvent*) override;
    void mouseReleaseEvent(QMouseEvent*) override { drag_edge_ = 0; drag_notch_ = -1; drag_shift_ = false; }
    void wheelEvent(QWheelEvent*) override;
    void leaveEvent(QEvent*) override { hover_x_ = -1; update(); }
private:
    QString unit_ = "dBFS";
    std::vector<float> db_, avg_; float alpha_ = 0.5f; double view_lo_ = 0, view_hi_ = 1; double span_hz_ = 48000, dial_hz_ = 7100000, pb_lo_ = 200, pb_hi_ = 2800; fireside::display::Scale scale_; int hover_x_ = -1, hover_y_ = -1, wheel_acc_ = 0, drag_edge_ = 0, drag_notch_ = -1; bool drag_shift_ = false; double shift_last_hz_ = 0; std::vector<double> notches_; bool bandplan_ = true;
    int notchAt(int x) const;   // drag_edge_: 1 = low edge, 2 = high edge
    double offLo() const { return (view_lo_ - 0.5) * span_hz_; }        // offsets from the dial at the plot's left / right edges
    double offHi() const { return (view_hi_ - 0.5) * span_hz_; }
    int xOf(double offHz, int left, int pw) const;                      // offset from the dial -> x (may lie outside the plot)
    double offAt(int x, int left, int pw) const;
    int edgeAt(int x, int y) const;                                      // only the small handles along the bottom of the plot grab an edge: a click anywhere else in the trace tunes
};
