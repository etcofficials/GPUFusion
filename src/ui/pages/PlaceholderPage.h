#pragma once

#include <QStringList>
#include <QWidget>

namespace gf::ui {

// Shown for sections that are not implemented yet. It states that plainly, names the phase
// that will deliver the feature, and lists what is planned — it never shows sample data.
class PlaceholderPage : public QWidget {
    Q_OBJECT
public:
    PlaceholderPage(int phase, const QString& summary, const QStringList& planned, QWidget* parent = nullptr);
};

} // namespace gf::ui
