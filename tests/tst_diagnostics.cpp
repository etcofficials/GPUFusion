// Pure tests for the diagnostic report: issue rules, multi-GPU assessment, metric availability,
// rendering, log parsing and privacy redaction — with synthetic inputs shaped like the reference PC.
#include "diagnostics/DiagnosticReport.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTest>

using namespace gf;
using namespace gf::diag;
using hw::ApiProbe;

namespace {

const QString kH264 = QStringLiteral("{1B81BE68-A0C7-11D3-B984-00C04F2E73C5}");
const QString kMpeg2 = QStringLiteral("{EE27417F-5E28-4E65-BEEA-1D26B508ADC9}");
const QString kVc1 = QStringLiteral("{1B81BEA3-A0C7-11D3-B984-00C04F2E73C5}");

hw::Adapter gpu(const QString& name, quint32 vendor, quint64 luid, int wddm, const QString& fl, bool compute, bool d3d12, QStringList profiles)
{
    hw::Adapter a;
    hw::DxgiAdapter d;
    d.description = name;
    d.vendorId = vendor;
    d.luid = luid;
    d.wddmVersion = wddm;
    d.dedicatedVideoMemory = vendor == 0x8086 ? 32ull << 20 : 4ull << 30;
    d.d3d11.device = { ApiProbe::State::Succeeded, fl, 0, {} };
    d.d3d11.computeShaders = compute;
    d.d3d11.videoDevice = true;
    d.d3d11.decoderProfiles = std::move(profiles);
    d.d3d12.device = d3d12 ? ApiProbe{ ApiProbe::State::Succeeded, QStringLiteral("11_0"), 0, {} }
                           : ApiProbe{ ApiProbe::State::Failed, {}, qint32(0x887A0004), QStringLiteral("The driver does not support this API (HRESULT 0x887A0004)") };
    d.d3d12.maxFeatureLevel = QStringLiteral("11_0");
    a.dxgi = d;
    a.classification = hw::classify(d);
    a.findings = hw::findingsFor(a, QDate(2026, 10, 16));
    return a;
}

hw::DetectionResult referencePc()
{
    hw::DetectionResult r;
    r.adapters << gpu(QStringLiteral("NVIDIA GeForce GT 730"), 0x10DE, 1, 2300, QStringLiteral("11_0"), true, true, { kH264, kMpeg2, kVc1 });
    r.adapters << gpu(QStringLiteral("Intel(R) HD Graphics"), 0x8086, 2, 1200, QStringLiteral("10_1"), false, false, { kH264, kMpeg2 });
    hw::Adapter warp;
    warp.dxgi = hw::DxgiAdapter{};
    warp.dxgi->description = QStringLiteral("Microsoft Basic Render Driver");
    warp.dxgi->softwareFlag = true;
    warp.dxgi->luid = 3;
    warp.classification = hw::classify(*warp.dxgi);
    r.adapters << warp;
    return r;
}

mon::Snapshot snapshot()
{
    using mon::MetricValue;
    mon::Snapshot s;
    s.collectionMs = 0.4;
    mon::AdapterSample nv;
    nv.name = QStringLiteral("NVIDIA GeForce GT 730");
    nv.utilization = MetricValue::ok(3);
    nv.engines << mon::EngineSample{ 0, QStringLiteral("3D"), MetricValue::ok(3) };
    nv.dedicatedUsed = nv.sharedUsed = MetricValue::ok(1);
    nv.temperature = nv.fanPercent = nv.pstate = MetricValue::ok(40);
    nv.powerWatts = nv.coreClockMHz = nv.memoryClockMHz = MetricValue::unsupported(QStringLiteral("NVML reports this is not supported"));
    nv.sources << QStringLiteral("D3DKMT") << QStringLiteral("NVML");
    mon::AdapterSample in;
    in.name = QStringLiteral("Intel(R) HD Graphics");
    in.utilization = MetricValue::ok(0);
    in.engines << mon::EngineSample{ 0, QStringLiteral("Engine 0"), MetricValue::ok(0) };
    in.dedicatedUsed = in.sharedUsed = MetricValue::ok(0);
    in.temperature = in.fanPercent = in.powerWatts = in.coreClockMHz = in.memoryClockMHz = in.pstate =
        MetricValue::unsupported(QStringLiteral("No sensor interface is available for this adapter"));
    s.adapters << nv << in;
    return s;
}

ReportInputs inputs()
{
    ReportInputs in;
    in.generated = QDateTime(QDate(2026, 10, 16), QTime(19, 0));
    in.appVersion = QStringLiteral("0.1.0");
    in.detection = referencePc();
    in.snapshot = snapshot();
    in.providers << mon::ProviderStatus{ QStringLiteral("D3DKMT statistics"), true, QStringLiteral("2 of 2 adapters"), {} }
                 << mon::ProviderStatus{ QStringLiteral("NVIDIA NVML"), true, QStringLiteral("Loaded"), { QStringLiteral("power draw"), QStringLiteral("core clock") } };
    in.dataRoot = QStringLiteral("G:\\GPUFusion\\data");
    return in;
}

const Issue* find(const Report& r, const QString& titleStart)
{
    for (const Issue& i : r.issues) {
        if (i.title.startsWith(titleStart))
            return &i;
    }
    return nullptr;
}
// A pointer into a temporary report would dangle (found by the debug heap): keep the report in a variable.
const Issue* find(Report&&, const QString&) = delete;

const Technique* technique(const QList<Technique>& list, const QString& start)
{
    for (const Technique& t : list) {
        if (t.name.startsWith(start))
            return &t;
    }
    return nullptr;
}

} // namespace

