#include "FrequencyDisplay.h"
#include <QFontDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QWheelEvent>
#include <cmath>
#include <vector>
#include "Theme.h"

FrequencyDisplay::FrequencyDisplay(QWidget* p) : QWidget(p) { setMouseTracking(true); setFocusPolicy(Qt::NoFocus); setToolTip("Scroll over a digit to tune, or click the top / bottom half of a digit"); }

void FrequencyDisplay::setHz(double hz) {
    const qint64 v = qBound(lo_, qint64(std::llround(hz)), hi_);
    if (v == hz_) return;
    hz_ = v; update(); emit valueChanged(double(v));
}
void FrequencyDisplay::layoutCells(std::vector<Cell>& cells, int& sepY) const {
    QFont f = font(); f.setPixelSize(std::max(24, height() - 16)); f.setWeight(QFont::DemiBold); f.setFamily("DejaVu Sans Mono");
    const QFontMetrics fm(f); const int cw = fm.horizontalAdvance('0') + 2, sepw = std::max(8, cw / 3);
    const int total = kDigits * cw + 2 * sepw; int x = std::max(0, (width() - total) / 2);
    cells.clear(); sepY = fm.ascent();
    for (int i = 0; i < kDigits; ++i) {                      // most significant first; separators after the 2nd and 5th digit (MHz . kHz . Hz)
        const int place = kDigits - 1 - i; cells.push_back({x, cw, place}); x += cw;
        if (i == 1 || i == 4) x += sepw;
    }
}
int FrequencyDisplay::digitAt(const QPoint& p) const {
    std::vector<Cell> c; int sy; layoutCells(c, sy);
    for (const auto& cell : c) if (p.x() >= cell.x && p.x() < cell.x + cell.w) return cell.place;
    return -1;
}
void FrequencyDisplay::bump(int place, int dir) {
    qint64 step = 1; for (int i = 0; i < place; ++i) step *= 10;
    setHz(double(hz_ + dir * step));
}
void FrequencyDisplay::wheelEvent(QWheelEvent* e) {
    const int place = digitAt(e->position().toPoint()); if (place < 0) { e->ignore(); return; }
    const int dir = e->angleDelta().y() > 0 ? 1 : e->angleDelta().y() < 0 ? -1 : 0; if (dir) bump(place, dir); e->accept();
}
void FrequencyDisplay::mousePressEvent(QMouseEvent* e) {
    const int place = digitAt(e->pos()); if (place < 0 || e->button() != Qt::LeftButton) return;
    bump(place, e->pos().y() < height() / 2 ? 1 : -1);
}
void FrequencyDisplay::mouseMoveEvent(QMouseEvent* e) { const int h = digitAt(e->pos()); if (h != hover_) { hover_ = h; update(); } }

void FrequencyDisplay::paintEvent(QPaintEvent*) {
    QPainter p(this); p.setRenderHint(QPainter::TextAntialiasing);
    QFont f = font(); f.setPixelSize(std::max(24, height() - 16)); f.setWeight(QFont::DemiBold); f.setFamily("DejaVu Sans Mono"); p.setFont(f);
    std::vector<Cell> cells; int sepY; layoutCells(cells, sepY); const QFontMetrics fm(f);
    const int baseline = (height() + fm.ascent() - fm.descent()) / 2;
    const QColor on(label_only_ ? theme::kText : theme::kAccent), dim("#2a3140"), sep(theme::kMuted);
    qint64 v = hz_; bool significant = false;
    for (size_t i = 0; i < cells.size(); ++i) {
        const auto& c = cells[i]; qint64 pw = 1; for (int k = 0; k < c.place; ++k) pw *= 10;
        const int d = int((v / pw) % 10); if (d != 0 || c.place <= 5) significant = true;     // always show kHz and below; dim only the leading MHz zero
        const bool lead_zero = !significant || (c.place >= 6 && v < pw);
        if (c.place == hover_) p.fillRect(QRect(c.x, 4, c.w, height() - 8), QColor(255, 255, 255, 18));
        p.setPen(lead_zero ? dim : on); p.drawText(QRect(c.x, baseline - fm.ascent(), c.w, fm.height()), Qt::AlignHCenter | Qt::AlignTop, QString::number(d));
        if (i == 1 || i == 4) { p.setPen(sep); p.drawText(QRect(c.x + c.w, baseline - fm.ascent(), std::max(8, c.w / 3), fm.height()), Qt::AlignHCenter | Qt::AlignTop, "."); }
    }
}
