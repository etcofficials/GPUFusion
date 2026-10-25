// Pure tests: adapter-oriented GPU choices (what to write and whether Windows can enforce it),
// evaluation of observed per-process GPU usage, engine classification, and the dual-GPU assessment.
#include "monitoring/UsageVerification.h"
#include "preferences/DualGpu.h"
#include "preferences/GpuSelection.h"

#include <QTest>

using namespace gf;
using namespace gf::pref;
using namespace gf::mon;

namespace {

const SelectableGpu kNvidia{ QStringLiteral("NVIDIA GeForce GT 730"), 0x10DE, QStringLiteral("00000000_00007DEC"), false };
const SelectableGpu kIntel{ QStringLiteral("Intel(R) HD Graphics"), 0x8086, QStringLiteral("00000000_0000870A"), true };
const ProbeAdapter kNvProbe{ kNvidia.name, kNvidia.vendorId, kNvidia.luid };
const ProbeAdapter kIntelProbe{ kIntel.name, kIntel.vendorId, kIntel.luid };

VerificationStep step(GpuPreference p, const ProbeAdapter& got, const ProbeAdapter& minimumPower, const ProbeAdapter& highPerformance)
{
    VerificationStep s;
    s.preference = p;
    s.ok = true;
    s.report.d3d11Default = got;
    s.report.minimumPower = minimumPower;
    s.report.highPerformance = highPerformance;
    return s;
}

// The reference PC: every preference -> GT 730; Windows ranks the GT 730 first even for minimum power.
VerificationResult referencePc()
{
    VerificationResult v;
    v.restored = true;
    for (auto p : { GpuPreference::NotSet, GpuPreference::LetWindowsDecide, GpuPreference::PowerSaving, GpuPreference::HighPerformance })
        v.steps << step(p, kNvProbe, kNvProbe, kNvProbe);
    return v;
}

// A hybrid laptop: power saving -> Intel, high performance -> NVIDIA.
VerificationResult hybridLaptop()
{
    VerificationResult v;
    v.restored = true;
    v.steps << step(GpuPreference::NotSet, kIntelProbe, kIntelProbe, kNvProbe) << step(GpuPreference::PowerSaving, kIntelProbe, kIntelProbe, kNvProbe)
            << step(GpuPreference::HighPerformance, kNvProbe, kIntelProbe, kNvProbe);
    return v;
}

const QList<SelectableGpu> kGpus{ kNvidia, kIntel };
constexpr quint64 kNvLuid = 0x7DEC, kIntelLuid = 0x870A;

UsageEvidence evidence(std::optional<quint64> requested)
{
    UsageEvidence e;
    e.targetExe = QStringLiteral("C:\\Games\\game.exe");
    e.requestedLuid = requested;
    e.storedPreference = QStringLiteral("GpuPreference=2;");
    e.launched = true;
    e.passes = 6;
    e.adapters = { { kNvLuid, kNvidia.name }, { kIntelLuid, kIntel.name } };
    return e;
}

void addRendering(UsageEvidence& e, quint64 luid, const QString& engine, double percent, const QString& exe = QStringLiteral("C:\\Games\\game.exe"),
                  int passes = 5)
{
    for (int pass = 1; pass <= passes; ++pass)
        e.observations << UsageObservation{ pass, 100, exe, luid, { EngineSample{ 0, engine, MetricValue::ok(percent) } } };
}

} // namespace

class TestGpuSelection : public QObject {
    Q_OBJECT
private Q_SLOTS:
    // ---- choices ------------------------------------------------------------------------------
    void referencePcIntelIsNotEnforceableAndNotWritten()
    {
        const ChoicePlan p = planChoice(GpuChoice::Integrated, kGpus, referencePc());
        QCOMPARE(p.target->name, kIntel.name);
        QCOMPARE(p.enforcement, Enforcement::NotEnforceable);
        QVERIFY(!p.write.has_value()); // never store a value that would not do what it says
        QVERIFY2(p.summary.contains(kNvidia.name), qPrintable(p.summary));
        QVERIFY(p.reasons.join(u' ').contains(QStringLiteral("ranks NVIDIA GeForce GT 730 first")));
    }

