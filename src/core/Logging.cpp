#include "core/Logging.h"

#include <QDir>
#include <QFileInfo>
#include <QMutexLocker>
#include <QThread>

#include <atomic>
#include <cstdlib>

#include <windows.h>

Q_LOGGING_CATEGORY(lcApp, "gpufusion.app")
Q_LOGGING_CATEGORY(lcSettings, "gpufusion.settings")
Q_LOGGING_CATEGORY(lcUi, "gpufusion.ui")
Q_LOGGING_CATEGORY(lcHardware, "gpufusion.hardware")
Q_LOGGING_CATEGORY(lcMonitor, "gpufusion.monitor")
Q_LOGGING_CATEGORY(lcPrefs, "gpufusion.preferences")
Q_LOGGING_CATEGORY(lcBench, "gpufusion.benchmarks")

namespace gf::log {

QString levelName(Level level)
{
    switch (level) {
    case Level::Debug:   return QStringLiteral("debug");
    case Level::Info:    return QStringLiteral("info");
    case Level::Warning: return QStringLiteral("warning");
    case Level::Error:   return QStringLiteral("error");
    }
    return QStringLiteral("info");
}

bool parseLevel(const QString& text, Level* out)
{
    for (Level l : { Level::Debug, Level::Info, Level::Warning, Level::Error }) {
        if (text.compare(levelName(l), Qt::CaseInsensitive) == 0) {
            *out = l;
            return true;
        }
    }
    return false;
}

// ---- RotatingFileLog --------------------------------------------------------

QString RotatingFileLog::formatLine(const QDateTime& time, Level level, const QString& subsystem,
                                    quintptr threadId, const QString& message)
{
    static const char* const kTags[] = { "DEBUG", "INFO ", "WARN ", "ERROR" };
    QString text = message;
    text.replace(u'\n', QStringLiteral("\n    ")); // keep continuation lines visibly attached
    return QStringLiteral("%1 %2 [%3] (t%4) %5\n")
        .arg(time.toString(Qt::ISODateWithMs), QLatin1StringView(kTags[static_cast<int>(level)]), subsystem)
        .arg(threadId)
        .arg(text);
}

bool RotatingFileLog::open(const Options& options, QString* error)
{
    QMutexLocker lock(&m_mutex);
    m_options = options;
    m_options.maxFiles = std::max(1, m_options.maxFiles);
    m_options.maxFileBytes = std::max<qint64>(4096, m_options.maxFileBytes);
    if (!QDir().mkpath(m_options.directory)) {
        if (error) *error = QStringLiteral("cannot create log folder %1").arg(QDir::toNativeSeparators(m_options.directory));
        return false;
    }
    m_file.close();
    m_file.setFileName(pathFor(0));
    if (!m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        if (error) *error = QStringLiteral("cannot open %1: %2").arg(QDir::toNativeSeparators(m_file.fileName()), m_file.errorString());
        return false;
    }
    return true;
}

void RotatingFileLog::setLimits(qint64 maxFileBytes, int maxFiles)
{
    QMutexLocker lock(&m_mutex);
    m_options.maxFileBytes = std::max<qint64>(4096, maxFileBytes);
    m_options.maxFiles = std::max(1, maxFiles);
}

QString RotatingFileLog::pathFor(int index) const
{
    const QString name = index == 0 ? QStringLiteral("%1.log").arg(m_options.baseName)
                                    : QStringLiteral("%1.%2.log").arg(m_options.baseName).arg(index);
    return QDir(m_options.directory).filePath(name);
}

void RotatingFileLog::rotate()
{
    m_file.close();
    // Delete everything at or beyond the retention limit, then shift the rest up by one.
    for (int i = m_options.maxFiles - 1; i < m_options.maxFiles + 20; ++i)
        QFile::remove(pathFor(i));
    for (int i = m_options.maxFiles - 2; i >= 0; --i) {
        if (QFile::exists(pathFor(i)))
            QFile::rename(pathFor(i), pathFor(i + 1));
    }
    m_file.setFileName(pathFor(0));
    m_file.open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text);
}

void RotatingFileLog::write(const QDateTime& time, Level level, const QString& subsystem, const QString& message)
{
    const QByteArray line = formatLine(time, level, subsystem,
                                       reinterpret_cast<quintptr>(QThread::currentThreadId()), message).toUtf8();
    QMutexLocker lock(&m_mutex);
    if (!m_file.isOpen())
        return;
    if (m_file.size() > 0 && m_file.size() + line.size() > m_options.maxFileBytes)
        rotate();
    if (!m_file.isOpen())
        return;
    m_file.write(line);
    // Warnings and errors are flushed immediately so they survive a crash.
    if (level >= Level::Warning)
        m_file.flush();
}

void RotatingFileLog::flush()
{
    QMutexLocker lock(&m_mutex);
    if (m_file.isOpen())
        m_file.flush();
}

void RotatingFileLog::close()
{
    QMutexLocker lock(&m_mutex);
    m_file.close();
}

QString RotatingFileLog::activeFilePath() const
{
    QMutexLocker lock(&m_mutex);
    return m_file.fileName();
}

// ---- process-wide handler ---------------------------------------------------

namespace {

RotatingFileLog g_log;
std::atomic<int> g_minLevel{ static_cast<int>(Level::Info) };
std::atomic<bool> g_initialized{ false };
QtMessageHandler g_previousHandler = nullptr;

Level toLevel(QtMsgType type)
{
    switch (type) {
    case QtDebugMsg:    return Level::Debug;
    case QtInfoMsg:     return Level::Info;
    case QtWarningMsg:  return Level::Warning;
    case QtCriticalMsg: return Level::Error;
    case QtFatalMsg:    return Level::Error;
    }
    return Level::Info;
}

QString subsystemOf(const char* category)
{
    const QLatin1StringView name(category ? category : "default");
    if (name.startsWith(QLatin1StringView("gpufusion.")))
        return QString(name.sliced(10));
    if (name == QLatin1StringView("default"))
        return QStringLiteral("qt");
    return QString(name);
}

void messageHandler(QtMsgType type, const QMessageLogContext& context, const QString& message)
{
    const Level level = toLevel(type);
    if (static_cast<int>(level) >= g_minLevel.load(std::memory_order_relaxed) || type == QtFatalMsg)
        g_log.write(QDateTime::currentDateTime(), level, subsystemOf(context.category), message);

#ifndef NDEBUG
    const QString line = QStringLiteral("[%1] %2\n").arg(subsystemOf(context.category), message);
    OutputDebugStringW(reinterpret_cast<const wchar_t*>(line.utf16()));
#endif

    if (type == QtFatalMsg) {
        g_log.flush();
        std::abort();
    }
}

} // namespace

bool initialize(const RotatingFileLog::Options& options, Level minimumLevel, QString* error)
{
    if (!g_log.open(options, error))
        return false;
    g_minLevel = static_cast<int>(minimumLevel);
    if (!g_initialized.exchange(true))
        g_previousHandler = qInstallMessageHandler(messageHandler);
    return true;
}

void setMinimumLevel(Level level) { g_minLevel = static_cast<int>(level); }
void setLimits(qint64 maxFileBytes, int maxFiles) { g_log.setLimits(maxFileBytes, maxFiles); }
QString activeLogFile() { return g_log.activeFilePath(); }

void shutdown()
{
    if (g_initialized.exchange(false))
        qInstallMessageHandler(g_previousHandler);
    g_log.flush();
    g_log.close();
}

} // namespace gf::log
