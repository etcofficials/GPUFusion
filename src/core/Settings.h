#pragma once

#include <QByteArray>
#include <QJsonObject>
#include <QString>
#include <QStringList>

namespace gf {

// User settings. Every field has a safe default and a validated range; anything invalid in
// a loaded file is replaced by its default and reported, never silently trusted.
struct Settings {
    // 1: initial. 2: monitoring.useNvml.
    static constexpr int kSchemaVersion = 2;

    // Monitoring
    int refreshIntervalMs = 1000;      // 250 .. 10000
    int historySeconds = 300;          // 60 .. 3600
    bool useNvml = true;               // NVIDIA sensors through NVML when available

    // Startup
    bool startMinimized = false;
    bool startMonitoringOnLaunch = true;

    // Appearance
    QString theme = QStringLiteral("dark");   // "dark" | "light"

    // Logging
    QString logLevel = QStringLiteral("info"); // "debug" | "info" | "warning" | "error"
    int logMaxFileKB = 1024;           // 64 .. 16384
    int logMaxFiles = 5;               // 1 .. 20

    // Benchmarks (consumed from Phase 7)
    int benchmarkWarmupRuns = 1;       // 0 .. 10
    int benchmarkMeasuredRuns = 5;     // 1 .. 50
    int benchmarkRetentionDays = 90;   // 0 (keep forever) .. 3650

    // Window state (not user-editable)
    QByteArray windowGeometry;

    bool operator==(const Settings&) const = default;
};

struct SettingRange { int min; int max; };
namespace ranges {
inline constexpr SettingRange refreshIntervalMs{ 250, 10000 };
inline constexpr SettingRange historySeconds{ 60, 3600 };
inline constexpr SettingRange logMaxFileKB{ 64, 16384 };
inline constexpr SettingRange logMaxFiles{ 1, 20 };
inline constexpr SettingRange benchmarkWarmupRuns{ 0, 10 };
inline constexpr SettingRange benchmarkMeasuredRuns{ 1, 50 };
inline constexpr SettingRange benchmarkRetentionDays{ 0, 3650 };
}

QJsonObject toJson(const Settings& settings);
// Reads known fields; out-of-range, wrongly typed or unknown-enum values fall back to the
// default and add a human-readable entry to *warnings.
Settings fromJson(const QJsonObject& json, QStringList* warnings);
// Returns a list of problems (empty when valid).
QStringList validate(const Settings& settings);

// Loads and saves settings.json with atomic writes.
class SettingsStore {
public:
    enum class LoadStatus {
        Loaded,          // file read, all fields valid
        LoadedWithFixes, // file read, some fields replaced by defaults (see warnings)
        Missing,         // no file yet; defaults in use
        Corrupt,         // not valid JSON / not an object; file moved aside, defaults in use
        NewerVersion,    // written by a newer GPU Fusion; read leniently, saving disabled
        Unreadable       // I/O error; defaults in use
    };

    struct LoadResult {
        Settings settings;
        LoadStatus status = LoadStatus::Missing;
        QStringList warnings;
        QString backupPath; // where a corrupt file was moved
    };

    explicit SettingsStore(QString filePath);

    LoadResult load();
    bool save(const Settings& settings, QString* error);

    const QString& filePath() const { return m_filePath; }
    bool isReadOnly() const { return m_readOnly; }

private:
    QString m_filePath;
    bool m_readOnly = false;
};

QString describe(SettingsStore::LoadStatus status);

} // namespace gf