class TestDiagnostics : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void referencePcIssues()
    {
        const Report r = buildReport(inputs());
        QCOMPARE(r.count(Issue::Severity::Error), 0);
        const Issue* legacy = find(r, QStringLiteral("Legacy WDDM 1.2"));
        QVERIFY(legacy);
        QCOMPARE(legacy->severity, Issue::Severity::Warning);
        QCOMPARE(legacy->subject, QStringLiteral("Intel(R) HD Graphics"));
        QVERIFY(!legacy->actions.isEmpty());
        QVERIFY(find(r, QStringLiteral("No temperature, fan, power or clock sensors"))); // Intel
        QVERIFY(find(r, QStringLiteral("Some NVIDIA sensors are not exposed")));
        QVERIFY(find(r, QStringLiteral("Not verified on this PC"))); // preference verification missing
        QVERIFY(!find(r, QStringLiteral("Software renderer")));     // WARP is not an issue
        for (int i = 1; i < r.issues.size(); ++i) // most severe first
            QVERIFY(int(r.issues[i - 1].severity) <= int(r.issues[i].severity));
    }

    void noGpuAndDetectionErrorsAreErrors()
    {
        ReportInputs in = inputs();
        in.detection->adapters.removeFirst();
        in.detection->adapters.removeFirst();
        in.detection->errors << QStringLiteral("Could not start DXGI adapter enumeration");
        const Report r = buildReport(in);
        QVERIFY(find(r, QStringLiteral("No physical GPU")));
        QVERIFY(find(r, QStringLiteral("Detection was incomplete")));
        QCOMPARE(r.count(Issue::Severity::Error), 2);
        QCOMPARE(r.issues.first().severity, Issue::Severity::Error);
        ReportInputs none = inputs();
        none.detection.reset();
        const Report noneReport = buildReport(none);
        QVERIFY(find(noneReport, QStringLiteral("Hardware detection has not completed")));
    }

    void nvmlProblemsSuggestTheRightFix()
    {
        ReportInputs in = inputs();
        in.providers[1] = mon::ProviderStatus{ QStringLiteral("NVIDIA NVML"), false, QStringLiteral("Disabled in Settings."), {} };
        const Report disabledReport = buildReport(in);
        const Issue* disabled = find(disabledReport, QStringLiteral("NVIDIA sensors are not available"));
        QVERIFY(disabled && disabled->actions.first().contains(QStringLiteral("Settings")));
        in.providers[1].detail = QStringLiteral("nvml.dll not found");
        const Report missingReport = buildReport(in);
        const Issue* missing = find(missingReport, QStringLiteral("NVIDIA sensors are not available"));
        QVERIFY(missing && missing->actions.first().contains(QStringLiteral("Reinstall")));
    }

    void preferenceVerdicts()
    {
        ReportInputs in = inputs();
        pref::VerificationResult v;
        v.time = in.generated;
        v.restored = true;
        for (auto p : { pref::GpuPreference::NotSet, pref::GpuPreference::PowerSaving }) {
            pref::VerificationStep s;
            s.preference = p;
            s.ok = true;
            s.report.d3d11Default = pref::ProbeAdapter{ QStringLiteral("NVIDIA GeForce GT 730"), 0x10DE, QStringLiteral("x") };
            v.steps << s;
        }
        in.preferenceVerification = v;
        const Report noneReport = buildReport(in);
        const Issue* none = find(noneReport, QStringLiteral("The Windows GPU preference makes no difference"));
        QVERIFY(none && none->detail.contains(QStringLiteral("GT 730")));
        in.preferenceVerification->restored = false;
        in.preferenceVerification->restoreError = QStringLiteral("access denied");
        const Report stuckReport = buildReport(in);
        const Issue* stuck = find(stuckReport, QStringLiteral("The probe's preference was not restored"));
        QVERIFY(stuck && stuck->severity == Issue::Severity::Warning);
    }

    void storageAndSettingsProblemsAreWarnings()
    {
        ReportInputs in = inputs();
        in.dataFallback = true;
        in.dataProblem = QStringLiteral("cannot create folder");
        in.settingsWarnings << QStringLiteral("theme: expected one of dark/light");
        const Report r = buildReport(in);
        QVERIFY(find(r, QStringLiteral("Data is stored in the fallback folder")));
        QVERIFY(find(r, QStringLiteral("The settings file needed corrections")));
    }

    void multiGpuAssessmentOfTheReferencePc()
    {
        const QList<Technique> t = assessMultiGpu(referencePc(), OpenClInfo{ false, { QStringLiteral("nvopencl.dll") } });
        QCOMPARE(technique(t, QStringLiteral("Separate Direct3D 11"))->verdict, Technique::Verdict::Feasible);
        const Technique* compute = technique(t, QStringLiteral("Direct3D compute"));
        QCOMPARE(compute->verdict, Technique::Verdict::NotFeasible);
        QVERIFY(compute->reason.contains(QStringLiteral("Intel")));
        const Technique* d3d12 = technique(t, QStringLiteral("Direct3D 12 explicit"));
        QCOMPARE(d3d12->verdict, Technique::Verdict::NotFeasible);
        QVERIFY(d3d12->reason.contains(QStringLiteral("Intel")) && !d3d12->reason.contains(QStringLiteral("NVIDIA")));
        QCOMPARE(technique(t, QStringLiteral("Sharing GPU memory"))->verdict, Technique::Verdict::NotFeasible);
        const Technique* video = technique(t, QStringLiteral("Hardware video decoding"));
        QCOMPARE(video->verdict, Technique::Verdict::Feasible);
        QVERIFY(video->reason.contains(QStringLiteral("H.264, MPEG-2")));
        QVERIFY(!video->reason.contains(QStringLiteral("VC-1"))); // only the NVIDIA has VC-1
        const Technique* cl = technique(t, QStringLiteral("OpenCL"));
        QCOMPARE(cl->verdict, Technique::Verdict::NotFeasible);
        QVERIFY(cl->reason.contains(QStringLiteral("OpenCL.dll")) && cl->reason.contains(QStringLiteral("nvopencl.dll")));
        QCOMPARE(technique(t, QStringLiteral("Linked-GPU"))->verdict, Technique::Verdict::NotFeasible);
    }

    void singleGpuHasNoMultiGpuTechnique()
    {
        hw::DetectionResult one = referencePc();
        one.adapters.removeAt(1);
        const QList<Technique> t = assessMultiGpu(one, {});
        QCOMPARE(t.size(), 1);
        QCOMPARE(t.first().verdict, Technique::Verdict::NotFeasible);
    }

    void metricAvailabilityNeverInventsValues()
    {
        const Report r = buildReport(inputs());
        const Table m = r.metricTable();
        QCOMPARE(m.header, (QStringList{ QStringLiteral("Metric"), QStringLiteral("NVIDIA GeForce GT 730"), QStringLiteral("Intel(R) HD Graphics") }));
        auto row = [&](const QString& metric) {
            for (const QStringList& rr : m.rows) {
                if (rr.first() == metric)
                    return rr;
            }
            return QStringList();
        };
        QCOMPARE(row(QStringLiteral("GPU utilization")), (QStringList{ QStringLiteral("GPU utilization"), QStringLiteral("Available"), QStringLiteral("Available") }));
        QCOMPARE(row(QStringLiteral("Temperature")), (QStringList{ QStringLiteral("Temperature"), QStringLiteral("Available"), QStringLiteral("Unsupported") }));
        QCOMPARE(row(QStringLiteral("Power draw")), (QStringList{ QStringLiteral("Power draw"), QStringLiteral("Unsupported"), QStringLiteral("Unsupported") }));
    }

    void markdownAndJsonRendering()
    {
        const Report r = buildReport(inputs());
        const QString md = r.toMarkdown();
        for (const char* heading : { "# GPU Fusion diagnostic report", "## Issues and suggestions", "## Graphics adapters", "## Monitoring",
                                     "## GPU preference", "## Multi-GPU compatibility", "Privacy:" })
            QVERIFY2(md.contains(QLatin1StringView(heading)), heading);
        QVERIFY(md.contains(QStringLiteral("| Direct3D 12 | Feature level 11_0 | Not available:")));
        QVERIFY(!md.contains(QStringLiteral("0 °C")));
        const QJsonObject j = r.toJson();
        QCOMPARE(j.value(QStringLiteral("schemaVersion")).toInt(), 1);
        QCOMPARE(j.value(QStringLiteral("issues")).toArray().size(), r.issues.size());
        QCOMPARE(j.value(QStringLiteral("summary")).toObject().value(QStringLiteral("warnings")).toInt(), r.count(Issue::Severity::Warning));
        QCOMPARE(j.value(QStringLiteral("adapters")).toArray().size(), 3);
        QVERIFY(QJsonDocument(j).toJson().size() > 500);
    }

    void parsesRecentLogProblems()
    {
        const QString log = QStringLiteral(
            "2026-10-16T18:00:00.000 INFO  [app] (t1) started\n"
            "2026-10-16T18:00:01.000 WARN  [monitor] (t2) Monitoring data is stale\n"
            "2026-10-16T18:00:02.000 ERROR [preferences] (t3) restore failed\n"
            "    second line of the error\n"
            "2026-10-16T18:00:03.000 DEBUG [ui] (t1) page\n"
            "garbage line\n"
            "2026-10-16T18:00:04.000 WARN  [hardware] (t4) third\n");
        const QList<LogLine> all = parseLogProblems(log, 10);
        QCOMPARE(all.size(), 3);
        QCOMPARE(all[0].subsystem, QStringLiteral("monitor"));
        QCOMPARE(all[1].severity, QStringLiteral("ERROR"));
        QCOMPARE(all[1].message, QStringLiteral("restore failed second line of the error"));
        const QList<LogLine> last = parseLogProblems(log, 2);
        QCOMPARE(last.size(), 2);
        QCOMPARE(last.last().message, QStringLiteral("third"));
    }

    void redactionRemovesIdentifiersOnly()
    {
        // Fictional identifiers; the user name deliberately also occurs inside ordinary words.
        const Redactor r{ QStringLiteral("C:\\Users\\DEV"), QStringLiteral("DEV"), QStringLiteral("DESKTOP-AB12CDE") };
        const QString in = QStringLiteral("Log: C:\\Users\\DEV\\AppData\\x.log; json \"C:\\\\Users\\\\DEV\\\\x\"; slash C:/Users/DEV/y; "
                                          "owner DESKTOP-AB12CDE\\DEV on desktop-ab12cde; device drivers, dev. Repo github.com/devtools stays.");
        const QString out = r.apply(in);
        QVERIFY2(!out.contains(QStringLiteral("Users\\DEV"), Qt::CaseInsensitive), qPrintable(out));
        QVERIFY(!out.contains(QStringLiteral("Users\\\\DEV"), Qt::CaseInsensitive));
        QVERIFY(!out.contains(QStringLiteral("Users/DEV"), Qt::CaseInsensitive));
        QVERIFY(!out.contains(QStringLiteral("AB12CDE"), Qt::CaseInsensitive));
        QVERIFY(out.contains(QStringLiteral("%USERPROFILE%\\AppData\\x.log")));
        QVERIFY(out.contains(QStringLiteral("<computer>\\<user>")));
        QVERIFY(out.contains(QStringLiteral("device drivers, dev.")));   // ordinary words untouched
        QVERIFY(out.contains(QStringLiteral("github.com/devtools")));    // not a path of this user
    }
};

QTEST_GUILESS_MAIN(TestDiagnostics)
#include "tst_diagnostics.moc"
