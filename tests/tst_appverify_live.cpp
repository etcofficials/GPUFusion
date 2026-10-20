// Live checks of real per-application GPU verification on this PC's hardware. GPUFusionProbe
// --render stands in for an application: it creates its Direct3D 11 device the way ordinary
// programs do (default adapter) — or on a chosen adapter, like programs with their own GPU setting —
// and renders, while GPU Fusion observes its process tree. Only the probe's own preference is ever
// changed, and it is restored (snapshot + CTest cleanup fixture, as in tst_preferences_live).
#include "hardware/AdapterDetector.h"
#include "monitoring/UsageObserver.h"
#include "platform/windows/PeImports.h"
#include "platform/windows/RegistryPreferenceStore.h"
#include "preferences/DualGpu.h"

#include "ProbePreferenceGuard.h"

#include <QTest>

using namespace gf;

namespace {

bool referenceMachine() { return qEnvironmentVariable("GPUFUSION_EXPECT_REFERENCE_MACHINE") == QLatin1StringView("1"); }

void print(const mon::UsageVerdict& v)
{
    qInfo().noquote() << "  " << v.headline;
    for (const mon::UsageStage& s : v.stages)
        qInfo().noquote() << "    " << s.name << ":" << mon::stageStateText(s.state) << "—" << s.detail;
    for (const QString& d : v.details)
        qInfo().noquote() << "    " << d;
}

} // namespace

class TestAppVerifyLive : public QObject {
    Q_OBJECT

    QList<mon::MonitorTarget> m_gpus;
    std::optional<mon::MonitorTarget> m_discrete, m_integrated;

    mon::UsageVerdict observe(const QStringList& args, std::optional<quint64> requested, const QString& exe = probeguard::probePath(),
                              std::optional<QString> stored = std::nullopt, bool required = false)
    {
        mon::ObserveOptions o;
        o.exe = exe;
        o.arguments = args;
        o.seconds = 8;
        mon::UsageEvidence e = mon::observeUsage(m_gpus, o);
        e.requestedLuid = requested;
        e.storedPreference = stored;
        e.preferenceRequired = required;
        return mon::evaluateUsage(e);
    }

private Q_SLOTS:
    void initTestCase()
    {
        if (!QFileInfo::exists(probeguard::probePath()))
            QSKIP("GPUFusionProbe.exe is not built next to the tests");
        const hw::DetectionResult d = hw::detectAdapters(nullptr);
        for (const mon::MonitorTarget& t : mon::targetsFrom(d)) {
            if (t.software)
                continue;
            m_gpus << t;
        }
        for (const hw::Adapter& a : d.adapters) {
            if (!a.dxgi || a.isSoftware())
                continue;
            for (const mon::MonitorTarget& t : m_gpus) {
                if (t.luid != a.dxgi->luid)
                    continue;
                if (a.classification.value == hw::AdapterClass::Integrated)
                    m_integrated = t;
                else if (a.classification.value == hw::AdapterClass::Discrete)
                    m_discrete = t;
            }
        }
        if (m_gpus.isEmpty())
            QSKIP("No physical GPU");
    }

    // The probe renders on the default adapter; the observer must attribute its 3D work to that GPU.
    void defaultAdapterRenderingIsConfirmed()
    {
        if (!m_discrete)
            QSKIP("No discrete GPU");
        const mon::UsageVerdict v = observe({ QStringLiteral("--render"), QStringLiteral("6") }, m_discrete->luid);
        print(v);
        if (referenceMachine()) // GT 730 drives the display: default programs land on it
            QCOMPARE(v.outcome, mon::UsageOutcome::Confirmed);
        else
            QVERIFY(v.outcome != mon::UsageOutcome::NoActivity && v.outcome != mon::UsageOutcome::NotLaunched);
    }

