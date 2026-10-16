#pragma once

#include <QWidget>

class QLabel;
class QVBoxLayout;

namespace gf::ui {

class AppContext;

// The graphics system explained: issues with suggested actions, adapters and APIs, metric
// availability, multi-GPU compatibility, the GPU-preference verdict, application/storage facts and
// recent log problems. Built from the same report object that is exported, so the page and the
// exported file always agree. Rebuilt when shown and when hardware, monitoring providers or
// preferences change — not on every monitoring sample.
class DiagnosticsPage : public QWidget {
    Q_OBJECT
public:
    explicit DiagnosticsPage(AppContext& context, QWidget* parent = nullptr);

protected:
    void showEvent(QShowEvent* event) override;

private:
    void refresh();
    void refreshIfVisible();
    void exportReport();
    void copyReport();

    AppContext& m_context;
    QLabel* m_summary;
    QLabel* m_status;
    QVBoxLayout* m_sections;
    bool m_dirty = true;
};

} // namespace gf::ui
