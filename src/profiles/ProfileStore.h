#pragma once

#include "profiles/AppProfile.h"

#include <QList>
#include <QString>

#include <functional>

namespace gf::profiles {

// One JSON file per program in <data>\config\profiles\<id>.json, written atomically — editing one
// program's profile never touches another's.
class ProfileStore {
public:
    explicit ProfileStore(QString directory);

    QString directory() const { return m_dir; }
    // Sorted by name. Files that cannot be read or parsed are skipped (and logged); `unreadable`, if
    // given, receives their paths.
    QList<AppProfile> list(QStringList* unreadable = nullptr) const;
    std::optional<AppProfile> find(const QString& idOrExe) const;
    bool save(AppProfile& profile, QString* error);        // fills id/created/modified
    bool remove(const QString& id, QString* error);

    // Called after a successful save or remove (e.g. to back the profiles up). Inside a batch it is
    // called once, when the outermost batch ends, if anything changed.
    void setChangeCallback(std::function<void()> callback) { m_onChange = std::move(callback); }
    void beginBatch() { ++m_batch; }
    void endBatch();

private:
    QString pathFor(const QString& id) const;
    void changed();
    QString m_dir;
    std::function<void()> m_onChange;
    int m_batch = 0;
    bool m_changedInBatch = false;
};

// Automatic backup: writes an export of all profiles to `folder` as
// gpufusion-profiles-<yyyyMMdd-HHmmss-zzz>.json, unless the newest backup there already holds the same
// profiles, and keeps only the newest `keep` backups (other files are never touched).
struct BackupResult {
    bool written = false;
    bool unchanged = false;     // identical to the newest backup: nothing written
    QString path;
    QString error;
    int removed = 0;            // old backups deleted beyond `keep`
};
BackupResult backupProfiles(const QList<AppProfile>& profiles, const QString& folder, int keep);
// The automatic backup of a store (used after every change and when the setting is saved). If any
// profile file cannot be read — e.g. damaged by a failing disk — nothing is written: a backup without
// those profiles would, after `keep` more changes, rotate away every backup that still has them.
BackupResult backupStore(const ProfileStore& store, const QString& folder, int keep);
// Whether two paths are on the same drive/volume (a backup there does not survive that drive failing).
bool onSameVolume(const QString& a, const QString& b);

// Self-updating programs often install each version into a sibling folder ("app-1.0.9188",
// "app-1.0.9265", "1.2.3"). If `exePath`'s folder looks like such a version folder and a newer sibling
// contains an executable of the same name, returns that one; otherwise returns `exePath` unchanged.
QString resolveCurrentExecutable(const QString& exePath);

// A profile for an executable the user picked (not saved).
AppProfile newProfile(const QString& exePath, const QString& name = {});
// A variant of `base` (same program, own mode and arguments), starting as a copy of it; not saved.
// Returns nullopt for an empty name.
std::optional<AppProfile> newVariant(const AppProfile& base, const QString& variantName);

// ---- backup / transfer ------------------------------------------------------------------------------
// One JSON document with every profile: { "gpuFusionProfiles": 1, "exported": ..., "profiles": [...] }.
QByteArray exportProfiles(const QList<AppProfile>& profiles);

struct ImportResult {
    bool ok = false;               // the document could be read
    QString error;
    int imported = 0, replaced = 0, skipped = 0;
    QStringList missingPrograms;   // imported, but the executable is not on this PC
    QString summary() const;
};
// Adds the profiles of an exported document to `store`. Existing profiles (same program) are kept
// unless `replace`. Verification results and applied Windows values are not imported — they describe
// the PC they were made on — and no Windows setting is changed: modes take effect when applied.
ImportResult importProfiles(ProfileStore& store, const QByteArray& document, bool replace);

} // namespace gf::profiles
