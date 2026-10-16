#include "ui/widgets/MonitorCard.h"

#include "monitoring/History.h"
#include "ui/Theme.h"
#include "ui/widgets/HistoryGraph.h"
#include "ui/widgets/LiveText.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QStyle>
#include <QVBoxLayout>

#include <cmath>

namespace gf::ui {

using namespace gf::mon;

// ---- MetricTile ------------------------------------------------------------------------------

MetricTile::MetricTile(const QString& label, QWidget* parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("tile"));
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(12, 10, 12, 10);
    layout->setSpacing(2);
    m_label = new QLabel(label);
    m_label->setObjectName(QStringLiteral("tileLabel"));
    // Fonts come from the stylesheet (#tileValue / #tileSub): it overrides QWidget::setFont.
    m_value = new LiveText(1);
    m_value->setObjectName(QStringLiteral("tileValue"));
    m_value->setTone(LiveText::Tone::Muted);
    m_value->setText(tr("Unknown"));
    m_sub = new LiveText(2);
    m_sub->setObjectName(QStringLiteral("tileSub"));
    m_sub->setTone(LiveText::Tone::Muted);
    layout->addWidget(m_label);
    layout->addWidget(m_value);
    layout->addWidget(m_sub);
    layout->addStretch();
}

void MetricTile::setReading(const MetricValue& v, Unit unit, const QString& subText)
{
    // Only repaints: no layout work per sample (see LiveText).
    const QString text = formatMetric(v, unit);
    m_value->setTone(v.isOk() ? LiveText::Tone::Normal : LiveText::Tone::Muted);
    m_value->setText(text);
    m_sub->setText(v.isOk() ? subText : v.note);
    const QString tip = v.isOk() ? v.note : QStringLiteral("%1: %2").arg(stateName(v.state), v.note);
    if (toolTip() != tip)
        setToolTip(tip);
    setAccessibleName(QStringLiteral("%1: %2").arg(m_label->text(), text));
}

// ---- EngineBars --------------------------------------------------------------------------------

namespace {
constexpr int kEngineRow = 22;
}

