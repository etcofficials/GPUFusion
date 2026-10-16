#include "ui/widgets/BarChart.h"

#include "ui/Theme.h"

#include <QPainter>
#include <QPainterPath>

#include <cmath>

namespace gf::ui {

namespace {

constexpr int kRow = 30;      // row pitch
constexpr int kBar = 14;      // bar thickness
constexpr int kAxis = 22;     // room for tick labels

double niceStep(double range, int targetTicks)
{
    const double raw = range / std::max(1, targetTicks);
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    const double norm = raw / mag;
    return (norm < 1.5 ? 1 : norm < 3 ? 2 : norm < 7 ? 5 : 10) * mag;
}

QString number(double v)
{
    return QString::number(v, 'f', v < 10 ? 2 : v < 100 ? 1 : 0);
}

} // namespace

BarChart::BarChart(QWidget* parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void BarChart::setData(QList<Bar> bars, QList<Marker> markers, const QString& unit)
{
    m_bars = std::move(bars);
    m_markers = std::move(markers);
    m_unit = unit;
    QStringList text;
    for (const Bar& b : m_bars)
        text << (b.value ? QStringLiteral("%1: %2 %3").arg(b.label, number(*b.value), m_unit) : QStringLiteral("%1: %2").arg(b.label, b.note));
    setAccessibleName(tr("Bar chart"));
    setAccessibleDescription(text.join(QStringLiteral("; ")));
    updateGeometry();
    update();
}

QSize BarChart::sizeHint() const
{
    return { 640, topRoom() + int(m_bars.size()) * kRow + kAxis + 4 };
}

void BarChart::paintEvent(QPaintEvent*)
{
    const ThemeColors& c = colors();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QFontMetrics fm = fontMetrics();

    int labelWidth = 0;
    for (const Bar& b : m_bars)
        labelWidth = std::max(labelWidth, fm.horizontalAdvance(b.label));
    labelWidth = std::min(labelWidth + 12, width() * 2 / 5);
    const int valueRoom = fm.horizontalAdvance(QStringLiteral("0000.0 ± 00.0 ") + m_unit) + 8;
    const QRectF plot(labelWidth, topRoom(), std::max(40, width() - labelWidth - valueRoom), m_bars.size() * kRow);

    double maxValue = 0;
    for (const Bar& b : m_bars) {
        if (b.value)
            maxValue = std::max(maxValue, *b.value + b.error);
    }
    for (const Marker& m : m_markers)
        maxValue = std::max(maxValue, m.value);
    if (maxValue <= 0)
        maxValue = 1;
    const double step = niceStep(maxValue, 4);
    const double axisMax = std::ceil(maxValue / step) * step;
    auto xOf = [&](double v) { return plot.left() + plot.width() * v / axisMax; };

    // Grid + tick labels.
    p.setFont(font());
    for (double t = 0; t <= axisMax + step / 2; t += step) {
        const double x = xOf(t);
        p.setPen(QPen(c.grid, 1));
        p.drawLine(QPointF(x, plot.top()), QPointF(x, plot.bottom()));
        p.setPen(c.textMuted);
        const bool last = t + step > axisMax + step / 2; // the unit once, on the last tick, keeps labels apart
        const QString label = last ? QStringLiteral("%1 %2").arg(QString::number(t, 'g', 6), m_unit) : QString::number(t, 'g', 6);
        const int w = fm.horizontalAdvance(label);
        p.drawText(QPointF(std::clamp(x - w / 2.0, plot.left(), double(width() - w)), plot.bottom() + fm.ascent() + 6), label);
    }

    // Bars.
    for (int i = 0; i < m_bars.size(); ++i) {
        const Bar& b = m_bars[i];
        const double yMid = plot.top() + i * kRow + kRow / 2.0;
        p.setPen(c.text);
        p.drawText(QRectF(0, yMid - kRow / 2.0, labelWidth - 10, kRow), Qt::AlignRight | Qt::AlignVCenter,
                   fm.elidedText(b.label, Qt::ElideRight, labelWidth - 10));
        if (!b.value) {
            p.setPen(c.textMuted);
            p.drawText(QRectF(plot.left() + 6, yMid - kRow / 2.0, plot.width(), kRow), Qt::AlignLeft | Qt::AlignVCenter, b.note);
            continue;
        }
        QColor fill = c.series1;
        if (!b.emphasized)
            fill.setAlphaF(0.55f);
        const QRectF bar(plot.left(), yMid - kBar / 2.0, std::max(1.0, xOf(*b.value) - plot.left()), kBar);
        QPainterPath path;
        path.addRoundedRect(bar, 2, 2);
        p.fillPath(path, fill);
        if (b.error > 0) {
            p.setPen(QPen(c.text, 1));
            const double x0 = xOf(std::max(0.0, *b.value - b.error)), x1 = xOf(*b.value + b.error);
            p.drawLine(QPointF(x0, yMid), QPointF(x1, yMid));
            p.drawLine(QPointF(x0, yMid - 4), QPointF(x0, yMid + 4));
            p.drawLine(QPointF(x1, yMid - 4), QPointF(x1, yMid + 4));
        }
        p.setPen(c.text);
        const QString value = b.error > 0 ? QStringLiteral("%1 ± %2 %3").arg(number(*b.value), number(b.error), m_unit)
                                          : QStringLiteral("%1 %2").arg(number(*b.value), m_unit);
        p.drawText(QPointF(xOf(*b.value + b.error) + 6, yMid + fm.ascent() / 2.0 - 1), value);
    }

    // Markers, each labelled on its own line above the plot (they may be close together).
    for (int i = 0; i < m_markers.size(); ++i) {
        const Marker& m = m_markers[i];
        const double x = xOf(m.value);
        QPen pen(c.textMuted, 1.5);
        if (m.dashed)
            pen.setStyle(Qt::DashLine);
        p.setPen(pen);
        p.drawLine(QPointF(x, 6 + (i + 1) * fm.height()), QPointF(x, plot.bottom()));
        p.setPen(c.textMuted);
        const int w = fm.horizontalAdvance(m.label);
        p.drawText(QPointF(std::clamp(x - w / 2.0, 0.0, double(width() - w)), 4 + fm.ascent() + i * fm.height()), m.label);
    }
}

} // namespace gf::ui
