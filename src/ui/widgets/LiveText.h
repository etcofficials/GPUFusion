#pragma once

#include <QWidget>

namespace gf::ui {

// Text that changes every monitoring sample. Unlike QLabel::setText, setText here only repaints:
// it never invalidates layouts. (Measured: per-sample QLabel updates caused ~9 relayouts/s and
// ~35 ms of UI-thread CPU per second on the dashboard; see docs/TESTING.md.) The height is fixed
// at `lines` text lines; longer text is elided (plain) or clipped to those lines (rich).
class LiveText : public QWidget {
    Q_OBJECT
public:
    enum class Tone { Normal, Muted };

    explicit LiveText(int lines = 1, QWidget* parent = nullptr);

    void setText(const QString& text);
    void setRichText(const QString& html); // simple HTML (<b>, <span style='color:...'>)
    void setTone(Tone tone);
    QString text() const { return m_text; }

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;
    void changeEvent(QEvent* event) override;

private:
    QString m_text;
    bool m_rich = false;
    Tone m_tone = Tone::Normal;
    int m_lines;
};

} // namespace gf::ui