EngineBars::EngineBars(QWidget* parent) : QWidget(parent)
{
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

void EngineBars::setEngines(const QList<EngineSample>& engines)
{
    const bool resize = engines.size() != m_engines.size();
    m_engines = engines;
    QStringList parts;
    for (const EngineSample& e : engines)
        parts << QStringLiteral("%1 %2").arg(e.name, formatMetric(e.utilization, Unit::Percent));
    setAccessibleDescription(parts.join(QStringLiteral(", ")));
    if (resize)
        updateGeometry();
    update();
}

QSize EngineBars::sizeHint() const
{
    const int rows = (int(m_engines.size()) + 1) / 2;
    return { 400, std::max(1, rows) * kEngineRow };
}

void EngineBars::paintEvent(QPaintEvent*)
{
    const ThemeColors& c = colors();
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    const QFontMetrics fm(font());
    const int columns = 2;
    const double colWidth = (width() - 24.0) / columns;
    const double nameWidth = std::min(colWidth * 0.42, 130.0);
    const double valueWidth = 72;
    for (int i = 0; i < m_engines.size(); ++i) {
        const EngineSample& e = m_engines[i];
        const double x = (i % columns) * (colWidth + 24);
        const double y = (i / columns) * kEngineRow;
        p.setPen(c.textMuted);
        p.drawText(QRectF(x, y, nameWidth - 6, kEngineRow), Qt::AlignLeft | Qt::AlignVCenter,
                   fm.elidedText(e.name, Qt::ElideRight, int(nameWidth - 6)));
        const QRectF track(x + nameWidth, y + kEngineRow / 2.0 - 4, colWidth - nameWidth - valueWidth, 8);
        p.setPen(Qt::NoPen);
        p.setBrush(c.surfaceAlt);
        p.drawRoundedRect(track, 4, 4);
        if (e.utilization.isOk() && e.utilization.value > 0) {
            QRectF fill = track;
            fill.setWidth(std::max(8.0, track.width() * e.utilization.value / 100.0));
            p.setBrush(c.series1);
            p.drawRoundedRect(fill, 4, 4);
        }
        p.setPen(e.utilization.isOk() ? c.text : c.textMuted);
        p.drawText(QRectF(track.right() + 8, y, valueWidth - 8, kEngineRow), Qt::AlignLeft | Qt::AlignVCenter,
                   formatMetric(e.utilization, Unit::Percent));
    }
}

// ---- MonitorCard -------------------------------------------------------------------------------

namespace {

QLabel* sectionLabel(const QString& text)
{
    auto* l = new QLabel(text);
    l->setObjectName(QStringLiteral("sectionTitle"));
    return l;
}

QVector<HistoryPoint> copyOf(const RingBuffer<HistoryPoint>* series)
{
    QVector<HistoryPoint> out;
    if (!series)
        return out;
    out.reserve(series->size());
    for (int i = 0; i < series->size(); ++i)
        out << series->at(i);
    return out;
}

// Memory is plotted in one unit chosen from the data so axis ticks land on clean numbers
// (100 / 200 / 300 MB rather than 191 / 381 / 572 MB).
struct ByteUnit { double divisor; QString suffix; };

ByteUnit byteUnitFor(const QList<QVector<HistoryPoint>>& series)
{
    double hi = 0;
    for (const auto& s : series) {
        for (const auto& p : s) {
            if (!std::isnan(p.value))
                hi = std::max(hi, double(p.value));
        }
    }
    if (hi >= double(1ull << 30))
        return { double(1ull << 30), QStringLiteral("GB") };
    if (hi >= double(1ull << 20))
        return { double(1ull << 20), QStringLiteral("MB") };
    return { 1024.0, QStringLiteral("KB") };
}

QVector<HistoryPoint> scaled(QVector<HistoryPoint> points, double divisor)
{
    for (auto& p : points)
        p.value = float(p.value / divisor); // NaN stays NaN
    return points;
}

} // namespace

MonitorCard::MonitorCard(const MonitorTarget& target, const QString& classification, QWidget* parent)
    : Card(target.name, parent), m_target(target)
{
    addBadge(classification, classification == tr("Discrete") ? Tone::Accent : Tone::Neutral);

    auto* tiles = new QGridLayout;
    tiles->setSpacing(10);
    m_utilization = new MetricTile(tr("GPU utilization"));
    m_dedicated = new MetricTile(tr("Dedicated memory in use"));
    m_shared = new MetricTile(tr("Shared memory in use"));
    m_temperature = new MetricTile(tr("Temperature"));
    int col = 0;
    for (MetricTile* t : { m_utilization, m_dedicated, m_shared, m_temperature }) {
        tiles->addWidget(t, 0, col);
        tiles->setColumnStretch(col++, 1);
    }
    body()->addLayout(tiles);

    m_sensors = new LiveText(2);
    m_sensors->setTone(LiveText::Tone::Muted);
    body()->addWidget(m_sensors);

    auto* graphs = new QHBoxLayout;
    graphs->setSpacing(20);
    auto* utilBox = new QVBoxLayout;
    m_utilTitle = sectionLabel(tr("Utilization history"));
    m_utilGraph = new HistoryGraph;
    m_utilGraph->setYRange(0, 100);
    m_utilGraph->setFormatter([](double v) { return QStringLiteral("%1%").arg(qRound(v)); });
    m_utilGraph->setAccessibleName(tr("%1 utilization history").arg(target.name));
    utilBox->addWidget(m_utilTitle);
    utilBox->addWidget(m_utilGraph);
    auto* memBox = new QVBoxLayout;
    m_memTitle = sectionLabel(tr("Memory history"));
    m_memGraph = new HistoryGraph;
    m_memGraph->setAccessibleName(tr("%1 memory history").arg(target.name));
    memBox->addWidget(m_memTitle);
    memBox->addWidget(m_memGraph);
    graphs->addLayout(utilBox, 1);
    graphs->addLayout(memBox, 1);
    body()->addLayout(graphs);

    body()->addWidget(sectionLabel(tr("Engines (current)")));
    m_engines = new EngineBars;
    body()->addWidget(m_engines);

    m_sources = new QLabel;
    m_sources->setObjectName(QStringLiteral("muted"));
    m_sources->setWordWrap(true);
    body()->addWidget(m_sources);
}

void MonitorCard::showSample(const AdapterSample& s, const History& history, qint64 nowMs, int intervalMs, int historySeconds)
{
    m_utilization->setReading(s.utilization, Unit::Percent, tr("busiest engine"));
    m_dedicated->setReading(s.dedicatedUsed, Unit::Bytes, tr("of %1 dedicated").arg(hw::formatBytes(s.dedicatedTotal)));
    m_shared->setReading(s.sharedUsed, Unit::Bytes, tr("of up to %1 system RAM").arg(hw::formatBytes(s.sharedLimit)));
    m_temperature->setReading(s.temperature, Unit::Celsius, tr("GPU core"));

    const ThemeColors& c = colors();
    auto sensor = [&](const QString& name, const MetricValue& v, Unit unit) {
        const QString value = v.isOk() ? formatMetric(v, unit).toHtmlEscaped()
                                       : QStringLiteral("<span style='color:%1'>%2</span>").arg(c.textMuted.name(), stateName(v.state));
        return QStringLiteral("%1 <b>%2</b>").arg(name.toHtmlEscaped(), value);
    };
    m_sensors->setRichText(QStringList{ sensor(tr("Fan"), s.fanPercent, Unit::Percent), sensor(tr("Performance state"), s.pstate, Unit::PState),
                                    sensor(tr("Power"), s.powerWatts, Unit::Watts), sensor(tr("Core clock"), s.coreClockMHz, Unit::MHz),
                                    sensor(tr("Memory clock"), s.memoryClockMHz, Unit::MHz) }.join(QStringLiteral(" &nbsp;·&nbsp; ")));
    // Explain the non-OK sensors once, in plain text.
    QStringList reasons;
    for (const MetricValue* v : { &s.fanPercent, &s.pstate, &s.powerWatts, &s.coreClockMHz, &s.memoryClockMHz }) {
        if (!v->isOk() && !reasons.contains(v->note))
            reasons << v->note;
    }
    if (const QString tip = reasons.join(QLatin1Char('\n')); m_sensors->toolTip() != tip)
        m_sensors->setToolTip(tip);

    const qint64 windowMs = qint64(historySeconds) * 1000;
    const QString window = tr("last %1 min").arg(double(historySeconds) / 60.0, 0, 'g', 3);
    m_utilTitle->setText(tr("Utilization history · %1").arg(window));
    m_memTitle->setText(tr("Memory history · %1").arg(window));

    m_utilGraph->setTimeWindow(windowMs, nowMs, intervalMs);
    m_utilGraph->setEmptyText(s.utilization.isOk() ? QString() : s.utilization.note);
    m_utilGraph->setSeries({ { tr("Utilization"), c.series1, copyOf(history.series(s.luid, Series::Utilization)) } });

    m_memGraph->setTimeWindow(windowMs, nowMs, intervalMs);
    m_memGraph->setEmptyText(s.dedicatedUsed.isOk() ? QString() : s.dedicatedUsed.note);
    const QVector<HistoryPoint> dedicated = copyOf(history.series(s.luid, Series::DedicatedMemory));
    const QVector<HistoryPoint> shared = copyOf(history.series(s.luid, Series::SharedMemory));
    const ByteUnit unit = byteUnitFor({ dedicated, shared });
    m_memGraph->setFormatter([suffix = unit.suffix](double v) {
        QString n = QString::number(v, 'f', std::abs(v) >= 10 ? 0 : 1);
        if (n.endsWith(QLatin1StringView(".0")))
            n.chop(2);
        return QStringLiteral("%1 %2").arg(n, suffix);
    });
    m_memGraph->setSeries({ { tr("Dedicated"), c.series1, scaled(dedicated, unit.divisor) },
                            { tr("Shared"), c.series2, scaled(shared, unit.divisor) } });

    m_engines->setEngines(s.engines);
    const bool namedEngines = std::none_of(s.engines.begin(), s.engines.end(),
                                           [](const EngineSample& e) { return e.name.startsWith(QLatin1StringView("Engine ")); });
    m_sources->setText(tr("Sources: %1.%2 Utilization is the busiest engine, as in Task Manager.")
                           .arg(s.sources.isEmpty() ? tr("none") : s.sources.join(QStringLiteral(", ")),
                                namedEngines ? QString() : tr(" This driver does not name its engines, so they are numbered.")));
}

} // namespace gf::ui
