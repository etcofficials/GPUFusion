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
    QStringList modes, bothProfiles;
    for (const AppProfile& p : profiles) {
        modes << (profiles.size() > 1 ? QStringLiteral("%1: %2").arg(p.variant.isEmpty() ? QStringLiteral("main") : p.variant, modeText(p.mode))
                                      : modeText(p.mode));
        if (p.mode == GpuMode::Automatic)
            continue;
        if (p.mode == GpuMode::Both) {
            // Not "any of the GPUs": Both is only what it asks for when the program works on two of them.
            bothProfiles << displayName(p);
        } else if (const auto g = modeGpu(p.mode, gpus)) {
            expected[g->luid.toUpper()] << displayName(p);
        }
    }
    const QString modeSummary = modes.join(QStringLiteral(", "));
    if (expected.isEmpty() && bothProfiles.isEmpty()) {
        c.state = LiveCheck::State::Automatic;
        c.text = modeSummary;
        c.detail = QStringLiteral("Its GPU profile does not ask for a GPU (Automatic).");
        return c;
    }

    QSet<QString> physical;
    for (const pref::SelectableGpu& g : gpus)
        physical.insert(g.luid.toUpper());
    QStringList activeNames, activeGpus, matching;
    for (const mon::ProcessAdapterUsage& u : usage) {
        if (mon::activityOf(u) != mon::Activity::Active)
            continue;
        const QString name = adapterNames.value(u.luid, hw::formatLuid(u.luid));
        activeNames << name;
        if (physical.contains(hw::formatLuid(u.luid).toUpper()))
            activeGpus << name;
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
    if (!bothProfiles.isEmpty() && activeGpus.size() >= 2) {
        c.state = LiveCheck::State::BothActive;
        c.text = QStringLiteral("✓ Both GPUs active · %1").arg(modeSummary);
        c.detail = QStringLiteral("The program's own GPU work is on %1 at the moment, as %2 (Both GPUs) offers. Work on both GPUs "
                                  "does not show that the program splits one task or that it is faster; only a measured comparison "
                                  "with one GPU alone shows that.").arg(activeGpus.join(QStringLiteral(" and ")), bothProfiles.join(QStringLiteral(", ")));
        return c;
    }
    if (!matching.isEmpty()) {
        c.state = LiveCheck::State::Matches;
        c.text = QStringLiteral("✓ %1").arg(modeSummary);
        c.detail = QStringLiteral("Working on %1, as its GPU profile asks.").arg(matching.join(QStringLiteral("; ")));
        return c;
    }
    if (!bothProfiles.isEmpty()) {
        const QString where = activeGpus.isEmpty() ? activeNames.join(QStringLiteral(", ")) : activeGpus.join(QStringLiteral(", "));
        c.state = LiveCheck::State::OneOfBoth;
        c.text = QStringLiteral("One GPU only (%1) · %2").arg(where, modeSummary);
        c.detail = QStringLiteral("Its profile %1 offers both GPUs, but at the moment the program works only on %2. Both GPUs are passed "
                                  "to programs that integrate the GPU Fusion SDK; other programs ignore them and run on one GPU, as in "
                                  "Automatic. GPU Fusion does not split another program's work.").arg(bothProfiles.join(QStringLiteral(", ")), where);
        return c;
    }
    // A profile in Automatic mode accepts any GPU. GPU Fusion cannot tell which of the program's profiles
    // started it, so working elsewhere is consistent with that profile, not a proven mismatch.
    QStringList automaticProfiles;
    for (const AppProfile& p : profiles)
        if (p.mode == GpuMode::Automatic)
            automaticProfiles << displayName(p);
    if (!automaticProfiles.isEmpty()) {
        c.state = LiveCheck::State::Matches;
        c.text = QStringLiteral("✓ %1").arg(modeSummary);
        c.detail = QStringLiteral("Working on %1: consistent with %2 (Automatic). If it was started for one of its other profiles (%3), "
                                  "it is not on that profile's GPU.")
                       .arg(activeNames.join(QStringLiteral(", ")), automaticProfiles.join(QStringLiteral(", ")), modeSummary);
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
