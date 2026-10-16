#pragma once

#include "benchmarks/BenchmarkRunner.h"

#include <QObject>
#include <QPointer>
#include <QThread>

#include <atomic>

namespace gf::bench {

// Runs benchmark sessions on a worker thread and stores them under <data>\benchmarks:
//   sessions\<id>.json   one file per session (full detail, used for baselines)
//   results.csv          one row per result, appended (for spreadsheets)
// Files older than the retention period are removed when a session is saved.
class BenchmarkService : public QObject {
    Q_OBJECT
public:
    BenchmarkService(const QString& benchmarksDir, QObject* parent = nullptr);
    ~BenchmarkService() override;

    bool isRunning() const { return m_thread != nullptr; }
    void start(const QList<BenchmarkSpec>& specs, const RunOptions& options, const hw::DetectionResult& detection, int retentionDays);
    // Runs any job that produces a session (e.g. a Workload Lab experiment) the same way.
    using Job = std::function<Session(const Progress& progress, const std::atomic<bool>* cancel)>;
    void startJob(Job job, int retentionDays);
    void cancel() { m_cancel = true; }

    const QList<Session>& history() const { return m_history; } // newest first
    void reloadHistory();
    QString benchmarksDir() const { return m_dir; }
    QString lastSaveError() const { return m_saveError; }

Q_SIGNALS:
    void progress(int done, int total, const QString& step);
    void finished(const gf::bench::Session& session);

private:
    void save(const Session& session, int retentionDays);

    QString m_dir;
    QList<Session> m_history;
    QPointer<QThread> m_thread;
    std::atomic<bool> m_cancel{ false };
    QString m_saveError;
};

} // namespace gf::bench

Q_DECLARE_METATYPE(gf::bench::Session)
