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
#include <QStorageInfo>
#include <QVersionNumber>

#include <algorithm>

namespace gf::profiles {

ProfileStore::ProfileStore(QString directory) : m_dir(std::move(directory)) {}

QString ProfileStore::pathFor(const QString& id) const
{
    return QDir(m_dir).filePath(id + QStringLiteral(".json"));
}

QList<AppProfile> ProfileStore::list(QStringList* unreadable) const
{
    QList<AppProfile> out;
    for (const QFileInfo& fi : QDir(m_dir).entryInfoList({ QStringLiteral("*.json") }, QDir::Files)) {
        QFile f(fi.absoluteFilePath());
        if (!f.open(QIODevice::ReadOnly)) {
            qCWarning(lcPrefs).noquote() << "GPU profile not readable:" << QDir::toNativeSeparators(fi.absoluteFilePath()) << f.errorString();
            if (unreadable)
                *unreadable << QDir::toNativeSeparators(fi.absoluteFilePath());
            continue;
        }
        QJsonParseError err{};
        const QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &err);
        if (auto p = AppProfile::fromJson(doc.object()))
            out << *p;
        else { // damaged (e.g. by a failing disk) or from a newer GPU Fusion: left in place, not overwritten
            qCWarning(lcPrefs).noquote() << "GPU profile ignored (invalid or newer format):" << QDir::toNativeSeparators(fi.absoluteFilePath())
                                         << (err.error != QJsonParseError::NoError ? err.errorString() : QString());
            if (unreadable)
                *unreadable << QDir::toNativeSeparators(fi.absoluteFilePath());
        }
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
    p.variant = p.variant.trimmed();
    p.id = profileIdFor(p.exePath, p.variant);
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
    changed();
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
    changed();
    return true;
}

void ProfileStore::changed()
{
    if (m_batch > 0) {
        m_changedInBatch = true;
        return;
    }
    if (m_onChange)
        m_onChange();
}

void ProfileStore::endBatch()
{
    if (m_batch > 0 && --m_batch == 0 && std::exchange(m_changedInBatch, false) && m_onChange)
        m_onChange();
}

// ---- automatic backup -------------------------------------------------------------------------------

namespace {

const QString kBackupPrefix = QStringLiteral("gpufusion-profiles-");

// What a backup holds, without the export time (so identical profiles compare equal).
QJsonArray backedUpProfiles(const QByteArray& document)
{
    return QJsonDocument::fromJson(document).object().value(QLatin1StringView("profiles")).toArray();
}

} // namespace

BackupResult backupProfiles(const QList<AppProfile>& profiles, const QString& folder, int keep)
{
    BackupResult r;
    QDir dir(folder);
    if (folder.trimmed().isEmpty() || !QDir().mkpath(folder)) {
        r.error = QStringLiteral("Cannot use the backup folder %1").arg(QDir::toNativeSeparators(folder));
        return r;
    }
    const QByteArray document = exportProfiles(profiles);
    // Newest first (the time stamp in the name sorts chronologically).
    QStringList existing = dir.entryList({ kBackupPrefix + QStringLiteral("*.json") }, QDir::Files, QDir::Name | QDir::Reversed);
    if (!existing.isEmpty()) {
        QFile newest(dir.filePath(existing.first()));
        if (newest.open(QIODevice::ReadOnly) && backedUpProfiles(newest.readAll()) == backedUpProfiles(document)) {
            r.unchanged = true;
            r.path = QDir::toNativeSeparators(newest.fileName());
            return r;
        }
    }
    const QString name = kBackupPrefix + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz")) + QStringLiteral(".json");
    QSaveFile out(dir.filePath(name));
    if (!out.open(QIODevice::WriteOnly) || out.write(document) < 0 || !out.commit()) {
        r.error = out.errorString();
        return r;
    }
    r.written = true;
    r.path = QDir::toNativeSeparators(dir.filePath(name));
    existing.prepend(name);
    for (int i = std::max(1, keep); i < existing.size(); ++i)
        if (QFile::remove(dir.filePath(existing[i])))
            ++r.removed;
    return r;
}

BackupResult backupStore(const ProfileStore& store, const QString& folder, int keep)
{
    QStringList unreadable;
    const QList<AppProfile> profiles = store.list(&unreadable);
    if (!unreadable.isEmpty()) {
        BackupResult r;
        r.error = QStringLiteral("not backed up: %1 profile file(s) could not be read (%2). The existing backups are kept, so they "
                                 "still contain those profiles; restore them with Import….")
                      .arg(unreadable.size()).arg(unreadable.join(QStringLiteral(", ")));
        return r;
    }
    return backupProfiles(profiles, folder, keep);
}

bool onSameVolume(const QString& a, const QString& b)
{
    const QStorageInfo va(QFileInfo(a).absolutePath()), vb(QFileInfo(b).absolutePath());
    return va.isValid() && vb.isValid() && va.rootPath().compare(vb.rootPath(), Qt::CaseInsensitive) == 0;
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
    // One change notification (e.g. one automatic backup) for the whole import, not one per profile.
    struct Batch {
        ProfileStore& s;
        explicit Batch(ProfileStore& store) : s(store) { s.beginBatch(); }
        ~Batch() { s.endBatch(); }
    } batch(store);
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
            r.missingPrograms << displayName(*p);
    }
    return r;
}

std::optional<AppProfile> newVariant(const AppProfile& base, const QString& variantName)
{
    const QString v = variantName.trimmed();
    if (v.isEmpty())
        return std::nullopt;
    AppProfile p = base;
    p.variant = v;
    p.id = profileIdFor(p.exePath, v);
    p.appliedValue.clear();
    p.lastVerification.reset();
    p.created = p.modified = QDateTime();
    return p;
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
