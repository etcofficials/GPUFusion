#include "ui/widgets/HistoryGraph.h"

#include "ui/Theme.h"

#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QToolTip>

#include <cmath>

namespace gf::ui {

namespace {

constexpr double kTopPad = 8;
constexpr double kLegendHeight = 20;
constexpr double kBottomAxis = 20;

// 1-2-5 "nice" step so ticks land on clean numbers.
double niceStep(double range, int targetTicks)
{
    if (range <= 0)
        return 1;
    const double raw = range / targetTicks;
    const double mag = std::pow(10.0, std::floor(std::log10(raw)));
    for (double m : { 1.0, 2.0, 2.5, 5.0, 10.0 }) {
        if (m * mag >= raw)
            return m * mag;
    }
    return 10 * mag;
}

QString durationText(qint64 ms)
{
    if (ms >= 60'000)
        return QStringLiteral("%1 min").arg(double(ms) / 60'000.0, 0, 'g', 3);
    return QStringLiteral("%1 s").arg(ms / 1000);
}

} // namespace

HistoryGraph::HistoryGraph(QWidget* parent) : QWidget(parent)
{
    setMouseTracking(true);
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Preferred);
    m_format = [](double v) { return QString::number(v, 'f', 0); };
}

void HistoryGraph::setSeries(QList<Series> series)
{
    m_series = std::move(series);
    updateAccessibleText();
    update();
}

void HistoryGraph::setYRange(double min, double max)
{
    m_fixedMin = min;
    m_fixedMax = max;
    update();
}

void HistoryGraph::setTimeWindow(qint64 windowMs, qint64 nowMs, int intervalMs)
{
    m_windowMs = std::max<qint64>(1000, windowMs);
    m_nowMs = nowMs;
    m_intervalMs = std::max(1, intervalMs);
    update();
}

HistoryGraph::Axis HistoryGraph::computeAxis() const
{
    Axis a;
    if (m_fixedMax > m_fixedMin) {
        a.min = m_fixedMin;
        a.max = m_fixedMax;
    } else {
        double hi = 0;
        for (const Series& s : m_series) {
            for (const auto& p : s.points) {
                if (!std::isnan(p.value))
                    hi = std::max(hi, double(p.value));
            }
        }
        a.min = 0;
        const double step = niceStep(hi > 0 ? hi : 1, 4);
        a.max = std::max(step, std::ceil(hi / step) * step);
        if (a.max <= hi) // keep headroom so the line never sits on the top edge
            a.max += step;
    }
    const double step = niceStep(a.max - a.min, 4);
    for (double v = a.min; v <= a.max + step * 1e-6; v += step)
        a.ticks << v;
    return a;
}

QRectF HistoryGraph::plotRect(const Axis& axis) const
{
    const QFontMetrics fm(font());
    int labelWidth = 0;
    for (double t : axis.ticks)
        labelWidth = std::max(labelWidth, fm.horizontalAdvance(m_format(t)));
    // Room on the right for the end-of-line value labels, measured rather than assumed.
    int endWidth = 0;
    for (const Series& s : m_series) {
        if (!s.points.isEmpty() && !std::isnan(s.points.last().value))
            endWidth = std::max(endWidth, fm.horizontalAdvance(m_format(s.points.last().value)));
    }
    const double right = endWidth + 14;
    const double top = kTopPad + (m_series.size() >= 2 ? kLegendHeight : 0);
    return QRectF(labelWidth + 10, top, width() - labelWidth - 10 - right, height() - top - kBottomAxis);
}

double HistoryGraph::xFor(qint64 t, const QRectF& plot) const
{
    return plot.right() - double(m_nowMs - t) / double(m_windowMs) * plot.width();
}

QString HistoryGraph::agoText(qint64 t) const
{
    const qint64 ago = m_nowMs - t;
    return ago < 1000 ? tr("now") : tr("%1 ago").arg(durationText(ago));
}

void HistoryGraph::paintEvent(QPaintEvent*)
{
    const ThemeColors& c = colors();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const Axis axis = computeAxis();
    const QRectF plot = plotRect(axis);
    const QFontMetrics fm(font());
    if (plot.width() < 40 || plot.height() < 30)
        return;
    const auto yFor = [&](double v) { return plot.bottom() - (v - axis.min) / (axis.max - axis.min) * plot.height(); };

    // Grid: hairline, solid, recessive; y labels in muted ink.
    p.setPen(QPen(c.grid, 1));
    for (double t : axis.ticks) {
        const double y = std::round(yFor(t)) + 0.5;
        p.drawLine(QPointF(plot.left(), y), QPointF(plot.right(), y));
    }
    p.setPen(c.textMuted);
    for (double t : axis.ticks) {
        const QString label = m_format(t);
        p.drawText(QRectF(0, yFor(t) - fm.height() / 2.0, plot.left() - 8, fm.height()), Qt::AlignRight | Qt::AlignVCenter, label);
    }
    // Time axis: window start, midpoint, now.
    const QString leftLabel = tr("−%1").arg(durationText(m_windowMs));
    const QString midLabel = tr("−%1").arg(durationText(m_windowMs / 2));
    const double axisY = plot.bottom() + 4;
    p.drawText(QRectF(plot.left(), axisY, 120, fm.height()), Qt::AlignLeft | Qt::AlignTop, leftLabel);
    p.drawText(QRectF(plot.center().x() - 60, axisY, 120, fm.height()), Qt::AlignHCenter | Qt::AlignTop, midLabel);
    p.drawText(QRectF(plot.right() - 120, axisY, 120, fm.height()), Qt::AlignRight | Qt::AlignTop, tr("now"));

    // Legend (only with 2+ series): short line key in the series colour, name in text ink.
    if (m_series.size() >= 2) {
        double x = plot.left();
        for (const Series& s : m_series) {
            p.setPen(QPen(s.color, 2, Qt::SolidLine, Qt::RoundCap));
            p.drawLine(QPointF(x, kTopPad + 8), QPointF(x + 14, kTopPad + 8));
            p.setPen(c.textMuted);
            p.drawText(QPointF(x + 19, kTopPad + 8 + fm.ascent() / 2.0 - 1), s.name);
            x += 19 + fm.horizontalAdvance(s.name) + 18;
        }
    }

    bool anyData = false;
    const qint64 gapMs = qint64(m_intervalMs * 2.5);
    p.save();
    p.setClipRect(plot.adjusted(-6, -6, 6, 6));
    for (const Series& s : m_series) {
        // Build contiguous runs; NaN values and time gaps end a run.
        QList<QPolygonF> runs;
        QPolygonF run;
        qint64 lastT = 0;
        for (const auto& pt : s.points) {
            if (pt.tMs < m_nowMs - m_windowMs)
                continue;
            const bool gap = !run.isEmpty() && pt.tMs - lastT > gapMs;
            if (std::isnan(pt.value) || gap) {
                if (!run.isEmpty())
                    runs << run;
                run.clear();
            }
            if (!std::isnan(pt.value))
                run << QPointF(xFor(pt.tMs, plot), yFor(std::clamp(double(pt.value), axis.min, axis.max)));
            lastT = pt.tMs;
        }
        if (!run.isEmpty())
            runs << run;

        for (const QPolygonF& r : runs) {
            anyData = true;
            if (m_series.size() == 1 && r.size() >= 2) { // ~10% area wash for a single series
                QPainterPath area;
                area.moveTo(r.first().x(), plot.bottom());
                for (const QPointF& q : r)
                    area.lineTo(q);
                area.lineTo(r.last().x(), plot.bottom());
                QColor wash = s.color;
                wash.setAlphaF(0.10f);
                p.fillPath(area, wash);
            }
            p.setPen(QPen(s.color, 2, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
            if (r.size() == 1)
                p.drawPoint(r.first());
            else
                p.drawPolyline(r);
        }

        // End marker + direct label for the latest valid point (selective labelling).
        if (!s.points.isEmpty() && !std::isnan(s.points.last().value) && !runs.isEmpty()) {
            const QPointF end = runs.last().last();
            p.setPen(QPen(c.surface, 2));
            p.setBrush(s.color);
            p.drawEllipse(end, 4.5, 4.5);
            p.setBrush(Qt::NoBrush);
        }
    }
    p.restore();

    // End labels outside the plot, in text ink (never in the series colour).
    for (const Series& s : m_series) {
        if (s.points.isEmpty() || std::isnan(s.points.last().value))
            continue;
        const double y = yFor(std::clamp(double(s.points.last().value), axis.min, axis.max));
        p.setPen(c.text);
        p.drawText(QRectF(plot.right() + 8, y - fm.height() / 2.0, width() - plot.right() - 8, fm.height()),
                   Qt::AlignLeft | Qt::AlignVCenter, m_format(s.points.last().value));
    }

    if (!anyData) {
        p.setPen(c.textMuted);
        p.drawText(plot, Qt::AlignCenter | Qt::TextWordWrap, m_emptyText.isEmpty() ? tr("No data yet") : m_emptyText);
    }

    // Hover crosshair
    if (m_hoverX >= plot.left() && m_hoverX <= plot.right() && anyData) {
        p.setPen(QPen(c.textMuted, 1));
        p.drawLine(QPointF(m_hoverX + 0.5, plot.top()), QPointF(m_hoverX + 0.5, plot.bottom()));
    }
}

void HistoryGraph::mouseMoveEvent(QMouseEvent* event)
{
    const Axis axis = computeAxis();
    const QRectF plot = plotRect(axis);
    const double x = event->position().x();
    if (x < plot.left() || x > plot.right() || m_series.isEmpty()) {
        leaveEvent(event);
        return;
    }
    const qint64 t = m_nowMs - qint64((plot.right() - x) / plot.width() * double(m_windowMs));
    // Nearest sample in time across series.
    QStringList lines;
    qint64 shownT = -1;
    for (const Series& s : m_series) {
        const mon::HistoryPoint* best = nullptr;
        for (const auto& pt : s.points) {
            if (!best || std::llabs(pt.tMs - t) < std::llabs(best->tMs - t))
                best = &pt;
        }
        if (!best || std::llabs(best->tMs - t) > m_intervalMs * 2)
            continue;
        shownT = best->tMs;
        lines << QStringLiteral("%1: %2").arg(s.name, std::isnan(best->value) ? tr("not available") : m_format(best->value));
    }
    if (lines.isEmpty()) {
        leaveEvent(event);
        return;
    }
    m_hoverX = int(xFor(shownT, plot));
    QToolTip::showText(event->globalPosition().toPoint(), QStringLiteral("%1\n%2").arg(agoText(shownT), lines.join(u'\n')), this);
    update();
}

void HistoryGraph::leaveEvent(QEvent*)
{
    if (m_hoverX != -1) {
        m_hoverX = -1;
        QToolTip::hideText();
        update();
    }
}

void HistoryGraph::updateAccessibleText()
{
    QStringList parts;
    for (const Series& s : m_series) {
        if (s.points.isEmpty())
            continue;
        const float v = s.points.last().value;
        parts << QStringLiteral("%1 %2").arg(s.name, std::isnan(v) ? tr("not available") : m_format(v));
    }
    setAccessibleDescription(parts.join(QStringLiteral(", ")));
}

} // namespace gf::ui
