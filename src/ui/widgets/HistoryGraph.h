#pragma once

#include "monitoring/History.h"

#include <QColor>
#include <QVector>
#include <QWidget>

#include <functional>

namespace gf::ui {

// Line chart of recent history on a single y-axis. Missing readings (NaN) and gaps in time
// (pauses) break the line instead of being drawn as zero. Hovering shows a crosshair and the
// values at that time. Designed for ≤ 2 series (a legend appears from 2 series).
class HistoryGraph : public QWidget {
    Q_OBJECT
public:
    struct Series {
        QString name;
        QColor color;
        QVector<mon::HistoryPoint> points;
    };

    explicit HistoryGraph(QWidget* parent = nullptr);

    void setSeries(QList<Series> series);
    // Fixed y range (e.g. 0..100 %); when max <= min the range is fitted to the data.
    void setYRange(double min, double max);
    void setFormatter(std::function<QString(double)> format) { m_format = std::move(format); }
    // Time window shown, the "now" instant at the right edge and the sampling interval (for gaps).
    void setTimeWindow(qint64 windowMs, qint64 nowMs, int intervalMs);
    void setEmptyText(const QString& text) { m_emptyText = text; update(); }

    QSize minimumSizeHint() const override { return { 220, 150 }; }
    QSize sizeHint() const override { return { 420, 170 }; }

protected:
    void paintEvent(QPaintEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void leaveEvent(QEvent* event) override;

private:
    struct Axis { double min = 0, max = 1; QList<double> ticks; };
    Axis computeAxis() const;
    QRectF plotRect(const Axis& axis) const;
    double xFor(qint64 t, const QRectF& plot) const;
    QString agoText(qint64 t) const;
    void updateAccessibleText();

    QList<Series> m_series;
    double m_fixedMin = 0, m_fixedMax = 0;
    std::function<QString(double)> m_format;
    qint64 m_windowMs = 300'000, m_nowMs = 0;
    int m_intervalMs = 1000;
    QString m_emptyText;
    int m_hoverX = -1;
};

} // namespace gf::ui
