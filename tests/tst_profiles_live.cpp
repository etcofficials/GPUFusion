// Live checks of generic profiles on this PC: GPUFusionProbe stands in for an ordinary program that
// integrates the GPU Fusion SDK. Each mode is applied (Windows preference written only where it is
// justified), the probe is launched from its profile and GPU Fusion observes where it really rendered.
// Only the probe's own preference is changed, and it is restored (snapshot + CTest cleanup fixture).
#include "hardware/AdapterDetector.h"
#include "platform/windows/ProbeRunner.h"
#include "platform/windows/RegistryPreferenceStore.h"
#include "profiles/AppDiscovery.h"
#include "profiles/KnownOptions.h"
#include "profiles/ProfileActions.h"
#include "profiles/ProfileStore.h"

#include "ProbePreferenceGuard.h"

#include <QProcess>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>
#include <QUrl>

using namespace gf;
using namespace gf::profiles;

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

class TestProfilesLive : public QObject {
    Q_OBJECT

    QTemporaryDir m_dir;
    hw::DetectionResult m_detection;
    QList<pref::SelectableGpu> m_gpus;
    pref::win::RegistryPreferenceStore m_store;
    std::unique_ptr<pref::ChangeJournal> m_journal;
    std::unique_ptr<pref::PreferenceManager> m_manager;
    CapabilityInputs m_inputs;
    bool m_hasIntegrated = false, m_hasDiscrete = false;

    // Applies `mode` to the probe's profile, launches it from the profile and observes it.
    mon::UsageVerdict run(GpuMode mode, AppProfile* out)
    {
        AppProfile p = newProfile(probeguard::probePath(), QStringLiteral("Probe"));
        p.arguments = QStringLiteral("--render 6");
        p.mode = mode;
        const ApplyResult applied = applyProfile(p, *m_manager, m_inputs);
        if (!applied.ok)
            qWarning().noquote() << "apply failed:" << applied.message;
        qInfo().noquote() << modeText(mode) << "— applied:" << applied.message;
        const mon::UsageVerdict v = verifyProfile(p, m_detection, m_manager->rawValue(probeguard::probePath()), 8);
        print(v);
        *out = p;
        return v;
    }

private Q_SLOTS:
    void initTestCase()
    {
        if (!QFileInfo::exists(probeguard::probePath()))
            QSKIP("GPUFusionProbe.exe is not built next to the tests");
        QVERIFY(m_dir.isValid());
        m_detection = hw::detectAdapters(nullptr);
        m_gpus = selectableGpus(m_detection);
        if (m_gpus.isEmpty())
            QSKIP("No physical GPU");
        for (const pref::SelectableGpu& g : m_gpus)
            (g.integrated ? m_hasIntegrated : m_hasDiscrete) = true;
        if (probeguard::take(m_store))
            QSKIP("The probe already has a preference; this test only runs from a clean state");
        m_journal = std::make_unique<pref::ChangeJournal>(m_dir.filePath(QStringLiteral("journal.json")));
        m_manager = std::make_unique<pref::PreferenceManager>(m_store, *m_journal);
        // What the Windows preference does on this PC (set -> probe -> restore, the probe only).
        m_inputs.gpus = m_gpus;
        m_inputs.probeVerification = pref::verifyPreferences(m_store, *m_journal, probeguard::probePath(),
                                                             [](const QString& probe, QString* error) { return pref::win::runProbe(probe, error); });
        QVERIFY(m_inputs.probeVerification->restored);
    }

    void cleanupTestCase()
    {
        QString error;
        QVERIFY2(probeguard::restore(m_store, &error), qPrintable(error));
    }

    // The probe uses the GPU passed in its profile (SDK), even where Windows cannot place it there.
    void integratedModeReachesSdkPrograms()
    {
        if (!m_hasIntegrated)
            QSKIP("No integrated GPU");
        AppProfile p;
        const mon::UsageVerdict v = run(GpuMode::Integrated, &p);
        QVERIFY2(p.lastVerification && p.lastVerification->confirmed, qPrintable(v.headline));
        if (referenceMachine())
            QVERIFY(p.appliedValue.isEmpty()); // no Windows value: it would not select Intel here
    }

    void discreteModeIsAppliedAndConfirmed()
    {
        if (!m_hasDiscrete)
            QSKIP("No discrete GPU");
        AppProfile p;
        const mon::UsageVerdict v = run(GpuMode::Discrete, &p);
        QVERIFY2(p.lastVerification && p.lastVerification->confirmed, qPrintable(v.headline));
        if (referenceMachine())
            QCOMPARE(p.appliedValue, QStringLiteral("GpuPreference=2;"));
    }

    // Both: the SDK-integrated probe renders on every GPU passed to it; GPU Fusion must see both used.
    void bothModeIsSeenOnBothGpus()
    {
        if (!m_hasIntegrated || !m_hasDiscrete)
            QSKIP("Needs an integrated and a discrete GPU");
        AppProfile p;
        const mon::UsageVerdict v = run(GpuMode::Both, &p);
        QCOMPARE(v.outcome, mon::UsageOutcome::BothAdapters);
        QVERIFY(p.lastVerification->confirmed);
        QVERIFY(p.appliedValue.isEmpty());
    }

    void automaticModeReportsWhereItRan()
    {
        AppProfile p;
        const mon::UsageVerdict v = run(GpuMode::Automatic, &p);
        QCOMPARE(v.outcome, mon::UsageOutcome::Observed);
        QVERIFY(p.appliedValue.isEmpty());
        QVERIFY(!m_manager->rawValue(probeguard::probePath()).has_value());
    }

