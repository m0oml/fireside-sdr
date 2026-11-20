#include "SMeter.h"
#include <QDateTime>
#include <QPainter>
#include <algorithm>
#include <cmath>
#include "Theme.h"

void SMeter::setLevel(double db) {
    level_ = db; const qint64 now = QDateTime::currentMSecsSinceEpoch();
    if (db >= peak_) { peak_ = db; peak_t_ = now; }                                 // new peak: hold it for 1.2 s, then let it fall
    else if (now - peak_t_ > 1200) peak_ = std::max(db, peak_ - 0.5);
    update();
}
QString SMeter::sText(double dbm) {
    if (dbm >= -73) { const int over = int(std::lround(dbm + 73)); return over < 5 ? "S9" : QString("S9+%1").arg(over); }
    return QString("S%1").arg(std::clamp(int(std::floor((dbm + 127) / 6.0)), 0, 9));
}
void SMeter::paintEvent(QPaintEvent*) {
    QPainter p(this); p.setRenderHint(QPainter::Antialiasing);
    const double lo = -127, hi = -13; const int w = width(), barY = 4, barH = 18, left = 2, right = w - 2;           // S1 (-121 dBm) .. S9 (-73) .. S9+60 (-13)
    auto xFor = [&](double dbm) { return left + int((std::clamp(dbm, lo, hi) - lo) / (hi - lo) * double(right - left)); };
    const double lvl = dbm(level_), pk = dbm(peak_);
    const int segs = 38; const double segw = double(right - left) / segs;
    for (int i = 0; i < segs; ++i) {                                              // accent up to S9 (-73 dBm), amber above
        const double d = lo + (hi - lo) * (double(i) + 0.5) / segs; const bool on = d <= lvl;
        QColor c = d > -73 ? QColor(theme::kWarn) : QColor(theme::kAccent);
        if (!on) c = QColor("#222938");
        p.fillRect(QRectF(left + i * segw + 1, barY, segw - 2, barH), c);
    }
    if (dbm(peak_) > lo) { p.setPen(QPen(QColor("#ffffff"), 2)); p.drawLine(xFor(pk), barY - 2, xFor(pk), barY + barH + 2); }
    p.setPen(QColor(theme::kMuted)); QFont f = font(); f.setPixelSize(10); p.setFont(f);
    struct T { double dbm; const char* s; };
    for (const T& t : {T{-121, "S1"}, T{-109, "3"}, T{-97, "5"}, T{-85, "7"}, T{-73, "9"}, T{-53, "+20"}, T{-33, "+40"}, T{-13, "+60"}}) {
        const int x = xFor(t.dbm); p.drawLine(x, barY + barH + 2, x, barY + barH + 5);
        const int lx = std::clamp(x - 20, 0, w - 40); p.drawText(QRect(lx, barY + barH + 5, 40, 12), Qt::AlignHCenter, t.s);
    }
}
