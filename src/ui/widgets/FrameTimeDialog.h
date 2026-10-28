#pragma once

#include "frames/FrameSession.h"
#include "monitoring/MetricModel.h"

#include <QDialog>

#include <atomic>
#include <memory>
#include <thread>

class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTextBrowser;

namespace gf::ui {

// Measures the frame times of one running program from outside (Windows graphics event tracing, through
// the administrator prompt each time), shows what was measured, what was estimated and what was not,
// and compares with an earlier measurement of the same program ("before/after").
class FrameTimeDialog : public QDialog {
    Q_OBJECT
public:
    FrameTimeDialog(quint32 pid, const QString& name, const QString& path, const QList<mon::MonitorTarget>& targets, const QString& framesDir,
                    const QString& reportsDir, const QString& workDir, QWidget* parent = nullptr);
    ~FrameTimeDialog() override;

private:
    void start();
    void finished(const frames::FrameSession& s);
    void reloadEarlier();

    quint32 m_pid;
    QString m_name, m_path, m_framesDir, m_reportsDir, m_workDir, m_report;
    QList<mon::MonitorTarget> m_targets;
    QList<frames::FrameSession> m_earlier;
    QSpinBox* m_seconds;
    QLineEdit* m_label;
    QComboBox* m_compare;
    QPushButton* m_start;
    QPushButton* m_stop;
    QPushButton* m_openReport;
    QProgressBar* m_progress;
    QLabel* m_status;
    QTextBrowser* m_result;
    std::thread m_worker;
    std::shared_ptr<std::atomic<bool>> m_cancel;
};

} // namespace gf::ui