    void referencePcNvidiaIsVerified()
    {
        const ChoicePlan p = planChoice(GpuChoice::Discrete, kGpus, referencePc());
        QCOMPARE(p.enforcement, Enforcement::Verified);
        QCOMPARE(p.write, GpuPreference::HighPerformance);
        QVERIFY(p.reasons.join(u' ').contains(QStringLiteral("even without a preference")));
    }

    void verificationFromBeforeARebootStillApplies()
    {
        // Same GPUs, new LUIDs (Windows reassigns them at boot).
        SelectableGpu nv = kNvidia, intel = kIntel;
        nv.luid = QStringLiteral("00000000_00007E2C");
        intel.luid = QStringLiteral("00000000_000088B5");
        QCOMPARE(planChoice(GpuChoice::Discrete, { nv, intel }, referencePc()).enforcement, Enforcement::Verified);
        QCOMPARE(planChoice(GpuChoice::Integrated, { nv, intel }, referencePc()).enforcement, Enforcement::NotEnforceable);
    }

    void hybridLaptopBothChoicesVerified()
    {
        const ChoicePlan intel = planChoice(GpuChoice::Integrated, kGpus, hybridLaptop());
        QCOMPARE(intel.enforcement, Enforcement::Verified);
        QCOMPARE(intel.write, GpuPreference::PowerSaving);
        const ChoicePlan nv = planChoice(GpuChoice::Discrete, kGpus, hybridLaptop());
        QCOMPARE(nv.enforcement, Enforcement::Verified);
        QCOMPARE(nv.write, GpuPreference::HighPerformance);
    }

    void withoutVerificationNothingIsWritten()
    {
        for (auto c : { GpuChoice::Integrated, GpuChoice::Discrete }) {
            const ChoicePlan p = planChoice(c, kGpus, std::nullopt);
            QCOMPARE(p.enforcement, Enforcement::Unknown);
            QVERIFY(!p.write.has_value());
        }
    }

    void rankingOnlyEvidenceIsExpectedNotVerified()
    {
        VerificationResult v; // verification covered only "not set", but recorded Windows' ranking
        v.steps << step(GpuPreference::NotSet, kNvProbe, kIntelProbe, kNvProbe);
        const ChoicePlan p = planChoice(GpuChoice::Integrated, kGpus, v);
        QCOMPARE(p.enforcement, Enforcement::Expected);
        QCOMPARE(p.write, GpuPreference::PowerSaving);
        VerificationResult w;
        w.steps << step(GpuPreference::NotSet, kNvProbe, kNvProbe, kNvProbe);
        QCOMPARE(planChoice(GpuChoice::Integrated, kGpus, w).enforcement, Enforcement::NotEnforceable);
    }

    void automaticRemovesTheValueAndBothNeverWrites()
    {
        const ChoicePlan a = planChoice(GpuChoice::Automatic, kGpus, referencePc());
        QCOMPARE(a.write, GpuPreference::NotSet);
        QCOMPARE(a.enforcement, Enforcement::NotApplicable);
        const ChoicePlan b = planChoice(GpuChoice::Both, kGpus, referencePc());
        QVERIFY(!b.write.has_value());
        QCOMPARE(b.enforcement, Enforcement::NotApplicable);
        QVERIFY(b.summary.contains(QStringLiteral("Not possible")));
    }

    void ambiguousOrMissingTargets()
    {
        QVERIFY(!gpuFor(GpuChoice::Integrated, { kNvidia }).has_value());
        SelectableGpu second = kNvidia;
        second.luid = QStringLiteral("00000000_00009999");
        QVERIFY(!gpuFor(GpuChoice::Discrete, { kNvidia, second, kIntel }).has_value()); // two discrete: ambiguous
        const ChoicePlan p = planChoice(GpuChoice::Integrated, { kNvidia }, referencePc());
        QVERIFY(!p.write.has_value());
        QVERIFY(p.summary.contains(QStringLiteral("No single integrated GPU")));
    }

    // ---- observed usage --------------------------------------------------------------------------
    void engineClassification()
    {
        QCOMPARE(classifyEngine(QStringLiteral("3D")), EngineKind::Render3D);
        QCOMPARE(classifyEngine(QStringLiteral("Compute_0")), EngineKind::Compute);
        QCOMPARE(classifyEngine(QStringLiteral("Video Decode 2")), EngineKind::Video);
        QCOMPARE(classifyEngine(QStringLiteral("Video Processing")), EngineKind::Video);
        QCOMPARE(classifyEngine(QStringLiteral("Copy 1")), EngineKind::Copy);
        QCOMPARE(classifyEngine(QStringLiteral("Engine 0")), EngineKind::Unnamed);
        QCOMPARE(classifyEngine(QStringLiteral("Overlay")), EngineKind::Other);
    }

