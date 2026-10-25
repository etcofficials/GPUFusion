#include "platform/windows/ProbeRunner.h"

#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>

#include <windows.h>

namespace gf::pref::win {

namespace {

// GPU Fusion points its own TEMP at its data folder (to keep driver caches off C:). Programs it
// starts must get the user's normal environment back.
QProcessEnvironment childEnvironment()
{
    QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
    const QString original = env.value(QStringLiteral("GPUFUSION_ORIGINAL_TEMP"));
    if (!original.isEmpty()) {
        env.insert(QStringLiteral("TEMP"), original);
        env.insert(QStringLiteral("TMP"), original);
    }
    env.remove(QStringLiteral("GPUFUSION_ORIGINAL_TEMP"));
    return env;
}

} // namespace

std::optional<QByteArray> runProbe(const QString& probePath, QString* error, int timeoutMs)
{
    if (!QFileInfo::exists(probePath)) {
        if (error) *error = QStringLiteral("%1 is missing.").arg(probePath);
        return std::nullopt;
    }
    QProcess p;
    // The probe is GPU Fusion's own helper: it keeps GPU Fusion's TEMP (in the data folder), so the
    // shader cache the graphics driver writes when it creates a device stays off the system drive.
    p.setCreateProcessArgumentsModifier([](QProcess::CreateProcessArguments* args) { args->flags |= CREATE_NO_WINDOW; });
    p.start(probePath, {});
    if (!p.waitForStarted(5000)) {
        if (error) *error = p.errorString();
        return std::nullopt;
    }
    if (!p.waitForFinished(timeoutMs)) {
        p.kill();
        p.waitForFinished(2000);
        if (error) *error = QStringLiteral("The probe did not finish within %1 s.").arg(timeoutMs / 1000);
        return std::nullopt;
    }
    if (p.exitStatus() != QProcess::NormalExit) {
        if (error) *error = QStringLiteral("The probe crashed.");
        return std::nullopt;
    }
    return p.readAllStandardOutput();
}

bool launchDetached(const QString& exePath, QString* error)
{
    return launchDetachedPid(exePath, {}, error).has_value();
}

std::optional<quint32> launchDetachedPid(const QString& exePath, const QStringList& arguments, QString* error,
                                         const QHash<QString, QString>& extraEnvironment)
{
    QProcess p;
    p.setProgram(exePath);
    p.setArguments(arguments);
    p.setWorkingDirectory(QFileInfo(exePath).absolutePath());
    QProcessEnvironment env = childEnvironment();
    for (auto it = extraEnvironment.begin(); it != extraEnvironment.end(); ++it)
        env.insert(it.key(), it.value());
    p.setProcessEnvironment(env);
    qint64 pid = 0;
    if (!p.startDetached(&pid)) {
        if (error) *error = p.errorString();
        return std::nullopt;
    }
    return quint32(pid);
}

} // namespace gf::pref::win
