#pragma once

#include "preferences/PreferenceModel.h"

namespace gf::pref::win {

// The real store: HKCU\Software\Microsoft\DirectX\UserGpuPreferences (per user; no administrator
// rights). Only REG_SZ values named after executables are read or written; nothing else is touched.
class RegistryPreferenceStore : public IPreferenceStore {
public:
    QStringList names() const override;
    std::optional<QString> read(const QString& name) const override;
    bool write(const QString& name, const QString& data, QString* error) override;
    bool remove(const QString& name, QString* error) override;
};

} // namespace gf::pref::win
