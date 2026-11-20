#include "SpectrumWidget.h"
#include "fireside/bandplan.h"
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
using namespace fireside::display;

SpectrumWidget::SpectrumWidget(QWidget* p) : QWidget(p) { setMouseTracking(true); setMinimumHeight(140); }
void SpectrumWidget::setFrame(const std::vector<float>& db) {
    if (alpha_ >= 1.f || avg_.size() != db.size()) avg_ = db;
    else for (size_t i = 0; i < db.size(); ++i) avg_[i] += alpha_ * (db[i] - avg_[i]);     // exponential average in dB: steadier trace, same peaks after a moment
    db_ = avg_; update();
}
int SpectrumWidget::xOf(double off, int left, int pw) const { return left + int(std::lround((off - offLo()) / (offHi() - offLo()) * double(pw))); }
double SpectrumWidget::offAt(int x, int left, int pw) const { return offLo() + double(x - left) / double(pw) * (offHi() - offLo()); }
int SpectrumWidget::edgeAt(int x, int y) const {                           // 1 / 2 when (x, y) is on the low / high edge's handle: within 7 px sideways, in the bottom 16 px of the plot
    const int left = 44, bottom = 18, top = 6, pw = width() - left - 6, ph = height() - bottom - top; if (pw < 20 || y < top + ph - 16 || y > top + ph) return 0;
    const int x0 = xOf(pb_lo_, left, pw), x1 = xOf(pb_hi_, left, pw);
    const int d0 = std::abs(x - x0), d1 = std::abs(x - x1);
    if (d0 <= 7 && d0 <= d1) return 1; if (d1 <= 7) return 2; return 0;
}
int SpectrumWidget::notchAt(int x) const {
    const int left = 44, pw = width() - left - 6; if (pw < 20) return -1; int best = -1, bd = 7;
    for (size_t i = 0; i < notches_.size(); ++i) { const int d = std::abs(x - xOf(notches_[i], left, pw)); if (d < bd) { bd = d; best = int(i); } }
    return best;
}
void SpectrumWidget::mouseMoveEvent(QMouseEvent* e) {
    hover_x_ = e->pos().x(); hover_y_ = e->pos().y();
    if (drag_notch_ >= 0) { const int left = 44, pw = width() - left - 6; const double hz = offAt(std::clamp(hover_x_, left, left + pw), left, pw); notches_[size_t(drag_notch_)] = hz; emit notchMoved(drag_notch_, hz); }
    else if (drag_shift_) { const int left = 44, pw = width() - left - 6; const double hz = offAt(std::clamp(hover_x_, left, left + pw), left, pw); emit passbandShifted(hz - shift_last_hz_); shift_last_hz_ = hz; }
    else if (drag_edge_) {
        const int left = 44, pw = width() - left - 6; double hz = offAt(std::clamp(hover_x_, left, left + pw), left, pw);
        if (drag_edge_ == 1) pb_lo_ = std::min(hz, pb_hi_ - 100.0); else pb_hi_ = std::max(hz, pb_lo_ + 100.0);
        emit passbandEdgeDragged(pb_lo_, pb_hi_);
    } else setCursor((edgeAt(hover_x_, hover_y_) || notchAt(hover_x_) >= 0) ? Qt::SizeHorCursor : Qt::ArrowCursor);
    update();
}

