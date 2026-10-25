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

} // namespace gf::profiles
