#pragma once

#include <QColor>
#include <QString>

class QApplication;

namespace gf::ui {

// Semantic colours shared by custom-painted widgets (graphs, status dots).
struct ThemeColors {
    QColor window, surface, surfaceAlt, border, text, textMuted, accent, good, warn, bad;
    // Charts: categorical series slots (validated for CVD separation and 3:1 contrast against
    // `surface` in both themes with the dataviz palette validator), hairline grid.
    QColor series1, series2, grid;
};

void applyTheme(QApplication& app, const QString& name); // "dark" | "light"
const ThemeColors& colors();

} // namespace gf::ui