void SpectrumWidget::mousePressEvent(QMouseEvent* e) {
    const int left = 44, pw = width() - left - 6;
    if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::AltModifier) && pw > 20 && e->pos().x() >= left && e->pos().x() < left + pw) { emit notchToggled(offAt(e->pos().x(), left, pw)); return; }   // Alt+click: add / remove a notch
    if (e->button() == Qt::LeftButton && (drag_notch_ = notchAt(e->pos().x())) >= 0) return;                         // grab a notch marker
    if (e->button() == Qt::LeftButton && (e->modifiers() & Qt::ControlModifier) && pw > 20) {                           // Ctrl+drag inside the passband: slide it
        const double hz = offAt(e->pos().x(), left, pw); if (hz >= pb_lo_ && hz <= pb_hi_) { drag_shift_ = true; shift_last_hz_ = hz; return; }
    }
    if (e->button() == Qt::LeftButton && (drag_edge_ = edgeAt(e->pos().x(), e->pos().y())) != 0) return;                       // grab a passband edge handle instead of tuning
    if (e->button() == Qt::LeftButton && pw > 20 && e->pos().x() >= left && e->pos().x() < left + pw) emit offsetClicked(offAt(e->pos().x(), left, pw));
}
void SpectrumWidget::wheelEvent(QWheelEvent* e) {
    if (std::getenv("FIRESIDE_DEBUG_WHEEL")) std::fprintf(stderr, "wheel %s: modifiers=0x%x angle=(%d,%d) pixel=(%d,%d) phase=%d inverted=%d\n", metaObject()->className(), unsigned(e->modifiers().toInt()), e->angleDelta().x(), e->angleDelta().y(), e->pixelDelta().x(), e->pixelDelta().y(), int(e->phase()), int(e->inverted()));   // FIRESIDE_DEBUG_WHEEL=1: what the window system really delivers
    wheel_acc_ += e->angleDelta().y(); const int notches = wheel_acc_ / 120; wheel_acc_ -= notches * 120;      // touchpads send small deltas: accumulate to whole notches
    if (notches && (e->modifiers() & Qt::ControlModifier)) {                                                     // Ctrl+wheel: zoom about the mouse
        const int left = 44, pw = width() - left - 6; const double f = pw > 0 ? std::clamp(double(e->position().x() - left) / double(pw), 0.0, 1.0) : 0.5;
        emit zoomRequested(notches, view_lo_ + f * (view_hi_ - view_lo_));
    } else if (notches) emit wheelSteps(-notches * ((e->modifiers() & Qt::ShiftModifier) ? 10 : 1));        // wheel towards you = up (user preference, 2026-11-18)
    e->accept();
}
void SpectrumWidget::paintEvent(QPaintEvent*) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing, false);
    const int w = width(), h = height(), left = 44, bottom = 18, top = 6, pw = w - left - 6, ph = h - bottom - top;
    p.fillRect(rect(), QColor("#0b0e14"));
    if (pw < 20 || ph < 20) return;
    const QRect plot(left, top, pw, ph);
    auto yFor = [&](float db) { return top + int(std::lround((1.0f - to_unit(db, scale_)) * float(ph))); };
    p.setFont(QFont("monospace", 8));
    // horizontal dB grid
    const float gridDb = (scale_.hi_db - scale_.lo_db) * 14.f > float(ph) * 10.f ? 20.f : 10.f;   // 10 dB lines, but 20 dB when the range is so tall that the labels would crowd (about 14 px minimum)
    for (float d = std::ceil(scale_.lo_db / gridDb) * gridDb; d <= scale_.hi_db; d += gridDb) {
        const int y = yFor(d); p.setPen(QColor(50, 56, 70)); p.drawLine(left, y, left + pw, y);
        p.setPen(QColor(150, 158, 175)); p.drawText(QRect(0, y - 7, left - 4, 14), Qt::AlignRight | Qt::AlignVCenter, QString::number(int(d)));
    }
    // vertical frequency grid, labels in absolute kHz of the dial's MHz (e.g. 7101.0)
    const double vspan = offHi() - offLo(), step = nice_step(vspan, 8);
    for (double f = std::ceil(offLo() / step) * step; f <= offHi() + 1e-6; f += step) {
        const int x = xOf(f, left, pw); p.setPen(std::fabs(f) < 1e-6 ? QColor(120, 90, 40) : QColor(50, 56, 70)); p.drawLine(x, top, x, top + ph);
        p.setPen(QColor(150, 158, 175)); p.drawText(QRect(x - 40, top + ph + 2, 80, 14), Qt::AlignHCenter, QString::number((dial_hz_ + f) / 1000.0, 'f', step < 1000 ? 2 : 1));
    }
    // band plan along the TOP of the plot, clear of the noise: RSGB usage strip inside the Ofcom allocations (60 m gaps in red), FT8/FT4 ticks below it
    if (bandplan_) {
        const double lo = dial_hz_ + offLo(), hi = dial_hz_ + offHi(); const int sy = top + 1, sh = 8;
        auto clampx = [&](double hz) { return std::clamp(xOf(hz - dial_hz_, left, pw), left, left + pw); };
        for (const auto& g : fireside::bandplan::gaps_between(lo, hi)) { const int xa = clampx(g.lo_hz), xb = clampx(g.hi_hz); p.fillRect(QRect(xa, sy, std::max(1, xb - xa), sh), QColor(150, 40, 40, 170)); }
        for (const auto& sg : fireside::bandplan::segments_between(lo, hi)) {
            using fireside::bandplan::Kind; const int xa = clampx(sg.lo_hz), xb = clampx(sg.hi_hz);
            const QColor c = sg.kind == Kind::Cw ? QColor(70, 190, 110) : sg.kind == Kind::Narrow ? QColor(170, 110, 230) : sg.kind == Kind::AllModes ? QColor(80, 140, 230)
                           : sg.kind == Kind::Phone ? QColor(60, 110, 200) : sg.kind == Kind::Beacon ? QColor(240, 190, 60) : sg.kind == Kind::Fm ? QColor(60, 190, 200) : QColor(120, 126, 140);
            p.fillRect(QRect(xa, sy, std::max(1, xb - xa - 1), sh), c);                       // 1 px gap between neighbouring segments
            if (xb - xa > 52) { p.setPen(c.lightness() > 150 ? QColor(15, 18, 24) : QColor(235, 240, 250)); p.setFont(QFont("monospace", 7)); p.drawText(QRect(xa, sy, xb - xa, sh), Qt::AlignCenter, fireside::bandplan::kind_name(sg.kind)); p.setFont(QFont("monospace", 8)); }
        }
        for (const auto& m : fireside::bandplan::marks_between(lo, hi)) {
            const int mx = xOf(m.hz - dial_hz_, left, pw); p.setPen(QPen(QColor(255, 255, 255, 200), 1)); p.drawLine(mx, sy + sh, mx, sy + sh + 8);
            p.setPen(QColor(235, 235, 245)); p.drawText(QRect(mx - 20, sy + sh + 8, 40, 12), Qt::AlignHCenter, m.label);
        }
    }
    // passband overlay (clipped: when zoomed it can lie partly or wholly outside the view)
    p.setClipRect(plot);
    const int x0 = xOf(pb_lo_, left, pw), x1 = xOf(pb_hi_, left, pw);
    p.fillRect(QRect(QPoint(std::min(x0, x1), top), QPoint(std::max(x0, x1), top + ph)), QColor(80, 120, 200, 45));
    p.setPen(QPen(QColor(120, 170, 255, 190), 2)); p.drawLine(x0, top, x0, top + ph); p.drawLine(x1, top, x1, top + ph);          // draggable edges
    for (int xe : {x0, x1}) { p.setPen(Qt::NoPen); p.setBrush(QColor(120, 170, 255, 230)); const QPoint tri[3] = {{xe - 5, top + ph}, {xe + 5, top + ph}, {xe, top + ph - 9}}; p.drawPolygon(tri, 3); p.setBrush(Qt::NoBrush); }   // drag handles (edges are only grabbed here); the brush must not leak into the plot border
    for (size_t i = 0; i < notches_.size(); ++i) {                                                                               // manual notches
        const int nx = xOf(notches_[i], left, pw); p.setPen(QPen(QColor(255, 90, 90, 220), 1, Qt::DashLine)); p.drawLine(nx, top, nx, top + ph);
        p.fillRect(QRect(nx - 6, top, 12, 12), QColor(255, 90, 90)); p.setPen(QColor(20, 20, 20)); p.drawText(QRect(nx - 6, top, 12, 12), Qt::AlignCenter, "N");
    }
    // trace
    if (!db_.empty()) {
        std::vector<float> d(static_cast<size_t>(pw));                                                     // the visible bins, max per pixel (or the nearest bin when zoomed past 1 bin/px)
        { const double n = double(db_.size()), a0 = view_lo_ * n, a1 = view_hi_ * n;
          for (int x = 0; x < pw; ++x) { size_t i = size_t(std::clamp(a0 + (a1 - a0) * x / pw, 0.0, n - 1)), j = size_t(std::clamp(a0 + (a1 - a0) * (x + 1) / pw, 0.0, n)); if (j <= i) j = i + 1;
                                         float m = -300.f; for (size_t b = i; b < j && b < db_.size(); ++b) m = std::max(m, db_[b]); d[size_t(x)] = m; } }
        QPainterPath path; path.moveTo(left, top + ph);
        for (size_t i = 0; i < d.size(); ++i) path.lineTo(left + int(i), yFor(d[i]));
        path.lineTo(left + int(d.size()) - 1, top + ph); path.closeSubpath();
        p.setClipRect(plot); p.fillPath(path, QColor(60, 180, 255, 70)); p.setPen(QColor(120, 220, 255));
        for (size_t i = 1; i < d.size(); ++i) p.drawLine(left + int(i) - 1, yFor(d[i - 1]), left + int(i), yFor(d[i]));
        p.setClipping(false);
    }
    { const int tx = xOf(0.0, left, pw); if (tx >= left && tx < left + pw) { p.setClipRect(plot); p.setPen(QPen(QColor(255, 80, 60, 235), 2)); p.drawLine(tx, top, tx, top + ph); p.setClipping(false); } }   // the tuning line: the dial frequency
    p.setPen(QColor(70, 76, 92)); p.drawRect(plot.adjusted(0, 0, -1, -1));
    if (view_hi_ - view_lo_ < 0.999) {                                                          // zoom indicator
        p.setPen(QColor(255, 200, 80)); p.drawText(QRect(left + pw - 210, top + 22, 204, 14), Qt::AlignRight, QString("zoom x%1  (Ctrl+wheel)").arg(1.0 / (view_hi_ - view_lo_), 0, 'f', 1));
    }
    // hover readout
    if (hover_x_ >= left && hover_x_ < left + pw && !db_.empty()) {
        const double off = offAt(hover_x_, left, pw);
        const size_t bin = size_t(std::min(std::max(std::lround((off / span_hz_ + 0.5) * double(db_.size() - 1)), 0L), long(db_.size() - 1)));
        p.setPen(QColor(255, 200, 80)); p.drawLine(hover_x_, top, hover_x_, top + ph);
        const std::string seg = bandplan_ ? fireside::bandplan::describe(dial_hz_ + off) : std::string();
        p.drawText(QRect(left + 6, top + ph - 16, pw - 12, 14), Qt::AlignLeft, QString("%1  %2 dBFS%3").arg(QString::fromStdString(format_mhz(dial_hz_ + off))).arg(double(db_[bin]), 0, 'f', 1).arg(seg.empty() ? QString() : "  " + QString::fromStdString(seg)));
    }
}
