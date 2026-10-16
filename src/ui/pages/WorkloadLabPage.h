#pragma once

#include <QWidget>

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTableWidget;

namespace gf::bench { struct Session; struct LabAnalysis; }

namespace gf::ui {

class AppContext;
class BarChart;

// The multi-GPU experiments: split compute (one hash image computed by both GPUs, and the CPU) and
// parallel video decoding (one stream per GPU). Each shows a one-sentence answer measured against
// the best single device, the times as bars, a table with correctness and work shares, and the
// evidence (busy engines, GPU performance states). Nothing here changes system settings.
class WorkloadLabPage : public QWidget {
    Q_OBJECT
public:
    explicit WorkloadLabPage(AppContext& context, QWidget* parent = nullptr);

private:
    struct Panel {
        QString kind;            // "split" | "decode"
        QPushButton* run = nullptr;
        QPushButton* cancel = nullptr;
        QProgressBar* progress = nullptr;
        QLabel* status = nullptr;
        QLabel* headline = nullptr;
        QLabel* when = nullptr;
        BarChart* chart = nullptr;
        QLabel* chartCaption = nullptr;
        QTableWidget* table = nullptr;
        QLabel* findings = nullptr;
    };

    QWidget* buildPanel(Panel& panel, const QString& title, const QString& description, QComboBox* size);
    void start(Panel& panel);
    void onFinished(const bench::Session& session);
    void showLatest();
    void showPanel(Panel& panel, const bench::Session* session);
    void updateControls();

    AppContext& m_context;
    Panel m_split, m_decode;
    QComboBox* m_size = nullptr;
    Panel* m_running = nullptr;
    bool m_resumeMonitoring = false;
};

} // namespace gf::ui
