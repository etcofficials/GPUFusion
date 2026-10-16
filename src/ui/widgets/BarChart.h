#pragma once

#include <QWidget>

#include <optional>

namespace gf::ui {

// Horizontal bars on one value axis starting at zero (one series, so no legend): one bar per row,
// a whisker for ± one standard deviation, the value at the bar's end, and optional vertical marker
// lines (e.g. "GT 730 alone", "ideal split"). Rows without a valid value show a note instead of a
// bar — never a zero-length bar.
class BarChart : public QWidget {
    Q_OBJECT
public:
    struct Bar {
        QString label;
        std::optional<double> value;    // nullopt: no valid measurement
        double error = 0;               // ± whisker
        QString note;                   // shown instead of a bar when value is empty ("Failed", ...)
        bool emphasized = false;        // drawn at full strength; others slightly lighter
    };
    struct Marker {
        double value = 0;
        QString label;
        bool dashed = false;
    };

    explicit BarChart(QWidget* parent = nullptr);
    void setData(QList<Bar> bars, QList<Marker> markers, const QString& unit);

    QSize sizeHint() const override;
    QSize minimumSizeHint() const override { return { 320, sizeHint().height() }; }

private:
    int topRoom() const { return 8 + int(m_markers.size()) * fontMetrics().height(); }

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    QList<Bar> m_bars;
    QList<Marker> m_markers;
    QString m_unit;
};

} // namespace gf::ui
