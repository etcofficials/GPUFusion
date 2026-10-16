#pragma once

#include "benchmarks/BenchmarkRunner.h"

#include <QSet>
#include <QWidget>

class QComboBox;
class QLabel;
class QProgressBar;
class QPushButton;
class QTableWidget;
class QTreeWidget;

namespace gf::ui {

class AppContext;

// Choose benchmarks, run them (warm-up + measured runs, on a worker thread, with GPU Fusion's own
// monitoring paused), and read the results: mean, variation, range, output correctness and the
// comparison with the previous equivalent run. Past sessions are listed from the data folder.
class BenchmarksPage : public QWidget {
    Q_OBJECT
public:
    explicit BenchmarksPage(AppContext& context, QWidget* parent = nullptr);

private:
    void rebuildChecklist();
    QList<bench::BenchmarkSpec> selectedSpecs() const;
    void start();
    void onProgress(int done, int total, const QString& step);
    void onFinished(const bench::Session& session);
    void reloadSessions(const QString& selectId);
    void showSession(int index);
    void updateControls();

    AppContext& m_context;
    QTreeWidget* m_tests;
    QComboBox* m_preset;
    QLabel* m_runs;
    QPushButton* m_run;
    QPushButton* m_cancel;
    QProgressBar* m_progress;
    QLabel* m_status;
    QComboBox* m_sessions;
    QLabel* m_sessionInfo;
    QLabel* m_summary;
    QTableWidget* m_table;
    QLabel* m_notes;

    QList<bench::BenchmarkSpec> m_specs;
    QSet<QString> m_unchecked; // user choices survive re-detection
    bool m_resumeMonitoring = false;
};

} // namespace gf::ui
