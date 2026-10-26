#include "monitoring/UsageVerification.h"

#include <QFileInfo>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>

namespace gf::mon {

EngineKind classifyEngine(const QString& name)
{
    static const QRegularExpression unnamed(QStringLiteral(R"(^Engine \d+$)"));
    if (unnamed.match(name).hasMatch())
        return EngineKind::Unnamed;
    QString base = name.trimmed().toLower();
    static const QRegularExpression suffix(QStringLiteral(R"( \d+$)")); // "Video Decode 2" -> "video decode"
    base.remove(suffix);
    if (base == QLatin1StringView("3d"))
        return EngineKind::Render3D;
    if (base.startsWith(QLatin1StringView("compute")))
        return EngineKind::Compute;
    if (base.startsWith(QLatin1StringView("video")))
        return EngineKind::Video;
    if (base.startsWith(QLatin1StringView("copy")))
        return EngineKind::Copy;
    return EngineKind::Other;
}

QString stageStateText(StageState s)
{
    switch (s) {
    case StageState::Passed:        return QStringLiteral("Passed");
    case StageState::Failed:        return QStringLiteral("Failed");
    case StageState::Inconclusive:  return QStringLiteral("Inconclusive");
    case StageState::NotApplicable: return QStringLiteral("Not applicable");
    }
    return {};
}

namespace {

QString exeName(const QString& exe)
{
    const QString n = QFileInfo(exe).fileName();
    return n.isEmpty() ? exe : n;
}

struct Accumulator {
    AdapterActivity activity;
    QHash<int, double> renderByPass, unnamedByPass; // summed over the tree's processes, per pass
    QSet<QString> exes, renderExes;
};

QString percent(double v)
{
    return QStringLiteral("%1 %").arg(v, 0, 'f', v < 10 ? 1 : 0);
}

} // namespace

UsageVerdict evaluateUsage(const UsageEvidence& e)
{
    UsageVerdict v;
    QHash<quint64, QString> adapterNames;
    for (const ObservedAdapter& a : e.adapters)
        adapterNames.insert(a.luid, a.name);
    const QString requested = e.requestedName.isEmpty() && e.requestedLuid ? adapterNames.value(*e.requestedLuid) : e.requestedName;

    // ---- stage 1: preference saved ----
    UsageStage saved{ QStringLiteral("Preference saved") };
    if (!e.preferenceRequired) {
        saved.state = e.storedPreference ? StageState::Failed : StageState::NotApplicable;
        saved.detail = e.storedPreference ? QStringLiteral("A value is still stored (%1) although Windows should decide.").arg(*e.storedPreference)
                                          : QStringLiteral("No Windows preference is stored for this program.");
    } else if (e.storedPreference) {
        saved.state = StageState::Passed;
        saved.detail = QStringLiteral("Read back from Windows: %1").arg(*e.storedPreference);
    } else {
        saved.state = StageState::Failed;
        saved.detail = QStringLiteral("No preference is stored for this program.");
    }
    v.stages << saved;

    // ---- stage 2: launched ----
    UsageStage launched{ QStringLiteral("Application launched") };
    if (!e.launched) {
        launched.state = StageState::Failed;
        launched.detail = e.launchError.isEmpty() ? QStringLiteral("The program was not started.") : e.launchError;
        v.stages << launched << UsageStage{ QStringLiteral("GPU activity observed"), StageState::NotApplicable, {} }
                 << UsageStage{ QStringLiteral("Requested GPU confirmed"), StageState::NotApplicable, {} };
        v.outcome = UsageOutcome::NotLaunched;
        v.headline = QStringLiteral("Not verified: the program could not be started.");
        return v;
    }
    launched.state = StageState::Passed;
    launched.detail = QStringLiteral("Started and observed for %1 sampling passes (it and the processes it started).").arg(e.passes);
    if (e.alreadyRunning)
        launched.detail += QStringLiteral(" Another instance was already running: only processes started now are counted, and Windows "
                                          "applies a preference only when a program starts — close every instance for a clean test.");
    v.stages << launched;

    // ---- aggregate observations per adapter ----
    QHash<quint64, Accumulator> acc;
    for (const UsageObservation& o : e.observations) {
        Accumulator& a = acc[o.luid];
        a.activity.luid = o.luid;
        a.activity.name = adapterNames.value(o.luid, QStringLiteral("adapter %1").arg(o.luid, 0, 16));
        double render = 0, video = 0, copy = 0, unnamed = 0;
        for (const EngineSample& s : o.engines) {
            if (!s.utilization.isOk())
                continue;
            const double u = s.utilization.value;
            switch (classifyEngine(s.name)) {
            case EngineKind::Render3D:
            case EngineKind::Compute: render = std::max(render, u); break;
            case EngineKind::Video:   video = std::max(video, u); break;
            case EngineKind::Copy:    copy = std::max(copy, u); break;
            case EngineKind::Unnamed: unnamed = std::max(unnamed, u); break;
            case EngineKind::Other:   break;
            }
        }
        a.renderByPass[o.pass] = std::min(100.0, a.renderByPass.value(o.pass) + render);
        a.unnamedByPass[o.pass] = std::min(100.0, a.unnamedByPass.value(o.pass) + unnamed);
        a.activity.videoPeak = std::max(a.activity.videoPeak, video);
        a.activity.copyPeak = std::max(a.activity.copyPeak, copy);
        if (render >= kRenderPeakPercent || unnamed >= kRenderPeakPercent || video >= kRenderPeakPercent || copy >= kRenderPeakPercent)
            a.exes.insert(o.exe);
        if (render >= kRenderPeakPercent || unnamed >= kRenderPeakPercent)
            a.renderExes.insert(o.exe);
    }

    QSet<quint64> rendering, unnamedActive;
    QSet<QString> renderExes;
    for (auto it = acc.begin(); it != acc.end(); ++it) {
        Accumulator& a = it.value();
        int renderPasses = 0, unnamedPasses = 0;
        double sum = 0;
        for (double r : a.renderByPass) {
            a.activity.renderPeak = std::max(a.activity.renderPeak, r);
            sum += r;
            renderPasses += r >= kRenderPeakPercent ? 1 : 0;
        }
        for (double u : a.unnamedByPass) {
            a.activity.unnamedPeak = std::max(a.activity.unnamedPeak, u);
            unnamedPasses += u >= kRenderPeakPercent ? 1 : 0;
        }
        a.activity.renderMean = e.passes > 0 ? sum / e.passes : 0;
        if (renderPasses >= kRenderPasses)
            rendering.insert(it.key());
        if (unnamedPasses >= kRenderPasses)
            unnamedActive.insert(it.key());
        if (rendering.contains(it.key()) || unnamedActive.contains(it.key()))
            renderExes.unite(a.renderExes);
        QStringList exes(a.exes.begin(), a.exes.end());
        std::sort(exes.begin(), exes.end());
        a.activity.executables = exes;
        if (!exes.isEmpty())
            v.activity << a.activity;
    }
    std::sort(v.activity.begin(), v.activity.end(), [](const AdapterActivity& x, const AdapterActivity& y) { return x.renderPeak > y.renderPeak; });
    v.renderingExecutables = QStringList(renderExes.begin(), renderExes.end());
    std::sort(v.renderingExecutables.begin(), v.renderingExecutables.end());

    auto names = [&](const QSet<quint64>& luids) {
        QStringList out;
        for (quint64 l : luids)
            out << adapterNames.value(l, QStringLiteral("another adapter"));
        out.sort();
        return out.join(QStringLiteral(" and "));
    };

    // ---- stage 3: activity ----
    UsageStage activity{ QStringLiteral("GPU activity observed") };
    QStringList lines;
    for (const AdapterActivity& a : v.activity) {
        QStringList kinds;
        if (rendering.contains(a.luid))
            kinds << QStringLiteral("3D/compute up to %1 (mean %2)").arg(percent(a.renderPeak), percent(a.renderMean));
        if (unnamedActive.contains(a.luid))
            kinds << QStringLiteral("work of unknown type up to %1 (the driver does not name its engines)").arg(percent(a.unnamedPeak));
        if (a.videoPeak >= kRenderPeakPercent)
            kinds << QStringLiteral("video up to %1").arg(percent(a.videoPeak));
        if (a.copyPeak >= kRenderPeakPercent)
            kinds << QStringLiteral("copy up to %1").arg(percent(a.copyPeak));
        if (!kinds.isEmpty())
            lines << QStringLiteral("%1: %2").arg(a.name, kinds.join(QStringLiteral(", ")));
    }
    if (!rendering.isEmpty() || !unnamedActive.isEmpty()) {
        activity.state = StageState::Passed;
        activity.detail = lines.join(QStringLiteral("; "));
    } else {
        activity.state = StageState::Inconclusive;
        activity.detail = lines.isEmpty() ? QStringLiteral("No GPU engine activity from the program's processes while observed.")
                                          : QStringLiteral("Only video or copy activity, which is not rendering: %1").arg(lines.join(QStringLiteral("; ")));
    }
    v.stages << activity;

    // ---- stage 4 + outcome ----
    UsageStage confirmed{ QStringLiteral("Requested GPU confirmed") };
    if (!e.requestedLuid) {
        confirmed.state = StageState::NotApplicable;
        if (!rendering.isEmpty() || !unnamedActive.isEmpty()) {
            v.outcome = UsageOutcome::Observed;
            v.headline = QStringLiteral("The program ran its GPU work on %1.").arg(names(rendering.isEmpty() ? unnamedActive : rendering));
        } else {
            v.outcome = UsageOutcome::NoActivity;
            v.headline = QStringLiteral("The program did no measurable GPU work while observed, so its GPU could not be determined.");
        }
    } else {
        const quint64 req = *e.requestedLuid;
        QSet<quint64> others = rendering;
        others.remove(req);
        const bool reqRenders = rendering.contains(req);
        // GPU work of unknown type (drivers that do not name engines) on another adapter also means the
        // program used that adapter, although not necessarily for rendering.
        QSet<quint64> otherUnnamed = unnamedActive;
        otherUnnamed.remove(req);
        otherUnnamed.subtract(others);
        if (reqRenders && others.isEmpty() && !otherUnnamed.isEmpty()) {
            // Its rendering ran on the requested GPU; the other work is of unknown type and may be incidental.
            v.outcome = UsageOutcome::Confirmed;
            confirmed.state = StageState::Passed;
            confirmed.detail = QStringLiteral("3D/compute work of the program's processes ran on %1. It also did GPU work of unknown type on "
                                              "%2 (its driver does not report engine types), which may be incidental.")
                                   .arg(requested, names(otherUnnamed));
            v.headline = QStringLiteral("Confirmed: the program rendered on %1.").arg(requested);
            for (quint64 l : std::as_const(otherUnnamed))
                v.otherAdaptersWithUnnamedWork << adapterNames.value(l, QStringLiteral("another adapter"));
            v.otherAdaptersWithUnnamedWork.sort();
        } else if (reqRenders && others.isEmpty()) {
            v.outcome = UsageOutcome::Confirmed;
            confirmed.state = StageState::Passed;
            confirmed.detail = QStringLiteral("3D/compute work of the program's processes ran on %1 and on no other GPU.").arg(requested);
            v.headline = QStringLiteral("Confirmed: the program rendered on %1.").arg(requested);
        } else if (reqRenders) {
            v.outcome = UsageOutcome::BothAdapters;
            confirmed.state = StageState::Inconclusive;
            confirmed.detail = QStringLiteral("The program rendered on %1 and also on %2.").arg(requested, names(others));
            v.headline = QStringLiteral("Partly: the program used %1 and %2.").arg(requested, names(others));
        } else if (!others.isEmpty()) {
            v.outcome = UsageOutcome::WrongAdapter;
            confirmed.state = StageState::Failed;
            confirmed.detail = QStringLiteral("The program rendered on %1, not on %2.").arg(names(others), requested);
            v.headline = QStringLiteral("Not applied: the program rendered on %1 instead of %2.").arg(names(others), requested);
        } else if (unnamedActive.contains(req) && unnamedActive.size() == 1) {
            v.outcome = UsageOutcome::ConfirmedUnnamed;
            confirmed.state = StageState::Passed;
            confirmed.detail = QStringLiteral("GPU work of the program ran only on %1. Its driver does not report engine types, so this "
                                              "shows where the work ran, not that it was 3D rendering.").arg(requested);
            v.headline = QStringLiteral("Confirmed: the program's GPU work ran on %1 (engine type not reported by its driver).").arg(requested);
        } else if (!unnamedActive.isEmpty()) {
            QSet<quint64> elsewhere = unnamedActive;
            elsewhere.remove(req);
            v.outcome = UsageOutcome::WrongAdapter;
            confirmed.state = StageState::Failed;
            confirmed.detail = QStringLiteral("GPU work of the program ran on %1, not on %2.").arg(names(elsewhere), requested);
            v.headline = QStringLiteral("Not applied: the program's GPU work ran on %1 instead of %2.").arg(names(elsewhere), requested);
        } else {
            v.outcome = UsageOutcome::NoActivity;
            confirmed.state = StageState::Inconclusive;
            confirmed.detail = QStringLiteral("No rendering was observed, so the GPU it would use could not be determined.");
            v.headline = QStringLiteral("Not verified: the program did no measurable GPU work while observed (it may need to be "
                                        "used, or its window shown, while GPU Fusion watches).");
        }
    }
    v.stages << confirmed;

    // ---- launcher resolution ----
    const QString target = exeName(e.targetExe);
    QStringList otherExes;
    for (const QString& x : v.renderingExecutables) {
        if (exeName(x).compare(target, Qt::CaseInsensitive) != 0)
            otherExes << x;
    }
    const bool targetRendered = otherExes.size() < v.renderingExecutables.size();
    if (!otherExes.isEmpty())
        v.details << (targetRendered
            ? QStringLiteral("Processes it started also did GPU work: %1.").arg(otherExes.join(QStringLiteral(", ")))
            : QStringLiteral("The GPU work came from %1, not from %2 itself — %2 is probably a launcher. A preference must be set for the "
                             "executable that renders.").arg(otherExes.join(QStringLiteral(", ")), target));
    if (v.outcome == UsageOutcome::NoActivity && e.alreadyRunning)
        v.details << QStringLiteral("An instance was already running; the new launch may have handed over to it and exited.");
    if (!e.uninspected.isEmpty() && !e.legacyAdapters.isEmpty()) {
        QStringList skipped;
        for (const QString& x : e.uninspected)
            skipped << exeName(x);
        skipped.removeDuplicates();
        v.details << QStringLiteral("GPU use on %1 could not be read for %2 process(es) of the program (%3): Windows did not allow "
                                    "GPU Fusion to inspect them, so work there may be missing from this result. Running GPU Fusion as "
                                    "administrator can read them.")
                         .arg(e.legacyAdapters.join(QStringLiteral(", "))).arg(e.uninspected.size()).arg(skipped.join(QStringLiteral(", ")));
        if (v.outcome == UsageOutcome::NoActivity)
            v.headline = QStringLiteral("Not verified: no measurable GPU work was seen, but some of the program's processes could not be "
                                        "inspected on %1.").arg(e.legacyAdapters.join(QStringLiteral(", ")));
    }
    return v;
}

} // namespace gf::mon
