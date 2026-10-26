#include "profiles/ProfileStore.h"

#include "core/Logging.h"

#include <QCollator>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QRegularExpression>
#include <QSaveFile>
#include <QVersionNumber>

#include <algorithm>

namespace gf::profiles {

ProfileStore::ProfileStore(QString directory) : m_dir(std::move(directory)) {}

QString ProfileStore::pathFor(const QString& id) const
{
    return QDir(m_dir).filePath(id + QStringLiteral(".json"));
}

QList<AppProfile> ProfileStore::list() const
{
    QList<AppProfile> out;
    for (const QFileInfo& fi : QDir(m_dir).entryInfoList({ QStringLiteral("*.json") }, QDir::Files)) {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) {
            qCWarning(lcPrefs).noquote() << "GPU profile not readable:" << QDir::toNativeSeparators(fi.absoluteFilePath()) << f.errorString();
            continue;
        }
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (auto p = AppProfile::fromJson(doc.object()))
            out << *p;
        else // damaged (e.g. by a failing disk) or from a newer GPU Fusion: left in place, not overwritten
            qCWarning(lcPrefs).noquote() << "GPU profile ignored (invalid or newer format):" << QDir::toNativeSeparators(fi.absoluteFilePath())
                                         << (err.error != QJsonParseError::NoError ? err.errorString() : QString());
    }
    QCollator collator;
    collator.setCaseSensitivity(Qt::CaseInsensitive);
    std::sort(out.begin(), out.end(), [&](const AppProfile& a, const AppProfile& b) { return collator.compare(a.name, b.name) < 0; });
    return out;
}

std::optional<AppProfile> ProfileStore::find(const QString& idOrExe) const
{
    const QString id = idOrExe.contains(u'\\') || idOrExe.contains(u'/') ? profileIdFor(idOrExe) : idOrExe;
    QFile f(pathFor(id));
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    return AppProfile::fromJson(QJsonDocument::fromJson(f.readAll()).object());
}

