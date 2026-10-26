#include "core/Settings.h"

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QJsonParseError>
#include <QSaveFile>

#include <cmath>

namespace gf {

namespace {

const QStringList kThemes{ QStringLiteral("dark"), QStringLiteral("light") };
const QStringList kLogLevels{ QStringLiteral("debug"), QStringLiteral("info"), QStringLiteral("warning"), QStringLiteral("error") };

QJsonObject section(const QJsonObject& root, const char* key, QStringList* warnings)
{
    const QJsonValue v = root.value(QLatin1StringView(key));
    if (v.isUndefined())
        return {};
    if (!v.isObject()) {
        warnings->append(QStringLiteral("\"%1\" is not an object; using defaults for that section").arg(QLatin1StringView(key)));
        return {};
    }
    return v.toObject();
}

void readInt(const QJsonObject& obj, const char* key, SettingRange range, int* field, QStringList* warnings)
{
    const QJsonValue v = obj.value(QLatin1StringView(key));
    if (v.isUndefined())
        return;
    const double d = v.toDouble();
    if (!v.isDouble() || d != std::floor(d)) {
        warnings->append(QStringLiteral("%1: expected a whole number; using default %2").arg(QLatin1StringView(key)).arg(*field));
        return;
    }
    if (d < range.min || d > range.max) {
        warnings->append(QStringLiteral("%1: %2 is outside %3..%4; using default %5")
                             .arg(QLatin1StringView(key)).arg(d, 0, 'g', 15).arg(range.min).arg(range.max).arg(*field));
        return;
    }
    *field = static_cast<int>(d);
}

void readBool(const QJsonObject& obj, const char* key, bool* field, QStringList* warnings)
{
    const QJsonValue v = obj.value(QLatin1StringView(key));
    if (v.isUndefined())
        return;
    if (!v.isBool()) {
        warnings->append(QStringLiteral("%1: expected true/false; using default").arg(QLatin1StringView(key)));
        return;
    }
    *field = v.toBool();
}

void readChoice(const QJsonObject& obj, const char* key, const QStringList& allowed, QString* field, QStringList* warnings)
{
    const QJsonValue v = obj.value(QLatin1StringView(key));
    if (v.isUndefined())
        return;
    const QString s = v.toString().toLower();
    if (!v.isString() || !allowed.contains(s)) {
        warnings->append(QStringLiteral("%1: expected one of %2; using default \"%3\"")
                             .arg(QLatin1StringView(key), allowed.join(u'/'), *field));
        return;
    }
    *field = s;
}

bool inRange(int v, SettingRange r) { return v >= r.min && v <= r.max; }

} // namespace

QJsonObject toJson(const Settings& s)
{
    return QJsonObject{
        { "schemaVersion", Settings::kSchemaVersion },
        { "monitoring", QJsonObject{ { "refreshIntervalMs", s.refreshIntervalMs }, { "historySeconds", s.historySeconds },
                                     { "useNvml", s.useNvml } } },
        { "startup", QJsonObject{ { "startMinimized", s.startMinimized }, { "startMonitoringOnLaunch", s.startMonitoringOnLaunch } } },
        { "appearance", QJsonObject{ { "theme", s.theme } } },
        { "logging", QJsonObject{ { "level", s.logLevel }, { "maxFileKB", s.logMaxFileKB }, { "maxFiles", s.logMaxFiles } } },
        { "benchmarks", QJsonObject{ { "warmupRuns", s.benchmarkWarmupRuns }, { "measuredRuns", s.benchmarkMeasuredRuns },
                                     { "retentionDays", s.benchmarkRetentionDays } } },
        { "profiles", QJsonObject{ { "backupFolder", s.profileBackupFolder }, { "backupsKept", s.profileBackupsKept } } },
        { "window", QJsonObject{ { "geometry", QString::fromLatin1(s.windowGeometry.toBase64()) } } },
    };
}

Settings fromJson(const QJsonObject& root, QStringList* warnings)
{
    Settings s;
    const QJsonObject mon = section(root, "monitoring", warnings);
    readInt(mon, "refreshIntervalMs", ranges::refreshIntervalMs, &s.refreshIntervalMs, warnings);
    readInt(mon, "historySeconds", ranges::historySeconds, &s.historySeconds, warnings);
    readBool(mon, "useNvml", &s.useNvml, warnings); // added in schema 2; absent in schema 1 -> default

    const QJsonObject start = section(root, "startup", warnings);
    readBool(start, "startMinimized", &s.startMinimized, warnings);
    readBool(start, "startMonitoringOnLaunch", &s.startMonitoringOnLaunch, warnings);

    readChoice(section(root, "appearance", warnings), "theme", kThemes, &s.theme, warnings);

    const QJsonObject logging = section(root, "logging", warnings);
    readChoice(logging, "level", kLogLevels, &s.logLevel, warnings);
    readInt(logging, "maxFileKB", ranges::logMaxFileKB, &s.logMaxFileKB, warnings);
    readInt(logging, "maxFiles", ranges::logMaxFiles, &s.logMaxFiles, warnings);

    const QJsonObject bench = section(root, "benchmarks", warnings);
    readInt(bench, "warmupRuns", ranges::benchmarkWarmupRuns, &s.benchmarkWarmupRuns, warnings);
    readInt(bench, "measuredRuns", ranges::benchmarkMeasuredRuns, &s.benchmarkMeasuredRuns, warnings);
    readInt(bench, "retentionDays", ranges::benchmarkRetentionDays, &s.benchmarkRetentionDays, warnings);

    // Added in schema 3; absent before -> defaults (backup off).
    const QJsonObject prof = section(root, "profiles", warnings);
    if (const QJsonValue folder = prof.value(QLatin1StringView("backupFolder")); !folder.isUndefined()) {
        const QString f = folder.toString().trimmed();
        if (!folder.isString() || (!f.isEmpty() && QDir::isRelativePath(f)))
            warnings->append(QStringLiteral("backupFolder: expected an absolute folder path; automatic profile backup is off"));
        else
            s.profileBackupFolder = QDir::toNativeSeparators(f);
    }
    readInt(prof, "backupsKept", ranges::profileBackupsKept, &s.profileBackupsKept, warnings);

    const QJsonValue geometry = section(root, "window", warnings).value(QLatin1StringView("geometry"));
    if (geometry.isString())
        s.windowGeometry = QByteArray::fromBase64(geometry.toString().toLatin1());
    return s;
}

QStringList validate(const Settings& s)
{
    QStringList problems;
    auto check = [&](int v, SettingRange r, const char* name) {
        if (!inRange(v, r))
            problems << QStringLiteral("%1 must be between %2 and %3").arg(QLatin1StringView(name)).arg(r.min).arg(r.max);
    };
    check(s.refreshIntervalMs, ranges::refreshIntervalMs, "Refresh interval (ms)");
    check(s.historySeconds, ranges::historySeconds, "Graph history (s)");
    check(s.logMaxFileKB, ranges::logMaxFileKB, "Log file size (KB)");
    check(s.logMaxFiles, ranges::logMaxFiles, "Log files kept");
    check(s.benchmarkWarmupRuns, ranges::benchmarkWarmupRuns, "Warm-up runs");
    check(s.benchmarkMeasuredRuns, ranges::benchmarkMeasuredRuns, "Measured runs");
    check(s.benchmarkRetentionDays, ranges::benchmarkRetentionDays, "Benchmark retention (days)");
    check(s.profileBackupsKept, ranges::profileBackupsKept, "Profile backups kept");
    if (!s.profileBackupFolder.trimmed().isEmpty() && QDir::isRelativePath(s.profileBackupFolder.trimmed()))
        problems << QStringLiteral("Profile backup folder must be a full path (e.g. D:\\Backups\\GPU Fusion)");
    if (!kThemes.contains(s.theme))
        problems << QStringLiteral("Theme must be one of %1").arg(kThemes.join(u'/'));
    if (!kLogLevels.contains(s.logLevel))
        problems << QStringLiteral("Log level must be one of %1").arg(kLogLevels.join(u'/'));
    return problems;
}

// ---- SettingsStore ----------------------------------------------------------

SettingsStore::SettingsStore(QString filePath) : m_filePath(std::move(filePath)) {}

SettingsStore::LoadResult SettingsStore::load()
{
    LoadResult result;
    m_readOnly = false;

    QFile file(m_filePath);
    if (!file.exists()) {
        result.status = LoadStatus::Missing;
        return result;
    }
    if (!file.open(QIODevice::ReadOnly)) {
        result.status = LoadStatus::Unreadable;
        result.warnings << QStringLiteral("Cannot read %1: %2").arg(QDir::toNativeSeparators(m_filePath), file.errorString());
        // Do not overwrite a file we could not read.
        m_readOnly = true;
        return result;
    }
    const QByteArray bytes = file.readAll();
    file.close();

    QJsonParseError parseError{};
    const QJsonDocument doc = QJsonDocument::fromJson(bytes, &parseError);
    if (parseError.error != QJsonParseError::NoError || !doc.isObject()) {
        result.status = LoadStatus::Corrupt;
        const QString reason = parseError.error != QJsonParseError::NoError
            ? QStringLiteral("%1 at offset %2").arg(parseError.errorString()).arg(parseError.offset)
            : QStringLiteral("top-level value is not an object");
        result.backupPath = QStringLiteral("%1.corrupt-%2").arg(m_filePath, QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss")));
        if (QFile::rename(m_filePath, result.backupPath)) {
            result.warnings << QStringLiteral("Settings file was invalid (%1). It was moved to %2 and defaults are in use.")
                                   .arg(reason, QDir::toNativeSeparators(result.backupPath));
        } else {
            result.warnings << QStringLiteral("Settings file was invalid (%1) and could not be moved aside; defaults are in use and saving is disabled.").arg(reason);
            result.backupPath.clear();
            m_readOnly = true;
        }
        return result;
    }

    const QJsonObject root = doc.object();
    const int version = root.value(QLatin1StringView("schemaVersion")).toInt(0);
    result.settings = fromJson(root, &result.warnings);

    if (version > Settings::kSchemaVersion) {
        result.status = LoadStatus::NewerVersion;
        result.warnings.prepend(QStringLiteral("Settings were written by a newer GPU Fusion (schema %1, this build understands %2). "
                                               "Known values were loaded; changes will not be saved to avoid losing newer data.")
                                    .arg(version).arg(Settings::kSchemaVersion));
        m_readOnly = true;
    } else if (version < 1) {
        result.warnings.prepend(QStringLiteral("schemaVersion missing or invalid; treated as version 1"));
        result.status = LoadStatus::LoadedWithFixes;
    } else {
        result.status = result.warnings.isEmpty() ? LoadStatus::Loaded : LoadStatus::LoadedWithFixes;
    }
    return result;
}

bool SettingsStore::save(const Settings& settings, QString* error)
{
    if (m_readOnly) {
        if (error) *error = QStringLiteral("Saving is disabled for this settings file (see earlier warning).");
        return false;
    }
    if (const QStringList problems = validate(settings); !problems.isEmpty()) {
        if (error) *error = problems.join(QStringLiteral("; "));
        return false;
    }
    QDir().mkpath(QFileInfo(m_filePath).absolutePath());
    QSaveFile file(m_filePath);
    if (!file.open(QIODevice::WriteOnly)) {
        if (error) *error = file.errorString();
        return false;
    }
    file.write(QJsonDocument(toJson(settings)).toJson(QJsonDocument::Indented));
    if (!file.commit()) {
        if (error) *error = file.errorString();
        return false;
    }
    return true;
}

QString describe(SettingsStore::LoadStatus status)
{
    switch (status) {
    case SettingsStore::LoadStatus::Loaded:          return QStringLiteral("loaded");
    case SettingsStore::LoadStatus::LoadedWithFixes: return QStringLiteral("loaded with corrections");
    case SettingsStore::LoadStatus::Missing:         return QStringLiteral("not found, defaults in use");
    case SettingsStore::LoadStatus::Corrupt:         return QStringLiteral("invalid, defaults in use");
    case SettingsStore::LoadStatus::NewerVersion:    return QStringLiteral("newer version, read-only");
    case SettingsStore::LoadStatus::Unreadable:      return QStringLiteral("unreadable, defaults in use");
    }
    return QStringLiteral("unknown");
}

} // namespace gf
