#pragma once
#include <QFrame>
#include <QLabel>
#include <QString>
#include <QVBoxLayout>

// One calm dark theme with a single accent. Everything on screen follows the same few rules: cards on a 12 px grid, one control height,
// muted small-caps section titles, and the frequency digits as the only large type.
namespace theme {
inline const char* kBg = "#0e1117";
inline const char* kCard = "#161a23";
inline const char* kBorder = "#252b38";
inline const char* kControl = "#202635";
inline const char* kControlHover = "#2a3246";
inline const char* kText = "#d7dce6";
inline const char* kMuted = "#7b8598";
inline const char* kAccent = "#3ea6ff";
inline const char* kAccentDeep = "#1c5f9e";
inline const char* kWarn = "#ffb454";

inline QString stylesheet() {
    return QString(R"(
* { font-family: "Inter", "Noto Sans", "DejaVu Sans"; font-size: 12px; color: %1; }
QMainWindow, QWidget#root, QTabWidget::pane { background: %2; border: 0; }
QTabBar::tab { background: transparent; color: %3; padding: 8px 16px; border: 0; border-bottom: 2px solid transparent; }
QTabBar::tab:selected { color: %1; border-bottom: 2px solid %4; }
QTabBar::tab:hover { color: %1; }
QFrame#card { background: %5; border: 1px solid %6; border-radius: 8px; }
QLabel#cardTitle { color: %3; font-size: 10px; font-weight: 600; letter-spacing: 1px; }
QLabel#muted { color: %3; }
QLabel#status { color: %3; padding: 2px 4px; }
QPushButton { background: %7; border: 1px solid %6; border-radius: 6px; padding: 4px 10px; min-height: 20px; }
QPushButton:hover { background: %8; }
QPushButton:checked { background: %9; border-color: %4; color: #ffffff; }
QPushButton:disabled { color: #566074; background: #1a1f2b; }
QPushButton#primary { background: %9; border-color: %4; color: #ffffff; font-weight: 600; }
QPushButton#primary:hover { background: #2371b6; }
QPushButton[rec="true"] { background: #b5322a; border-color: #e0584e; color: #ffffff; font-weight: 600; }
QPushButton#pill { padding: 4px 6px; min-width: 34px; }
QPushButton#txstop { background: #6e2a2a; border-color: #9c4a4a; color: #f4dede; font-weight: 600; }
QPushButton#txstop:hover { background: #823434; }
QPushButton#txstop:pressed { background: #5a2020; }
QPushButton#txstop:disabled { background: #2a1f24; border-color: #4a3036; color: #7a6066; }
QCheckBox { spacing: 6px; }
QCheckBox::indicator { width: 14px; height: 14px; border-radius: 3px; border: 1px solid #3a4358; background: %7; }
QCheckBox::indicator:checked { background: %4; border-color: %4; }
QComboBox, QSpinBox, QDoubleSpinBox, QLineEdit { background: %7; border: 1px solid %6; border-radius: 6px; padding: 3px 8px; min-height: 20px; }
QComboBox QAbstractItemView { background: %5; border: 1px solid %6; selection-background-color: %9; }
QSlider::groove:horizontal { height: 4px; background: #2c3446; border-radius: 2px; }
QSlider::sub-page:horizontal { background: %4; border-radius: 2px; }
QSlider::handle:horizontal { background: #e9eefb; width: 14px; height: 14px; margin: -5px 0; border-radius: 7px; }
QPlainTextEdit { background: #0b0e14; border: 1px solid %6; border-radius: 6px; font-family: "DejaVu Sans Mono", monospace; }
QLabel#status { font-family: "DejaVu Sans Mono", monospace; }
QScrollBar:vertical { background: #0b0e14; width: 12px; margin: 0; border: 0; }
QScrollBar:horizontal { background: #0b0e14; height: 12px; margin: 0; border: 0; }
QScrollBar::handle:vertical { background: #2c3446; min-height: 28px; border-radius: 5px; margin: 2px; }
QScrollBar::handle:horizontal { background: #2c3446; min-width: 28px; border-radius: 5px; margin: 2px; }
QScrollBar::handle:vertical:hover, QScrollBar::handle:horizontal:hover { background: #3d4860; }
QScrollBar::handle:vertical:pressed, QScrollBar::handle:horizontal:pressed { background: %4; }
QScrollBar::add-line, QScrollBar::sub-line { height: 0; width: 0; border: 0; background: none; }
QScrollBar::add-page, QScrollBar::sub-page { background: transparent; }
QAbstractScrollArea::corner { background: #0b0e14; border: 0; }
QDialog { background: %2; }
QTreeView, QListView, QTableView { background: %5; alternate-background-color: %5; border: 1px solid %6; border-radius: 6px; selection-background-color: %9; selection-color: #ffffff; outline: 0; }
QHeaderView::section { background: %7; color: %3; border: 0; border-right: 1px solid %6; padding: 3px 6px; }
QDialog QToolButton { background: %7; border: 1px solid %6; border-radius: 6px; padding: 3px; }
QDialog QToolButton:hover { background: %8; }
QToolTip { background: %5; color: %1; border: 1px solid %6; }
)").arg(kText, kBg, kMuted, kAccent, kCard, kBorder, kControl, kControlHover, kAccentDeep);
}

// A titled card: small-caps title over the body layout, contents packed to the top. The one building block of every screen.
inline QFrame* makeCard(const QString& title, QLayout* body, bool expand = false) {   // expand: the body takes all spare height (log views)
    auto* f = new QFrame; f->setObjectName("card"); auto* col = new QVBoxLayout(f); col->setContentsMargins(12, 10, 12, 12); col->setSpacing(8);
    auto* t = new QLabel(title.toUpper()); t->setObjectName("cardTitle"); col->addWidget(t); col->addLayout(body, expand ? 1 : 0); if (!expand) col->addStretch(1); return f;
}
}  // namespace theme
