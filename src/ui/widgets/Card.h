#pragma once

#include <QFrame>

class QHBoxLayout;
class QVBoxLayout;
class QLabel;

namespace gf::ui {

// A titled surface used to group related content on a page.
class Card : public QFrame {
    Q_OBJECT
public:
    enum class Tone { Neutral, Accent, Good, Warning, Bad };

    explicit Card(const QString& title, QWidget* parent = nullptr);

    QVBoxLayout* body() const { return m_body; }
    // Adds a small coloured tag to the right of the title (e.g. "Planned — Phase 3").
    QLabel* addBadge(const QString& text, Tone tone, const QString& toolTip = {});
    void setBadge(const QString& text) { addBadge(text, Tone::Warning); }

private:
    QHBoxLayout* m_header;
    QVBoxLayout* m_body;
};

// Two-column "label: value" grid inside a card; values are selectable for copying.
class InfoGrid : public QWidget {
    Q_OBJECT
public:
    explicit InfoGrid(QWidget* parent = nullptr);
    void addRow(const QString& label, const QString& value);
    void clear();
};

} // namespace gf::ui
