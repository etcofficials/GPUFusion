#pragma once

#include "monitoring/MetricModel.h"
#include "ui/widgets/Card.h"

class QLabel;
class QGridLayout;

namespace gf::mon { class History; }

namespace gf::ui {

class HistoryGraph;
class LiveText;

// A labelled current reading. Non-OK readings show their state in muted ink with the reason as
// a tooltip — never a number.
class MetricTile : public QFrame {
    Q_OBJECT
public:
    explicit MetricTile(const QString& label, QWidget* parent = nullptr);
    void setReading(const mon::MetricValue& value, mon::Unit unit, const QString& subText = {});

private:
    QLabel* m_label;
    LiveText* m_value;
    LiveText* m_sub;
};

// Per-engine utilization as horizontal bars (current values only).
class EngineBars : public QWidget {
    Q_OBJECT
public:
    explicit EngineBars(QWidget* parent = nullptr);
    void setEngines(const QList<mon::EngineSample>& engines);
    QSize sizeHint() const override;

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QList<mon::EngineSample> m_engines;
};

// Live view of one adapter: current readings, history graphs, engines, sources.
class MonitorCard : public Card {
    Q_OBJECT
public:
    MonitorCard(const mon::MonitorTarget& target, const QString& classification, QWidget* parent = nullptr);
    void showSample(const mon::AdapterSample& sample, const mon::History& history, qint64 nowMs, int intervalMs, int historySeconds);

private:
    mon::MonitorTarget m_target;
    MetricTile* m_utilization;
    MetricTile* m_dedicated;
    MetricTile* m_shared;
    MetricTile* m_temperature;
    LiveText* m_sensors;
    HistoryGraph* m_utilGraph;
    HistoryGraph* m_memGraph;
    QLabel* m_utilTitle;
    QLabel* m_memTitle;
    EngineBars* m_engines;
    QLabel* m_sources;
};

} // namespace gf::ui
