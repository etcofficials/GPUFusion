#include "profiles/LiveCheck.h"

#include "hardware/AdapterModel.h"

#include <QSet>

namespace gf::profiles {

LiveCheck liveCheck(const QList<AppProfile>& profiles, const QList<mon::ProcessAdapterUsage>& usage,
                    const QList<pref::SelectableGpu>& gpus, const QHash<quint64, QString>& adapterNames)
{
    LiveCheck c;
    if (profiles.isEmpty())
        return c;

    // What the profiles ask for: luid text -> the profiles asking for it.
    QHash<QString, QStringList> expected;
    QStringList modes;
    for (const AppProfile& p : profiles) {
        modes << (profiles.size() > 1 ? QStringLiteral("%1: %2").arg(p.variant.isEmpty() ? QStringLiteral("main") : p.variant, modeText(p.mode))
                                      : modeText(p.mode));
        if (p.mode == GpuMode::Automatic)
            continue;
        if (p.mode == GpuMode::Both) {
            for (const pref::SelectableGpu& g : gpus)
                expected[g.luid.toUpper()] << displayName(p);
        } else if (const auto g = modeGpu(p.mode, gpus)) {
            expected[g->luid.toUpper()] << displayName(p);
        }
    }
    const QString modeSummary = modes.join(QStringLiteral(", "));
    if (expected.isEmpty()) {
        c.state = LiveCheck::State::Automatic;
        c.text = modeSummary;
        c.detail = QStringLiteral("Its GPU profile does not ask for a GPU (Automatic).");
        return c;
    }

    QStringList activeNames, matching;
    for (const mon::ProcessAdapterUsage& u : usage) {
        if (mon::activityOf(u) != mon::Activity::Active)
            continue;
        const QString name = adapterNames.value(u.luid, hw::formatLuid(u.luid));
        activeNames << name;
        const auto it = expected.constFind(hw::formatLuid(u.luid).toUpper());
        if (it != expected.cend())
            matching << QStringLiteral("%1 (%2)").arg(name, it->join(QStringLiteral(", ")));
    }
    if (activeNames.isEmpty()) {
        c.state = LiveCheck::State::NoGpuWork;
        c.text = QStringLiteral("%1 · idle").arg(modeSummary);
        c.detail = QStringLiteral("Its GPU profile: %1. It is doing no GPU work at the moment, so there is nothing to compare.").arg(modeSummary);
        return c;
    }
    if (!matching.isEmpty()) {
        c.state = LiveCheck::State::Matches;
        c.text = QStringLiteral("✓ %1").arg(modeSummary);
        c.detail = QStringLiteral("Working on %1, as its GPU profile asks.").arg(matching.join(QStringLiteral("; ")));
        return c;
    }
    c.state = LiveCheck::State::Elsewhere;
    c.text = QStringLiteral("✗ %1").arg(modeSummary); // where it runs instead: the GPU column, tooltip and details
    c.detail = QStringLiteral("Working on %1, although its GPU profile asks for %2. A profile's GPU option and arguments apply only when "
                              "the program is started through GPU Fusion (Launch, a GPU Fusion shortcut, --launch-profile); a Windows "
                              "preference only when the program starts after it was applied. Close the program and start it through "
                              "its profile.").arg(activeNames.join(QStringLiteral(", ")), modeSummary);
    return c;
}

} // namespace gf::profiles