    void confirmedOnRequestedAdapter()
    {
        UsageEvidence e = evidence(kNvLuid);
        e.preferenceRequired = true;
        addRendering(e, kNvLuid, QStringLiteral("3D"), 60);
        const UsageVerdict v = evaluateUsage(e);
        QCOMPARE(v.outcome, UsageOutcome::Confirmed);
        QCOMPARE(v.stages.size(), 4);
        for (const UsageStage& s : v.stages)
            QCOMPARE(s.state, StageState::Passed);
    }

    void wrongAdapterIsReported()
    {
        UsageEvidence e = evidence(kIntelLuid);
        addRendering(e, kNvLuid, QStringLiteral("3D"), 60);
        const UsageVerdict v = evaluateUsage(e);
        QCOMPARE(v.outcome, UsageOutcome::WrongAdapter);
        QCOMPARE(v.stages[3].state, StageState::Failed);
        QVERIFY(v.headline.contains(QStringLiteral("instead of Intel")));
    }

    void unnamedEnginesOnlyGiveAQualifiedConfirmation()
    {
        UsageEvidence e = evidence(kIntelLuid);
        addRendering(e, kIntelLuid, QStringLiteral("Engine 0"), 90);
        const UsageVerdict v = evaluateUsage(e);
        QCOMPARE(v.outcome, UsageOutcome::ConfirmedUnnamed);
        QVERIFY(v.stages[3].detail.contains(QStringLiteral("not that it was 3D rendering")));
    }

    void videoCopyAndOtherProcessesAreNotRendering()
    {
        UsageEvidence e = evidence(kNvLuid);
        addRendering(e, kNvLuid, QStringLiteral("Video Decode 1"), 80); // video decoding is not rendering
        addRendering(e, kNvLuid, QStringLiteral("Copy"), 50);
        const UsageVerdict v = evaluateUsage(e);
        QCOMPARE(v.outcome, UsageOutcome::NoActivity);
        QCOMPARE(v.stages[2].state, StageState::Inconclusive);
        QVERIFY(v.stages[2].detail.contains(QStringLiteral("not rendering")));
    }

    void briefBlipIsNotEnough()
    {
        UsageEvidence e = evidence(kNvLuid);
        addRendering(e, kNvLuid, QStringLiteral("3D"), 80, QStringLiteral("C:\\Games\\game.exe"), 1); // one pass only
        QCOMPARE(evaluateUsage(e).outcome, UsageOutcome::NoActivity);
    }

    void bothAdaptersAndLauncherResolution()
    {
        UsageEvidence e = evidence(kNvLuid);
        e.targetExe = QStringLiteral("C:\\Launcher\\launcher.exe");
        addRendering(e, kNvLuid, QStringLiteral("3D"), 50, QStringLiteral("C:\\Games\\engine.exe"));
        addRendering(e, kIntelLuid, QStringLiteral("Engine 0"), 0); // idle Intel: ignored
        addRendering(e, kIntelLuid, QStringLiteral("3D"), 20, QStringLiteral("C:\\Games\\engine.exe"));
        const UsageVerdict v = evaluateUsage(e);
        QCOMPARE(v.outcome, UsageOutcome::BothAdapters);
        QCOMPARE(v.renderingExecutables, QStringList{ QStringLiteral("C:\\Games\\engine.exe") });
        QVERIFY(v.details.join(u' ').contains(QStringLiteral("probably a launcher")));
    }

    void unnamedWorkOnTheOtherAdapterCountsAsBoth()
    {
        UsageEvidence e = evidence(kNvLuid);
        addRendering(e, kNvLuid, QStringLiteral("3D"), 45);
        addRendering(e, kIntelLuid, QStringLiteral("Engine 0"), 90);
        const UsageVerdict v = evaluateUsage(e);
        QCOMPARE(v.outcome, UsageOutcome::BothAdapters);
        QVERIFY(v.stages[3].detail.contains(QStringLiteral("does not report engine types")));
        QVERIFY(v.headline.startsWith(QStringLiteral("Partly")));
    }

