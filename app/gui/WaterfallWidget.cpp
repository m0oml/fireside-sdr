#include "WaterfallWidget.h"
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <cstdio>
#include <cstdlib>
#include <algorithm>
#include <cmath>
#include <cstring>
using namespace fireside::display;

WaterfallWidget::WaterfallWidget(QWidget* p) : QWidget(p), img_(1024, 240, QImage::Format_RGB32) {
    img_.fill(QColor("#05081a"));                                             // 240 rows: fills in ~10 s, and the empty part is a dark blue, not a black hole
    setMinimumHeight(120);
}
void WaterfallWidget::paintRow(int y, const Row& r) {
    Scale s = r.scale; s.lo_db += floor_off_;
    auto* line = reinterpret_cast<QRgb*>(img_.scanLine(y));
    const int w = img_.width(); const double n = double(r.db.size()), a0 = view_lo_ * n, a1 = view_hi_ * n;
    for (int x = 0; x < w; ++x) {                                             // the visible bins, max per column (nearest bin when zoomed past 1 bin per column)
        float m = -200.f;
        if (!r.db.empty()) { size_t i = size_t(std::clamp(a0 + (a1 - a0) * x / w, 0.0, n - 1)), j = size_t(std::clamp(a0 + (a1 - a0) * (x + 1) / w, 0.0, n)); if (j <= i) j = i + 1; m = -300.f; for (size_t b = i; b < j && b < r.db.size(); ++b) m = std::max(m, r.db[b]); }
        line[x] = 0xFF000000u | colormap(to_unit(m, s), palette_);
    }
}
void WaterfallWidget::pushRow(const std::vector<float>& db, Scale s) {
    Row row{db, s};
    std::memmove(img_.bits() + img_.bytesPerLine(), img_.constBits(), size_t(img_.bytesPerLine()) * size_t(img_.height() - 1));   // scroll down one row
    paintRow(0, row); hist_.push_front(std::move(row)); while (hist_.size() > size_t(img_.height())) hist_.pop_back();
    ++rows_; update();
}
void WaterfallWidget::setView(double lo, double hi) { if (lo == view_lo_ && hi == view_hi_) return; view_lo_ = lo; view_hi_ = hi; for (size_t y = 0; y < hist_.size(); ++y) paintRow(int(y), hist_[y]); update(); }
void WaterfallWidget::setPalette(Palette p) { if (p == palette_) return; palette_ = p; for (size_t y = 0; y < hist_.size(); ++y) paintRow(int(y), hist_[y]); update(); }
void WaterfallWidget::setFloorOffsetDb(float db) { if (db == floor_off_) return; floor_off_ = db; for (size_t y = 0; y < hist_.size(); ++y) paintRow(int(y), hist_[y]); update(); }
void WaterfallWidget::wheelEvent(QWheelEvent* e) {
    if (std::getenv("FIRESIDE_DEBUG_WHEEL")) std::fprintf(stderr, "wheel %s: modifiers=0x%x angle=(%d,%d) pixel=(%d,%d) phase=%d inverted=%d\n", metaObject()->className(), unsigned(e->modifiers().toInt()), e->angleDelta().x(), e->angleDelta().y(), e->pixelDelta().x(), e->pixelDelta().y(), int(e->phase()), int(e->inverted()));   // FIRESIDE_DEBUG_WHEEL=1: what the window system really delivers
    wheel_acc_ += e->angleDelta().y(); const int notches = wheel_acc_ / 120; wheel_acc_ -= notches * 120;      // touchpads send small deltas: accumulate to whole notches
    const int left = 44, pw = width() - left - 6;
    if (notches && (e->modifiers() & Qt::ControlModifier)) { const double f = pw > 0 ? std::clamp(double(e->position().x() - left) / double(pw), 0.0, 1.0) : 0.5; emit zoomRequested(notches, view_lo_ + f * (view_hi_ - view_lo_)); }
    else if (notches) emit wheelSteps(-notches * ((e->modifiers() & Qt::ShiftModifier) ? 10 : 1));        // same direction as the spectrum
    e->accept();
}
void WaterfallWidget::mousePressEvent(QMouseEvent* e) {
    const int left = 44, pw = width() - left - 6;
    if (e->button() == Qt::LeftButton && pw > 20 && e->pos().x() >= left && e->pos().x() < left + pw) emit clickedAt(view_lo_ + double(e->pos().x() - left) / double(pw) * (view_hi_ - view_lo_));
}
void WaterfallWidget::paintEvent(QPaintEvent*) {
    QPainter p(this); p.fillRect(rect(), QColor("#0b0e14"));
    const int left = 44, pw = width() - left - 6;                                // same plot columns as the spectrum above it
    if (pw > 0) p.drawImage(QRect(left, 0, pw, height()), img_);
    if (pw > 20 && span_hz_ > 0 && view_hi_ > view_lo_) {                        // the tuning line and the passband, drawn over the picture (display only: a click here always tunes)
        auto xOf = [&](double off) { return left + int(std::lround(((off / span_hz_ + 0.5) - view_lo_) / (view_hi_ - view_lo_) * pw)); };
        p.setClipRect(QRect(left, 0, pw, height()));
        const int x0 = xOf(pb_lo_), x1 = xOf(pb_hi_); p.fillRect(QRect(QPoint(std::min(x0, x1), 0), QPoint(std::max(x0, x1), height())), QColor(80, 120, 200, 40));
        p.setPen(QPen(QColor(120, 170, 255, 150), 1)); p.drawLine(x0, 0, x0, height()); p.drawLine(x1, 0, x1, height());
        const int tx = xOf(0.0); p.setPen(QPen(QColor(255, 80, 60, 235), 2)); p.drawLine(tx, 0, tx, height());
    }
}
