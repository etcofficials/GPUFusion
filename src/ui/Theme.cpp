#include "ui/Theme.h"

#include <QApplication>
#include <QPalette>
#include <QStyleFactory>

namespace gf::ui {

namespace {

const ThemeColors kDark{
    QColor(0x12, 0x15, 0x1b), QColor(0x1a, 0x1f, 0x28), QColor(0x22, 0x28, 0x33), QColor(0x2c, 0x34, 0x42),
    QColor(0xe6, 0xe9, 0xef), QColor(0x8b, 0x95, 0xa7), QColor(0x4c, 0x8d, 0xff),
    QColor(0x3d, 0xd6, 0x8c), QColor(0xf2, 0xb3, 0x3d), QColor(0xf0, 0x5d, 0x5e),
    QColor(0x39, 0x87, 0xe5), QColor(0xd9, 0x59, 0x26), QColor(0x2a, 0x31, 0x3d),
};

const ThemeColors kLight{
    QColor(0xf3, 0xf5, 0xf8), QColor(0xff, 0xff, 0xff), QColor(0xec, 0xef, 0xf4), QColor(0xd5, 0xdb, 0xe5),
    QColor(0x1b, 0x20, 0x2a), QColor(0x5d, 0x67, 0x78), QColor(0x2f, 0x6f, 0xe0),
    QColor(0x1f, 0x9d, 0x60), QColor(0xb7, 0x7b, 0x0b), QColor(0xc8, 0x3a, 0x3b),
    QColor(0x2a, 0x78, 0xd6), QColor(0xeb, 0x68, 0x34), QColor(0xe6, 0xe9, 0xee),
};

const ThemeColors* g_current = &kDark;

QString styleSheet(const ThemeColors& c)
{
    // Kept deliberately small: layout comes from widgets; this only sets surfaces and spacing.
    return QStringLiteral(R"(
QWidget { font-size: 10pt; }
QListWidget#navigation { background: %1; border: none; border-right: 1px solid %4; padding: 8px 0; outline: 0; }
QListWidget#navigation::item { color: %6; padding: 9px 18px; margin: 1px 8px; border-radius: 6px; }
QListWidget#navigation::item:hover { background: %3; color: %5; }
QListWidget#navigation::item:selected { background: %3; color: %5; border-left: 3px solid %7; }
QLabel#appTitle { font-size: 13pt; font-weight: 600; color: %5; padding: 16px 18px 4px 18px; }
QLabel#appVersion { color: %6; padding: 0 18px 10px 18px; }
QLabel#pageTitle { font-size: 16pt; font-weight: 600; color: %5; }
QLabel#pageSubtitle { color: %6; }
QLabel#muted { color: %6; }
QLabel#badge { color: %1; background: %8; border-radius: 4px; padding: 2px 8px; font-weight: 600; }
QLabel#badgeNeutral { color: %5; background: %3; border: 1px solid %4; border-radius: 4px; padding: 1px 8px; }
QLabel#badgeAccent { color: white; background: %7; border-radius: 4px; padding: 2px 8px; font-weight: 600; }
QLabel#badgeGood { color: %1; background: %9; border-radius: 4px; padding: 2px 8px; font-weight: 600; }
QLabel#badgeBad { color: white; background: %10; border-radius: 4px; padding: 2px 8px; font-weight: 600; }
QFrame#tile { background: %3; border-radius: 6px; }
QLabel#tileLabel { color: %6; }
#tileValue { font-size: 16pt; font-weight: 600; }
#tileSub { font-size: 9pt; }
QLabel#sectionTitle { color: %6; font-size: 9pt; font-weight: 600; text-transform: uppercase; letter-spacing: 0.5px; }
QLabel#errorBanner { background: %3; color: %5; border: 1px solid %10; border-radius: 6px; padding: 8px 10px; }
QFrame#card { background: %2; border: 1px solid %4; border-radius: 8px; }
QLabel#cardTitle { font-size: 11pt; font-weight: 600; color: %5; }
QLabel#warningBanner { background: %3; color: %5; border: 1px solid %8; border-radius: 6px; padding: 8px 10px; }
QScrollArea { border: none; background: transparent; }
QScrollArea > QWidget > QWidget { background: transparent; }
QPushButton { padding: 6px 14px; border-radius: 5px; border: 1px solid %4; background: %3; color: %5; }
QPushButton:hover { border-color: %7; }
QPushButton:default { background: %7; border-color: %7; color: white; }
QPushButton:disabled { color: %6; }
QPushButton:default:disabled { background: %3; border-color: %4; color: %6; }
QComboBox { padding: 4px 6px; border: 1px solid %4; border-radius: 4px; background: %2; color: %5; min-height: 20px; }
QSpinBox { min-height: 24px; }
QStatusBar { color: %6; }
)")
        .arg(c.window.name(), c.surface.name(), c.surfaceAlt.name(), c.border.name(), c.text.name(),
             c.textMuted.name(), c.accent.name(), c.warn.name(), c.good.name(), c.bad.name());
}

} // namespace

void applyTheme(QApplication& app, const QString& name)
{
    g_current = name == QLatin1StringView("light") ? &kLight : &kDark;
    const ThemeColors& c = *g_current;

    app.setStyle(QStyleFactory::create(QStringLiteral("Fusion")));
    QPalette p;
    p.setColor(QPalette::Window, c.window);
    p.setColor(QPalette::WindowText, c.text);
    p.setColor(QPalette::Base, c.surface);
    p.setColor(QPalette::AlternateBase, c.surfaceAlt);
    p.setColor(QPalette::Text, c.text);
    p.setColor(QPalette::Button, c.surfaceAlt);
    p.setColor(QPalette::ButtonText, c.text);
    p.setColor(QPalette::ToolTipBase, c.surfaceAlt);
    p.setColor(QPalette::ToolTipText, c.text);
    p.setColor(QPalette::PlaceholderText, c.textMuted);
    p.setColor(QPalette::Highlight, c.accent);
    p.setColor(QPalette::HighlightedText, Qt::white);
    p.setColor(QPalette::Link, c.accent);
    p.setColor(QPalette::Mid, c.border);
    p.setColor(QPalette::Disabled, QPalette::Text, c.textMuted);
    p.setColor(QPalette::Disabled, QPalette::WindowText, c.textMuted);
    p.setColor(QPalette::Disabled, QPalette::ButtonText, c.textMuted);
    app.setPalette(p);
    app.setStyleSheet(styleSheet(c));
}

const ThemeColors& colors() { return *g_current; }

} // namespace gf::ui
