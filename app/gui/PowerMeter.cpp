#include "PowerMeter.h"
#include <QDateTime>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include "Theme.h"

namespace { constexpr double kMaxW = 30.0; double frac(double w) { return std::sqrt(std::clamp(w, 0.0, kMaxW) / kMaxW); } }   // square-root scale: low powers readable

PowerMeter::PowerMeter(QWidget* parent) : QWidget(parent) { setMinimumSize(300, 66); text_ = "not transmitting"; }
void PowerMeter::setReading(double fwd, double ref, double swr, bool keyed) {
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    keyed_ = keyed; fwd_ = keyed ? fwd : 0; ref_ = keyed ? ref : 0;
    if (fwd_ >= pk_) { pk_ = fwd_; pk_t_ = now; } else if (now - pk_t_ > 1500) pk_ = std::max(fwd_, pk_ * 0.9);
    if (keyed && fwd > 0.05) { last_fwd_ = fwd; last_ref_ = ref; last_swr_ = swr; }
    auto sw = [](double s) { return s >= 1 ? QString::number(s, 'f', 2) : QString("-"); };
    text_ = keyed ? QString("fwd %1 W   ref %2 W   SWR %3").arg(fwd, 0, 'f', 1).arg(ref, 0, 'f', 2).arg(sw(swr))
                  : last_fwd_ > 0 ? QString("last over: fwd %1 W   ref %2 W   SWR %3").arg(last_fwd_, 0, 'f', 1).arg(last_ref_, 0, 'f', 2).arg(sw(last_swr_)) : QString("not transmitting");
    update();
}
void PowerMeter::paintEvent(QPaintEvent*) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    const int w = width(), left = 2, right = w - 2, barH = 10, y1 = 2, y2 = y1 + barH + 3; const int segs = 38; const double segw = double(right - left) / segs;
    auto bar = [&](int y, double watts, const QColor& on) {
        const double f = frac(watts);
        for (int i = 0; i < segs; ++i) { const double c = (double(i) + 0.5) / segs; p.fillRect(QRectF(left + i * segw + 1, y, segw - 2, barH), c <= f && watts > 0 ? on : QColor("#222938")); }
    };
    bar(y1, fwd_, QColor(theme::kAccent)); bar(y2, ref_, QColor(theme::kWarn));
    if (pk_ > 0.05) { const int x = left + int(frac(pk_) * (right - left)); p.setPen(QPen(QColor("#ffffff"), 2)); p.drawLine(x, y1 - 1, x, y1 + barH + 1); }
    p.setPen(QColor(theme::kMuted)); QFont f = font(); f.setPixelSize(10); p.setFont(f);
    const int ty = y2 + barH + 2;
    for (double t : {0.0, 1.0, 2.0, 5.0, 10.0, 15.0, 20.0, 30.0}) {
        const int x = left + int(frac(t) * (right - left)); p.drawLine(x, ty, x, ty + 3);
        const int lx = std::clamp(x - 20, 0, w - 40); p.drawText(QRect(lx, ty + 3, 40, 12), Qt::AlignHCenter, t == 30.0 ? "30 W" : QString::number(t, 'f', 0));
    }
    QFont g = font(); g.setPixelSize(11); p.setFont(g); p.setPen(QColor(keyed_ ? "#e6ebf2" : theme::kMuted)); p.drawText(QRect(0, ty + 17, w, 16), Qt::AlignLeft | Qt::AlignVCenter, text_);
}
