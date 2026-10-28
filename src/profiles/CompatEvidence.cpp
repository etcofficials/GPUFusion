#include "profiles/CompatEvidence.h"

#include <QDir>

namespace gf::profiles {

compat::ProgramEvidence evidenceFor(const QString& exePath, const QList<AppProfile>& profiles)
{
    compat::ProgramEvidence e;
    for (const AppProfile& p : profiles) {
        if (QDir::toNativeSeparators(p.exePath).compare(QDir::toNativeSeparators(exePath), Qt::CaseInsensitive) != 0 || !p.lastVerification)
            continue;
        const ProfileVerification& v = *p.lastVerification;
        if (v.mode == GpuMode::Integrated)
            e.ranOnIntegrated = v.confirmed;
        if (v.mode == GpuMode::Discrete)
            e.ranOnDiscrete = v.confirmed;
        if (v.confirmed)
            e.note = QStringLiteral("GPU Fusion's verification of profile “%1” (%2): %3").arg(displayName(p), v.time.toString(QStringLiteral("yyyy-MM-dd")), v.headline);
    }
    return e;
}

} // namespace gf::profiles