    // A real, unmodified program put on the integrated GPU through its own option: a Chromium-based
    // browser with its known option (--use-adapter-luid={luid_pair}), a throwaway profile folder and a
    // local WebGL page. Only verifyProfile is used, so no Windows value is written for the browser.
    void realBrowserFollowsItsKnownOption()
    {
        if (!m_hasIntegrated || !m_hasDiscrete)
            QSKIP("Needs an integrated and a discrete GPU");
        QString browser;
        for (const QString& candidate : { qEnvironmentVariable("ProgramFiles(x86)") + QStringLiteral("\\Microsoft\\Edge\\Application\\msedge.exe"),
                                          qEnvironmentVariable("LOCALAPPDATA") + QStringLiteral("\\BraveSoftware\\Brave-Browser\\Application\\brave.exe") })
            if (browser.isEmpty() && QFileInfo::exists(candidate))
                browser = QDir::toNativeSeparators(candidate);
        if (browser.isEmpty())
            QSKIP("Neither Microsoft Edge nor Brave is installed");
        const auto known = knownOptionFor(browser, builtInKnownOptions());
        QVERIFY(known.has_value());

        QTemporaryDir dir(QDir(QDir::tempPath()).filePath(QStringLiteral("gfbrowser-XXXXXX")));
        QVERIFY(dir.isValid());
        QFile page(dir.filePath(QStringLiteral("webgl.html")));
        QVERIFY(page.open(QIODevice::WriteOnly));
        page.write(R"(<!doctype html><canvas id=c width=1280 height=720></canvas><script>
const g=document.getElementById('c').getContext('webgl');
const s=(t,x)=>{const h=g.createShader(t);g.shaderSource(h,x);g.compileShader(h);return h;};
const p=g.createProgram();g.attachShader(p,s(g.VERTEX_SHADER,'attribute vec2 a;void main(){gl_Position=vec4(a,0.,1.);}'));
g.attachShader(p,s(g.FRAGMENT_SHADER,'precision highp float;uniform float t;void main(){float v=0.;for(int i=0;i<200;i++){v+=sin(gl_FragCoord.x*float(i)*.01+t);}gl_FragColor=vec4(fract(v),0.,0.,1.);}'));
g.linkProgram(p);g.useProgram(p);g.bindBuffer(g.ARRAY_BUFFER,g.createBuffer());
g.bufferData(g.ARRAY_BUFFER,new Float32Array([-1,-1,1,-1,-1,1,1,1]),g.STATIC_DRAW);g.enableVertexAttribArray(0);g.vertexAttribPointer(0,2,g.FLOAT,false,0,0);
const u=g.getUniformLocation(p,'t');let t=0;(function f(){g.uniform1f(u,t+=.02);g.drawArrays(g.TRIANGLE_STRIP,0,4);requestAnimationFrame(f);})();
</script>)");
        page.close();
        const QString userData = QDir::toNativeSeparators(dir.filePath(QStringLiteral("profile")));
        auto closeBrowser = [&] { // only the processes of the throwaway profile
            QProcess::execute(QStringLiteral("powershell"), { QStringLiteral("-NoProfile"), QStringLiteral("-Command"),
                QStringLiteral("Get-CimInstance Win32_Process | Where-Object { $_.CommandLine -like '*%1*' } | "
                               "ForEach-Object { Stop-Process -Id $_.ProcessId -Force -ErrorAction SilentlyContinue }").arg(dir.path().section(u'/', -1)) });
            QThread::msleep(1500);
        };

        AppProfile p = newProfile(browser);
        // Chromium stops rendering windows it considers hidden or covered (e.g. while the desktop is
        // locked); these switches keep the test page rendering whatever the desktop's state.
        p.arguments = withKnownOption(QStringLiteral("\"--user-data-dir=%1\" --no-first-run --no-default-browser-check --disable-extensions "
                                                     "--disable-features=CalculateNativeWinOcclusion --disable-backgrounding-occluded-windows "
                                                     "--disable-renderer-backgrounding %2")
                                          .arg(userData, QUrl::fromLocalFile(page.fileName()).toString()), *known);
        p.mode = GpuMode::Integrated;
        const mon::UsageVerdict onIntel = verifyProfile(p, m_detection, std::nullopt, 12);
        closeBrowser();
        print(onIntel);
        QVERIFY2(p.lastVerification->confirmed, qPrintable(onIntel.headline));

        p.mode = GpuMode::Automatic; // the option is left out: the browser picks its default GPU
        const mon::UsageVerdict automatic = verifyProfile(p, m_detection, std::nullopt, 12);
        closeBrowser();
        print(automatic);
        QCOMPARE(automatic.outcome, mon::UsageOutcome::Observed);
    }

    void discoveryFindsInstalledPrograms()
    {
        const QList<DiscoveredApp> apps = discoverApps();
        qInfo() << apps.size() << "programs discovered";
        QVERIFY(!apps.isEmpty());
        for (const DiscoveredApp& a : apps) {
            QVERIFY2(QFileInfo(a.exePath).isFile(), qPrintable(a.exePath));
            QVERIFY(!a.name.isEmpty());
            QVERIFY(!looksLikeHelperExecutable(a.exePath));
        }
    }
};

QTEST_GUILESS_MAIN(TestProfilesLive)
#include "tst_profiles_live.moc"
