#include "profiles/KnownOptions.h"

#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>

namespace gf::profiles {

QList<KnownGpuOption> builtInKnownOptions()
{
    const QString chromiumCaveat = QStringLiteral("A browser that is already running ignores the option (the new window joins the running "
                                                  "instance): close every window first, or give the profile its own --user-data-dir.");
    return {
        { { QStringLiteral("msedge.exe"), QStringLiteral("brave.exe") }, QStringLiteral("Microsoft Edge, Brave (Chromium)"),
          QStringLiteral("--use-adapter-luid={luid_pair}"),
          QStringLiteral("Verified on the reference PC (2026-10-26): with the Intel GPU's LUID, Edge and Brave rendered WebGL with ANGLE on "
                         "Intel HD Graphics (renderer string and GPU Fusion's verification); without it, on the GT 730."),
          chromiumCaveat, false },
        { { QStringLiteral("chrome.exe"), QStringLiteral("vivaldi.exe"), QStringLiteral("opera.exe"), QStringLiteral("chromium.exe") },
          QStringLiteral("Other Chromium-based browsers"), QStringLiteral("--use-adapter-luid={luid_pair}"),
          QStringLiteral("Same Chromium switch as Edge and Brave (verified there); not verified with these browsers on the reference PC."),
          chromiumCaveat, false },
        { { QStringLiteral("ffmpeg.exe") }, QStringLiteral("FFmpeg"),
          QStringLiteral("-init_hw_device d3d11va=gf:{dxgi_index} -hwaccel d3d11va -hwaccel_device gf"),
          QStringLiteral("Verified on the reference PC (2026-10-26): Direct3D 11 hardware decoding ran on the chosen adapter (GPU Fusion's "
                         "verification), with identical frame hashes on both GPUs."),
          QStringLiteral("Hardware decoding only; add your own input and output arguments after it."), false },
    };
}

QList<KnownGpuOption> loadKnownOptions(const QString& userFile, QStringList* problems)
{
    QList<KnownGpuOption> out;
    QFile f(userFile);
    if (!userFile.isEmpty() && f.exists()) {
        if (!f.open(QIODevice::ReadOnly)) {
            if (problems) *problems << QStringLiteral("%1: %2").arg(userFile, f.errorString());
        } else {
            QJsonParseError err{};
            const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
            if (!doc.isArray()) {
                if (problems) *problems << QStringLiteral("%1: not a JSON array (%2)").arg(userFile, err.errorString());
            } else {
                int i = 0;
                for (const QJsonValue& v : doc.array()) {
                    ++i;
                    const QJsonObject o = v.toObject();
                    KnownGpuOption k;
                    for (const QJsonValue& e : o.value(QLatin1StringView("executables")).toArray())
                        if (!e.toString().trimmed().isEmpty())
                            k.executables << e.toString().trimmed();
                    k.arguments = o.value(QLatin1StringView("arguments")).toString().trimmed();
                    k.program = o.value(QLatin1StringView("program")).toString(k.executables.value(0));
                    k.evidence = o.value(QLatin1StringView("note")).toString(QStringLiteral("Added by you; not checked by GPU Fusion."));
                    k.userDefined = true;
                    if (k.executables.isEmpty() || k.arguments.isEmpty()) {
                        if (problems) *problems << QStringLiteral("%1: entry %2 needs \"executables\" and \"arguments\"").arg(userFile).arg(i);
                        continue;
                    }
                    out << k;
                }
            }
        }
    }
    out << builtInKnownOptions();
    return out;
}

std::optional<KnownGpuOption> knownOptionFor(const QString& exePath, const QList<KnownGpuOption>& options)
{
    const QString name = QFileInfo(exePath).fileName();
    for (const KnownGpuOption& o : options)
        for (const QString& e : o.executables)
            if (e.compare(name, Qt::CaseInsensitive) == 0)
                return o;
    return std::nullopt;
}

QString withKnownOption(const QString& arguments, const KnownGpuOption& option)
{
    const QString current = arguments.trimmed();
    if (current.contains(option.arguments))
        return current;
    return current.isEmpty() ? option.arguments : option.arguments + u' ' + current;
}

} // namespace gf::profiles
