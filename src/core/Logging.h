#pragma once

#include <QDateTime>
#include <QFile>
#include <QLoggingCategory>
#include <QMutex>
#include <QString>

// Subsystem categories. The part after "gpufusion." becomes the [subsystem] column in the log.
Q_DECLARE_LOGGING_CATEGORY(lcApp)
Q_DECLARE_LOGGING_CATEGORY(lcSettings)
Q_DECLARE_LOGGING_CATEGORY(lcUi)
Q_DECLARE_LOGGING_CATEGORY(lcHardware)
Q_DECLARE_LOGGING_CATEGORY(lcMonitor)
Q_DECLARE_LOGGING_CATEGORY(lcPrefs)
Q_DECLARE_LOGGING_CATEGORY(lcBench)

namespace gf::log {

enum class Level { Debug = 0, Info = 1, Warning = 2, Error = 3 };

QString levelName(Level level);                       // "debug", "info", ...
bool parseLevel(const QString& text, Level* out);     // case-insensitive

// Size-bounded log file: when the active file would exceed maxFileBytes it is renamed to
// <base>.1.log, older files shift up, and anything beyond maxFiles in total is deleted.
class RotatingFileLog {
public:
    struct Options {
        QString directory;
        QString baseName = QStringLiteral("gpufusion");
        qint64 maxFileBytes = 1024 * 1024;
        int maxFiles = 5; // active file + rotated files
    };

    bool open(const Options& options, QString* error);
    void setLimits(qint64 maxFileBytes, int maxFiles);
    void write(const QDateTime& time, Level level, const QString& subsystem, const QString& message);
    void flush();
    void close();
    QString activeFilePath() const;

    static QString formatLine(const QDateTime& time, Level level, const QString& subsystem,
                              quintptr threadId, const QString& message);

private:
    QString pathFor(int index) const;
    void rotate();

    mutable QMutex m_mutex;
    Options m_options;
    QFile m_file;
};

// Process-wide logging: installs a Qt message handler that routes qDebug/qInfo/qWarning/
// qCritical (with categories) to a RotatingFileLog and, in debug builds, the debugger.
bool initialize(const RotatingFileLog::Options& options, Level minimumLevel, QString* error);
void setMinimumLevel(Level level);
void setLimits(qint64 maxFileBytes, int maxFiles);
QString activeLogFile();
void shutdown();

} // namespace gf::log
