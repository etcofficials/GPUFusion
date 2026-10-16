#include "ui/DiagnosticsCollector.h"

#include "core/Logging.h"
#include "core/Version.h"
#include "hardware/HardwareService.h"
#include "monitoring/MonitorService.h"
#include "platform/windows/OpenClInfo.h"
#include "preferences/PreferenceService.h"
#include "ui/AppContext.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>
#include <QSysInfo>

namespace gf::ui {

namespace {

QString buildDescription()
{
#ifdef NDEBUG
    const QString type = QStringLiteral("Release");
#else
    const QString type = QStringLiteral("Debug");
#endif
#if defined(_MSC_FULL_VER)
    const int full = _MSC_FULL_VER;
    return QStringLiteral("%1, MSVC %2.%3.%4, x64").arg(type).arg(full / 10000000).arg((full / 100000) % 100).arg(full % 100000);
#else
    return type;
#endif
}

// The tail of the current log file (bounded read), parsed for warnings and errors.
QList<diag::LogLine> recentLogProblems(int maxLines)
{
    QFile f(log::activeLogFile());
    if (!f.open(QIODevice::ReadOnly))
        return {};
    constexpr qint64 kTail = 256 * 1024;
    if (f.size() > kTail)
        f.seek(f.size() - kTail);
    return diag::parseLogProblems(QString::fromUtf8(f.readAll()), maxLines);
}

} // namespace

diag::ReportInputs collectReportInputs(AppContext& context)
{
    diag::ReportInputs in;
    in.generated = QDateTime::currentDateTime();
    in.appVersion = QString::fromLatin1(version::kString);
    in.qtVersion = QString::fromLatin1(qVersion());
    in.osDescription = QStringLiteral("%1 (kernel %2)").arg(QSysInfo::prettyProductName(), QSysInfo::kernelVersion());
    in.buildDescription = buildDescription();
    in.detection = context.hardware().latest();
    in.snapshot = context.monitor().latest();
    in.providers = context.monitor().providers();
    in.monitoringPaused = context.monitor().isPaused();
    in.processes = context.monitor().latestProcesses();
    in.preferenceVerification = context.preferences().lastVerification();
    in.configuredPreferences = int(context.preferences().configuredApplications().size());
    in.dataRoot = context.data().root;
    in.dataFallback = context.data().source == DataLocation::Source::Fallback;
    in.dataProblem = context.data().problem;
    in.settingsWarnings = context.loadResult().warnings;
    in.openCl = diag::win::readOpenClInfo();
    in.recentProblems = recentLogProblems(20);
    return in;
}

QString exportReport(const diag::Report& report, const QString& basePath, QStringList* written)
{
    const diag::Redactor redactor = diag::currentRedactor();
    QDir().mkpath(QFileInfo(basePath).absolutePath());
    const QList<std::pair<QString, QByteArray>> files{
        { basePath + QStringLiteral(".md"), redactor.apply(report.toMarkdown()).toUtf8() },
        { basePath + QStringLiteral(".json"), redactor.apply(QString::fromUtf8(QJsonDocument(report.toJson()).toJson(QJsonDocument::Indented))).toUtf8() },
    };
    for (const auto& [path, bytes] : files) {
        QSaveFile f(path);
        if (!f.open(QIODevice::WriteOnly))
            return QStringLiteral("Cannot write %1: %2").arg(QDir::toNativeSeparators(path), f.errorString());
        f.write(bytes);
        if (!f.commit())
            return QStringLiteral("Cannot write %1: %2").arg(QDir::toNativeSeparators(path), f.errorString());
        if (written)
            *written << QDir::toNativeSeparators(path);
    }
    qCInfo(lcApp).noquote() << "Diagnostic report written:" << QDir::toNativeSeparators(basePath) + QStringLiteral(".md/.json");
    return {};
}

} // namespace gf::ui
