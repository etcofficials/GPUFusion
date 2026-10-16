#include "ui/widgets/LiveText.h"

#include "ui/Theme.h"

#include <QAbstractTextDocumentLayout>
#include <QEvent>
#include <QPainter>
#include <QTextDocument>

namespace gf::ui {

LiveText::LiveText(int lines, QWidget* parent) : QWidget(parent), m_lines(std::max(1, lines))
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    setAttribute(Qt::WA_OpaquePaintEvent, false);
}

void LiveText::setText(const QString& text)
{
    if (text == m_text && !m_rich)
        return;
    m_text = text;
    m_rich = false;
    setAccessibleName(text);
    update();
}

void LiveText::setRichText(const QString& html)
{
    if (html == m_text && m_rich)
        return;
    m_text = html;
    m_rich = true;
    update();
}

void LiveText::setTone(Tone tone)
{
    if (tone != m_tone) {
        m_tone = tone;
        update();
    }
}

QSize LiveText::sizeHint() const
{
    const QFontMetrics fm(font());
    return { fm.averageCharWidth() * 12, fm.lineSpacing() * m_lines + 2 };
}

QSize LiveText::minimumSizeHint() const
{
    const QFontMetrics fm(font());
    return { fm.averageCharWidth() * 4, fm.lineSpacing() * m_lines + 2 };
}

void LiveText::changeEvent(QEvent* event)
{
    if (event->type() == QEvent::FontChange)
        updateGeometry(); // only when the font changes, never per text update
    QWidget::changeEvent(event);
}

void LiveText::paintEvent(QPaintEvent*)
{
    const ThemeColors& c = colors();
    QPainter p(this);
    const QColor ink = m_tone == Tone::Muted ? c.textMuted : c.text;
    if (m_rich) {
        QTextDocument doc;
        doc.setDefaultFont(font());
        doc.setDocumentMargin(0);
        doc.setDefaultStyleSheet(QStringLiteral("body { color: %1; }").arg(ink.name()));
        doc.setHtml(QStringLiteral("<body>%1</body>").arg(m_text));
        doc.setTextWidth(width());
        p.setClipRect(rect());
        QAbstractTextDocumentLayout::PaintContext ctx;
        ctx.palette.setColor(QPalette::Text, ink);
        doc.documentLayout()->draw(&p, ctx);
        return;
    }
    p.setPen(ink);
    const QFontMetrics fm(font());
    if (m_lines == 1) {
        p.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter, fm.elidedText(m_text, Qt::ElideRight, width()));
    } else {
        p.drawText(rect(), Qt::AlignLeft | Qt::AlignTop | Qt::TextWordWrap, m_text);
    }
}

} // namespace gf::ui
