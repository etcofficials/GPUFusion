#pragma once

// Windows per-application graphics preference (Settings > System > Display > Graphics settings):
// value model, safe editing of the stored string, a change journal with rollback, and the logic
// that verifies what a preference really does by launching a probe program under it.
// Everything here works against interfaces so it is unit-tested with in-memory fakes.

#include <QDateTime>
#include <QJsonObject>
#include <QList>
#include <QString>
#include <QStringList>

#include <functional>
#include <optional>

namespace gf::pref {

enum class GpuPreference { NotSet, LetWindowsDecide, PowerSaving, HighPerformance, Unrecognised };

// Parses the stored value, e.g. "GpuPreference=2;" (other keys may precede or follow).
GpuPreference parseGpuPreference(const QString& data);
QString gpuPreferenceText(GpuPreference p);
int gpuPreferenceCode(GpuPreference p); // 0/1/2, or -1 for NotSet/Unrecognised

// Returns `existing` with GpuPreference set to `p` (NotSet removes the key). Other keys, such as
// SwapEffectUpgradeEnable=1, are preserved in their original order. Empty result = nothing left.
QString withGpuPreference(const QString& existing, GpuPreference p);

// An executable GPU Fusion may configure: absolute, local path ending in .exe.
bool isConfigurableExecutable(const QString& path, QString* why);

// ---- registry abstraction -------------------------------------------------------------------

// HKCU\Software\Microsoft\DirectX\UserGpuPreferences: value name = executable path, REG_SZ data.
class IPreferenceStore {
public:
    virtual ~IPreferenceStore() = default;
    virtual QStringList names() const = 0;
    virtual std::optional<QString> read(const QString& name) const = 0;
    virtual bool write(const QString& name, const QString& data, QString* error) = 0;
    virtual bool remove(const QString& name, QString* error) = 0;
};

// Finds the stored value name for an executable (value names are case-insensitive in the registry).
std::optional<QString> findValueName(const IPreferenceStore& store, const QString& exePath);

// ---- change journal -------------------------------------------------------------------------

struct ChangeRecord {
    QString id;                    // unique, time-based
    QDateTime time;
    QString valueName;             // registry value name (executable path)
    std::optional<QString> before; // data before the change (nullopt = value did not exist)
    std::optional<QString> after;  // data after the change (nullopt = value removed)
    QString origin;                // "user" or "verification"
    bool rolledBack = false;

    QJsonObject toJson() const;
    static std::optional<ChangeRecord> fromJson(const QJsonObject& o);
};

// A pending probe verification: the probe's original value, so it can be restored after a crash.
struct PendingVerification {
    QString valueName;
    std::optional<QString> original;
};

// JSON file holding every change GPU Fusion made (bounded) and any pending verification.
class ChangeJournal {
public:
    explicit ChangeJournal(QString filePath, int maxRecords = 500);
    bool load(QString* error);
    bool save(QString* error) const;

    const QList<ChangeRecord>& records() const { return m_records; }
    void append(const ChangeRecord& r);
    ChangeRecord* find(const QString& id);

    std::optional<PendingVerification> pending() const { return m_pending; }
    void setPending(std::optional<PendingVerification> p) { m_pending = std::move(p); }

private:
    QString m_filePath;
    int m_maxRecords;
    QList<ChangeRecord> m_records;
    std::optional<PendingVerification> m_pending;
};

// ---- manager -----------------------------------------------------------------------------------

struct ChangeResult {
    bool ok = false;
    QString error;
    std::optional<ChangeRecord> record;
};

// Applies and rolls back preferences. Every write is journalled (with the previous value) *before*
// the registry is touched, so a change can always be undone.
class PreferenceManager {
public:
    PreferenceManager(IPreferenceStore& store, ChangeJournal& journal);

    GpuPreference current(const QString& exePath) const;
    std::optional<QString> rawValue(const QString& exePath) const;

    ChangeResult apply(const QString& exePath, GpuPreference preference, const QString& origin = QStringLiteral("user"));
    // Restores the value a record replaced. Refuses (unless force) if the value was changed since
    // by someone else (e.g. Windows Settings), so a newer choice is never silently overwritten.
    ChangeResult rollback(const QString& recordId, bool force = false);

private:
    // Parameters by value on purpose: callers pass fields of journal records, and appending to the
    // journal may reallocate the list those references point into.
    ChangeResult writeAndJournal(QString valueName, std::optional<QString> before, std::optional<QString> after, QString origin);
    IPreferenceStore& m_store;
    ChangeJournal& m_journal;
};

// ---- verification ------------------------------------------------------------------------------

struct ProbeAdapter { QString name; quint32 vendorId = 0; QString luid; };
struct ProbeReport {
    QString exe;
    QList<ProbeAdapter> enumOrder;
    std::optional<ProbeAdapter> d3d11Default;
    std::optional<ProbeAdapter> minimumPower, highPerformance, unspecified;
    QString error;
};
std::optional<ProbeReport> parseProbeReport(const QByteArray& json, QString* error);

struct VerificationStep {
    GpuPreference preference = GpuPreference::NotSet;
    bool ok = false;
    QString error;
    ProbeReport report;
};

struct VerificationResult {
    QDateTime time;
    QString probePath;
    QList<VerificationStep> steps;
    bool restored = false;   // the probe's original value is back in place (verified by reading it)
    QString restoreError;

    // True if every successful step put the probe's default device on the same adapter.
    bool preferenceHasNoEffect() const;
    QJsonObject toJson() const;
    static std::optional<VerificationResult> fromJson(const QJsonObject& o);
};

// Runs the probe and returns its stdout (or an error). Injected so tests need no real process.
using ProbeRunner = std::function<std::optional<QByteArray>(const QString& probePath, QString* error)>;

// Sets each preference for the probe executable only, runs it, records what it got, and restores
// the original value. A pending marker is journalled first so recoverPending() can restore the
// value if GPU Fusion stops in the middle.
VerificationResult verifyPreferences(IPreferenceStore& store, ChangeJournal& journal, const QString& probePath,
                                     const ProbeRunner& run);
// Restores a probe value left behind by an interrupted verification. Returns true if anything was restored.
bool recoverPending(IPreferenceStore& store, ChangeJournal& journal, QString* error);

} // namespace gf::pref