    void notLaunchedAndAlreadyRunningAndNotSaved()
    {
        UsageEvidence e = evidence(kNvLuid);
        e.launched = false;
        e.launchError = QStringLiteral("The system cannot find the file specified.");
        const UsageVerdict v = evaluateUsage(e);
        QCOMPARE(v.outcome, UsageOutcome::NotLaunched);
        QCOMPARE(v.stages[1].detail, e.launchError);

        UsageEvidence r = evidence(kNvLuid);
        r.alreadyRunning = true;
        const UsageVerdict rv = evaluateUsage(r);
        QVERIFY(rv.stages[1].detail.contains(QStringLiteral("close every instance"))); // relaunch requirement
        QVERIFY(rv.details.join(u' ').contains(QStringLiteral("handed over")));

        UsageEvidence s = evidence(kNvLuid);
        s.storedPreference.reset();
        QCOMPARE(evaluateUsage(s).stages[0].state, StageState::Failed);
        s.preferenceRequired = false;                                   // Automatic
        QCOMPARE(evaluateUsage(s).stages[0].state, StageState::NotApplicable);
    }

    void automaticReportsWhereItRan()
    {
        UsageEvidence e = evidence(std::nullopt);
        e.preferenceRequired = false;
        e.storedPreference.reset();
        addRendering(e, kNvLuid, QStringLiteral("3D"), 40);
        const UsageVerdict v = evaluateUsage(e);
        QCOMPARE(v.outcome, UsageOutcome::Observed);
        QCOMPARE(v.stages[3].state, StageState::NotApplicable);
        QVERIFY(v.headline.contains(kNvidia.name));
    }

    // ---- dual GPU ----------------------------------------------------------------------------------
    void dualGpuOnTheReferencePc()
    {
        MultiGpuSystem sys{ 2, 1, false, false, { kIntel.name } };
        const DualGpuAssessment a = assessDualGpu(QStringLiteral("game.exe"), QStringList{ QStringLiteral("d3d12.dll"), QStringLiteral("kernel32.dll") }, {}, sys);
        QCOMPARE(a.support, DualGpuSupport::NotPossible);
        QCOMPARE(a.cases.size(), 4);
        QVERIFY(a.cases[0].contains(QStringLiteral("not possible on this PC")));
        QVERIFY(a.cases[0].contains(kIntel.name));
        QVERIFY(a.cases[2].contains(QStringLiteral("does not move programs")));
        QVERIFY(a.evidence.first().contains(QStringLiteral("Direct3D 12")));
    }

    void dualGpuWhereHardwareAllowsIt()
    {
        MultiGpuSystem sys{ 2, 2, false, true, {} };
        QCOMPARE(assessDualGpu(QStringLiteral("a.exe"), QStringList{ QStringLiteral("d3d12.dll") }, {}, sys).support,
                 DualGpuSupport::OnlyIfApplicationImplementsIt);
        QCOMPARE(assessDualGpu(QStringLiteral("b.exe"), QStringList{ QStringLiteral("d3d11.dll") }, {}, sys).support, DualGpuSupport::NotPossible);
        QCOMPARE(assessDualGpu(QStringLiteral("c.exe"), std::nullopt, QStringLiteral("access denied"), sys).support, DualGpuSupport::Unknown);
        QCOMPARE(assessDualGpu(QStringLiteral("d.exe"), QStringList{}, {}, MultiGpuSystem{ 1, 1, true, false, {} }).support, DualGpuSupport::NotPossible);
    }

    void apiDetection()
    {
        const GraphicsApis a = apisFromImports({ QStringLiteral("D3D11.dll"), QStringLiteral("dxgi.dll"), QStringLiteral("OPENGL32.DLL"),
                                                 QStringLiteral("vulkan-1.dll"), QStringLiteral("cudart64_110.dll") });
        QVERIFY(a.d3d11 && a.dxgi && a.opengl && a.vulkan && a.cuda && !a.d3d12 && !a.d3d9 && !a.opencl);
        QCOMPARE(a.names(), (QStringList{ QStringLiteral("Direct3D 11"), QStringLiteral("OpenGL"), QStringLiteral("Vulkan"), QStringLiteral("CUDA") }));
    }
};

QTEST_GUILESS_MAIN(TestGpuSelection)
#include "tst_gpuselection.moc"