    // An application that picks the Intel GPU itself: the observer must see that work on the Intel GPU,
    // proving that a "wrong adapter" result elsewhere is not a blind spot for that GPU.
    void explicitlyChosenIntegratedGpuIsSeen()
    {
        if (!m_integrated)
            QSKIP("No integrated GPU");
        const mon::UsageVerdict v = observe({ QStringLiteral("--render"), QStringLiteral("6"), QStringLiteral("--adapter"), hw::formatLuid(m_integrated->luid) },
                                            m_integrated->luid);
        print(v);
        QVERIFY2(v.outcome == mon::UsageOutcome::Confirmed || v.outcome == mon::UsageOutcome::ConfirmedUnnamed, qPrintable(v.headline));
    }

    // Windows' "Power saving" preference for the probe, then the probe renders with its default device:
    // does Windows move it to the integrated GPU? On the reference PC it does not (measured).
    void powerSavingPreferenceOutcomeIsMeasured()
    {
        if (!m_integrated || !m_discrete)
            QSKIP("Needs an integrated and a discrete GPU");
        pref::win::RegistryPreferenceStore store;
        const std::optional<QString> original = probeguard::take(store);
        if (original)
            QSKIP("The probe already has a preference; this test only runs from a clean state");
        QString error;
        QVERIFY2(store.write(probeguard::probePath(), QStringLiteral("GpuPreference=1;"), &error), qPrintable(error));
        const auto stored = store.read(probeguard::probePath());
        const mon::UsageVerdict v = observe({ QStringLiteral("--render"), QStringLiteral("6") }, m_integrated->luid, probeguard::probePath(), stored, true);
        QVERIFY(probeguard::restore(store, &error));
        QVERIFY(!pref::findValueName(store, probeguard::probePath()).has_value()); // back to "not set"
        print(v);
        QCOMPARE(v.stages[0].state, mon::StageState::Passed); // saved and read back
        if (referenceMachine()) {
            QCOMPARE(v.outcome, mon::UsageOutcome::WrongAdapter); // Windows kept it on the GT 730
            QVERIFY(v.headline.contains(m_discrete->name));
        } else {
            QVERIFY(v.outcome != mon::UsageOutcome::NotLaunched);
        }
    }

    // A launcher that starts the real renderer: the work must be traced to the child executable.
    void launcherChildIsIdentified()
    {
        const QString cmd = QDir(qEnvironmentVariable("SystemRoot")).filePath(QStringLiteral("System32\\cmd.exe"));
        // cmd.exe starts the probe as its child (the probe path has no spaces in the build tree).
        const mon::UsageVerdict v = observe({ QStringLiteral("/c"), probeguard::probePath(), QStringLiteral("--render"), QStringLiteral("6") },
                                            std::nullopt, cmd);
        print(v);
        QCOMPARE(v.outcome, mon::UsageOutcome::Observed);
        QVERIFY(std::any_of(v.renderingExecutables.begin(), v.renderingExecutables.end(),
                            [](const QString& x) { return x.endsWith(QStringLiteral("GPUFusionProbe.exe"), Qt::CaseInsensitive); }));
        QVERIFY(v.details.join(u' ').contains(QStringLiteral("launcher")));
    }

    void importsAreReadFromRealExecutables()
    {
        QString error;
        const auto probe = pref::win::readImports(probeguard::probePath(), &error);
        QVERIFY2(probe, qPrintable(error));
        QVERIFY(probe->contains(QStringLiteral("d3d11.dll")));
        QVERIFY(probe->contains(QStringLiteral("dxgi.dll")));
        const pref::GraphicsApis apis = pref::apisFromImports(*probe);
        QVERIFY(apis.d3d11 && !apis.d3d12);
        const auto app = pref::win::readImports(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("GPUFusion.exe")), &error);
        QVERIFY2(app, qPrintable(error));
        QVERIFY(app->contains(QStringLiteral("qt6core.dll")));
        QVERIFY(!pref::win::readImports(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("GPUFusion.datadir")), &error));
        QVERIFY(!pref::win::readImports(QStringLiteral("Z:\\no\\such\\program.exe"), &error));
    }
};

QTEST_GUILESS_MAIN(TestAppVerifyLive)
#include "tst_appverify_live.moc"
