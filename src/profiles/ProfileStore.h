#pragma once

#include "profiles/AppProfile.h"

#include <QList>
#include <QString>

namespace gf::profiles {

// One JSON file per program in <data>\config\profiles\<id>.json, written atomically — editing one
// program's profile never touches another's.
class ProfileStore {
public:
    explicit ProfileStore(QString directory);

    QString directory() const { return m_dir; }
    QList<AppProfile> list() const;                       // sorted by name
    std::optional<AppProfile> find(const QString& idOrExe) const;
    bool save(AppProfile& profile, QString* error);        // fills id/created/modified
    bool remove(const QString& id, QString* error);

private:
    QString pathFor(const QString& id) const;
    QString m_dir;
};

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