bool ProfileStore::save(AppProfile& p, QString* error)
{
    if (p.exePath.isEmpty()) {
        if (error) *error = QStringLiteral("The profile has no executable.");
        return false;
    }
    p.exePath = QDir::toNativeSeparators(QFileInfo(p.exePath).absoluteFilePath());
    p.id = profileIdFor(p.exePath);
    const QDateTime now = QDateTime::currentDateTime();
    if (!p.created.isValid())
        p.created = now;
    p.modified = now;
    if (!QDir().mkpath(m_dir)) {
        if (error) *error = QStringLiteral("Cannot create %1").arg(QDir::toNativeSeparators(m_dir));
        return false;
    }
    QSaveFile f(pathFor(p.id));
    if (!f.open(QIODevice::WriteOnly)) {
        if (error) *error = f.errorString();
        return false;
    }
    f.write(QJsonDocument(p.toJson()).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

bool ProfileStore::remove(const QString& id, QString* error)
{
    QFile f(pathFor(id));
    if (!f.exists())
        return true;
    if (!f.remove()) {
        if (error) *error = f.errorString();
        return false;
    }
    return true;
}

QString resolveCurrentExecutable(const QString& exePath)
{
    const QFileInfo exe(exePath);
    const QDir versionDir = exe.absoluteDir();
    static const QRegularExpression versioned(QStringLiteral(R"(^(?:app-)?(\d+(?:\.\d+){1,3})$)"), QRegularExpression::CaseInsensitiveOption);
    const auto mine = versioned.match(versionDir.dirName());
    if (!mine.hasMatch())
        return exePath;
    QDir parent = versionDir;
    if (!parent.cdUp())
        return exePath;
    const QString prefix = versionDir.dirName().startsWith(QLatin1StringView("app-"), Qt::CaseInsensitive) ? QStringLiteral("app-") : QString();
    QVersionNumber best = QVersionNumber::fromString(mine.captured(1));
    QString bestPath = exePath;
    for (const QFileInfo& sibling : parent.entryInfoList(QDir::Dirs | QDir::NoDotAndDotDot)) {
        const auto m = versioned.match(sibling.fileName());
        if (!m.hasMatch() || !sibling.fileName().startsWith(prefix, Qt::CaseInsensitive))
            continue;
        const QVersionNumber v = QVersionNumber::fromString(m.captured(1));
        const QString candidate = QDir(sibling.absoluteFilePath()).filePath(exe.fileName());
        if (v > best && QFileInfo::exists(candidate)) {
            best = v;
            bestPath = QDir::toNativeSeparators(candidate);
        }
    }
    return bestPath;
}

QByteArray exportProfiles(const QList<AppProfile>& profiles)
{
    QJsonArray list;
    for (const AppProfile& p : profiles)
        list.append(p.toJson());
    const QJsonObject doc{ { "gpuFusionProfiles", 1 }, { "exported", QDateTime::currentDateTime().toString(Qt::ISODate) },
                           { "profiles", list } };
    return QJsonDocument(doc).toJson(QJsonDocument::Indented);
}

QString ImportResult::summary() const
{
    if (!ok)
        return error;
    QStringList parts{ QStringLiteral("%1 profile(s) imported").arg(imported) };
    if (replaced)
        parts << QStringLiteral("%1 replaced").arg(replaced);
    if (skipped)
        parts << QStringLiteral("%1 skipped (already here)").arg(skipped);
    QString s = parts.join(QStringLiteral(", ")) + u'.';
    if (!missingPrograms.isEmpty())
        s += QStringLiteral(" Not installed on this PC: %1.").arg(missingPrograms.join(QStringLiteral(", ")));
    if (imported + replaced > 0)
        s += QStringLiteral(" No Windows setting was changed: apply a profile to put its mode into effect here, and verify it on this PC.");
    return s;
}

ImportResult importProfiles(ProfileStore& store, const QByteArray& document, bool replace)
{
    ImportResult r;
    QJsonParseError err{};
    const QJsonDocument doc = QJsonDocument::fromJson(document, &err);
    const QJsonObject root = doc.object();
    if (err.error != QJsonParseError::NoError || !root.contains(QLatin1StringView("gpuFusionProfiles"))) {
        r.error = QStringLiteral("This is not a GPU Fusion profile export%1.")
                      .arg(err.error != QJsonParseError::NoError ? QStringLiteral(" (%1)").arg(err.errorString()) : QString());
        return r;
    }
    if (root.value(QLatin1StringView("gpuFusionProfiles")).toInt() > 1) {
        r.error = QStringLiteral("The export was made by a newer GPU Fusion; update GPU Fusion to import it.");
        return r;
    }
    r.ok = true;
    for (const QJsonValue& v : root.value(QLatin1StringView("profiles")).toArray()) {
        std::optional<AppProfile> p = AppProfile::fromJson(v.toObject());
        if (!p) {
            ++r.skipped;
            continue;
        }
        // Results and applied values describe the PC the export was made on.
        p->lastVerification.reset();
        p->appliedValue.clear();
        p->created = QDateTime();
        const bool exists = store.find(p->id).has_value();
        if (exists && !replace) {
            ++r.skipped;
            continue;
        }
        QString error;
        if (!store.save(*p, &error)) {
            r.ok = false;
            r.error = error;
            return r;
        }
        ++(exists ? r.replaced : r.imported);
        if (!QFileInfo::exists(resolveCurrentExecutable(p->exePath)))
            r.missingPrograms << p->name;
    }
    return r;
}

AppProfile newProfile(const QString& exePath, const QString& name)
{
    AppProfile p;
    p.exePath = QDir::toNativeSeparators(QFileInfo(exePath).absoluteFilePath());
    p.id = profileIdFor(p.exePath);
    p.name = name.isEmpty() ? QFileInfo(exePath).completeBaseName() : name;
    return p;
}

} // namespace gf::profiles
