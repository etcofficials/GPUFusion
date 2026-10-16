#include "benchmarks/BenchmarkService.h"

#include "core/Logging.h"

#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

#include <memory>

namespace gf::bench {

BenchmarkService::BenchmarkService(const QString& benchmarksDir, QObject* parent) : QObject(parent), m_dir(benchmarksDir)
{
    qRegisterMetaType<Session>();
    reloadHistory();
}

BenchmarkService::~BenchmarkService()
{
    m_cancel = true;
    if (m_thread)
        m_thread->wait();
}

void BenchmarkService::reloadHistory()
{
    m_history.clear();
    QDirIterator it(QDir(m_dir).filePath(QStringLiteral("sessions")), { QStringLiteral("*.json") }, QDir::Files);
    while (it.hasNext()) {
        QFile f(it.next());
        if (!f.open(QIODevice::ReadOnly))
            continue;
        if (auto s = Session::fromJson(QJsonDocument::fromJson(f.readAll()).object()))
            m_history << *s;
    }
    std::sort(m_history.begin(), m_history.end(), [](const Session& a, const Session& b) { return a.started > b.started; });
}

void BenchmarkService::start(const QList<BenchmarkSpec>& specs, const RunOptions& options, const hw::DetectionResult& detection, int retentionDays)
{
    startJob([specs, options, detection](const Progress& progress, const std::atomic<bool>* cancel) {
        return runBenchmarks(specs, options, detection, progress, cancel);
    }, retentionDays);
}

void BenchmarkService::startJob(Job job, int retentionDays)
{
    if (m_thread)
        return;
    m_cancel = false;
    // The session is handed over when the thread has finished, so isRunning() is already false
    // when `finished` is emitted and listeners can start the next run straight away.
    auto result = std::make_shared<Session>();
    QThread* thread = QThread::create([this, job = std::move(job), result] {
        *result = job([this](int done, int total, const QString& step) {
            QMetaObject::invokeMethod(this, [this, done, total, step] { Q_EMIT progress(done, total, step); }, Qt::QueuedConnection);
        }, &m_cancel);
    });
    thread->setObjectName(QStringLiteral("gpufusion-benchmark"));
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this, result, retentionDays] {
        m_thread = nullptr;
        save(*result, retentionDays);
        Q_EMIT finished(*result);
    });
    m_thread = thread;
    thread->start();
}

void BenchmarkService::save(const Session& session, int retentionDays)
{
    m_saveError.clear();
    const QString sessions = QDir(m_dir).filePath(QStringLiteral("sessions"));
    QDir().mkpath(sessions);
    QSaveFile f(QDir(sessions).filePath(session.id + QStringLiteral(".json")));
    if (f.open(QIODevice::WriteOnly)) {
        f.write(QJsonDocument(session.toJson()).toJson(QJsonDocument::Indented));
        if (!f.commit())
            m_saveError = f.errorString();
    } else {
        m_saveError = f.errorString();
    }
    const QString csvPath = QDir(m_dir).filePath(QStringLiteral("results.csv"));
    QFile csv(csvPath);
    const bool fresh = !csv.exists();
    if (csv.open(QIODevice::Append | QIODevice::Text)) {
        if (fresh)
            csv.write(csvHeader().toUtf8());
        csv.write(csvRows(session).toUtf8());
    } else if (m_saveError.isEmpty()) {
        m_saveError = csv.errorString();
    }
    if (!m_saveError.isEmpty())
        qCWarning(lcBench).noquote() << "Saving benchmark results failed:" << m_saveError;
    else
        qCInfo(lcBench).noquote() << "Benchmark session saved:" << QDir::toNativeSeparators(f.fileName());

    // Retention: whole session files only; the CSV keeps its history (it is small).
    QList<std::pair<QString, QDateTime>> files;
    for (const QFileInfo& fi : QDir(sessions).entryInfoList({ QStringLiteral("*.json") }, QDir::Files))
        files.append({ fi.absoluteFilePath(), fi.lastModified() });
    for (const QString& old : filesToPrune(files, QDateTime::currentDateTime(), retentionDays)) {
        if (QFile::remove(old))
            qCInfo(lcBench).noquote() << "Removed benchmark session older than the retention period:" << QDir::toNativeSeparators(old);
    }
    reloadHistory();
}

} // namespace gf::bench
