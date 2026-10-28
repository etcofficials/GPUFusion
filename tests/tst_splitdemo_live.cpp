// Live checks of GPUFusionSplitDemo â€” a program that integrates the GPU Fusion SDK â€” on this PC's
// GPUs: alone it uses the default adapter only; with every adapter it measures each GPU alone, splits
// the image with the adaptive scheduler and reports a verdict against the fastest GPU alone, with
// correct output; launched from a "Both GPUs" profile (GPU Fusion's variables, nothing written to
// Windows), GPU Fusion's own verification sees its work on both GPUs. Changes no settings.
#include "hardware/AdapterDetector.h"
#include "profiles/ProfileActions.h"
#include "profiles/ProfileStore.h"

#include <QCoreApplication>
#include <QDir>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTest>

using namespace gf;

namespace {

bool referenceMachine() { return qEnvironmentVariableIntValue("GPUFUSION_EXPECT_REFERENCE_MACHINE") == 1; }

QString demoPath() { return QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("GPUFusionSplitDemo.exe")); }

QJsonObject runDemo(const QStringList& args, const QProcessEnvironment& env = QProcessEnvironment::systemEnvironment())
{
    QProcess p;
    p.setProcessEnvironment(env);
    p.start(demoPath(), args);
    if (!p.waitForFinished(180000))
        return {};
    const QByteArray out = p.readAllStandardOutput();
    const QJsonObject o = QJsonDocument::fromJson(out).object();
    if (o.isEmpty())
        qWarning().noquote() << "demo output:" << out.left(400) << p.readAllStandardError().left(400);
    return o;
}

} // namespace

class TestSplitDemoLive : public QObject {
    Q_OBJECT
    hw::DetectionResult m_detection;
    QList<pref::SelectableGpu> m_gpus;

private Q_SLOTS:
    void initTestCase()
    {
        if (!QFileInfo::exists(demoPath()))
            QSKIP("GPUFusionSplitDemo.exe is not built next to the tests");
        m_detection = hw::detectAdapters(nullptr);
        m_gpus = profiles::selectableGpus(m_detection);
        if (m_gpus.isEmpty())
            QSKIP("No physical GPU");
    }

    // Not launched by GPU Fusion: the SDK gives the default adapter only â€” one GPU, nothing split.
    void withoutGpuFusionOneAdapter()
    {
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        for (const char* v : { "GPUFUSION_MODE", "GPUFUSION_ADAPTERS", "GPUFUSION_ENV_VERSION", "GPUFUSION_PROFILE" })
            env.remove(QString::fromLatin1(v));
        const QJsonObject o = runDemo({ QStringLiteral("--size"), QStringLiteral("quick"), QStringLiteral("--runs"), QStringLiteral("3") }, env);
        QVERIFY(!o.isEmpty());
        QCOMPARE(o.value(QLatin1StringView("launchedByGpuFusion")).toBool(), false);
        QCOMPARE(o.value(QLatin1StringView("adapters")).toArray().size(), 1);
        QCOMPARE(o.value(QLatin1StringView("verdict")).toObject().value(QLatin1StringView("usedGpus")).toInt(), 1);
        QVERIFY(o.value(QLatin1StringView("allOutputCorrect")).toBool());
    }

    // Every hardware adapter: per-GPU baselines, the adaptive split, a measured verdict, correct output.
    void allAdaptersMeasuredAgainstTheFastestGpu()
    {
        if (m_gpus.size() < 2)
            QSKIP("Needs two GPUs");
        const QJsonObject o = runDemo({ QStringLiteral("--all-adapters"), QStringLiteral("--size"), QStringLiteral("standard"),
                                        QStringLiteral("--runs"), QStringLiteral("8") });
        QVERIFY(!o.isEmpty());
        QVERIFY(o.value(QLatin1StringView("allOutputCorrect")).toBool());
        const QJsonArray adapters = o.value(QLatin1StringView("adapters")).toArray();
        QCOMPARE(adapters.size(), m_gpus.size());
        const QJsonArray single = o.value(QLatin1StringView("single")).toArray();
        QCOMPARE(single.size(), m_gpus.size());
        for (const QJsonValue& s : single)
            QVERIFY(s.toObject().value(QLatin1StringView("correctVsCpuSamples")).toBool());
        const QJsonObject adaptive = o.value(QLatin1StringView("adaptive")).toObject();
        QCOMPARE(adaptive.value(QLatin1StringView("runs")).toInt(), 8);
        const QJsonObject verdict = o.value(QLatin1StringView("verdict")).toObject();
        const QString result = verdict.value(QLatin1StringView("result")).toString();
        QVERIFY2(QStringList({ QStringLiteral("faster"), QStringLiteral("slower"), QStringLiteral("no significant difference") }).contains(result),
                 qPrintable(result));
        // The primary does most of the rows.
        const QJsonArray rows = adaptive.value(QLatin1StringView("rowsPercentPerGpu")).toArray();
        QVERIFY(rows.at(0).toObject().value(QLatin1StringView("percent")).toDouble() >= 50.0);
        qInfo().noquote() << "primary" << o.value(QLatin1StringView("primary")).toString() << "verdict" << result << "speedup"
                          << verdict.value(QLatin1StringView("speedup")).toDouble() << "rows" << QJsonDocument(rows).toJson(QJsonDocument::Compact);
        if (referenceMachine()) {
            QVERIFY(o.value(QLatin1StringView("primary")).toString().contains(QStringLiteral("GT 730")));
            for (const QJsonValue& a : adapters) {
                const QJsonObject ad = a.toObject();
                if (ad.value(QLatin1StringView("vendorId")).toInt() == 0x8086) { // HD 2000: FL 10_1, no compute shaders
                    QCOMPARE(ad.value(QLatin1StringView("featureLevel")).toString(), QStringLiteral("10_1"));
                    QCOMPARE(ad.value(QLatin1StringView("computeShaders")).toBool(), false);
                    QVERIFY(ad.value(QLatin1StringView("usable")).toBool());
                }
            }
        }
    }

    // A "Both GPUs" profile for the demo: GPU Fusion launches it with its variables (no Windows value
    // involved) and its own per-process verification sees GPU work on both GPUs â€” because this program
    // integrates the SDK and splits its work, not because of the mode alone.
    void bothProfileIsVerifiedOnBothGpus()
    {
        bool integrated = false, discrete = false;
        for (const pref::SelectableGpu& g : m_gpus)
            (g.integrated ? integrated : discrete) = true;
        if (!integrated || !discrete)
            QSKIP("Needs an integrated and a discrete GPU");
        profiles::AppProfile p = profiles::newProfile(demoPath(), QStringLiteral("Split demo"));
        p.mode = profiles::GpuMode::Both;
        p.arguments = QStringLiteral("--no-baselines --seconds 12 --size standard --policy balanced");
        const mon::UsageVerdict v = profiles::verifyProfile(p, m_detection, std::nullopt, 10);
        qInfo().noquote() << v.headline;
        for (const mon::UsageStage& s : v.stages)
            qInfo().noquote() << "  " << s.name << ":" << mon::stageStateText(s.state) << "â€”" << s.detail;
        QCOMPARE(v.outcome, mon::UsageOutcome::BothAdapters);
        QVERIFY(p.lastVerification && p.lastVerification->confirmed);
        QVERIFY(v.headline.startsWith(QStringLiteral("Both GPUs used")));
    }
};

QTEST_GUILESS_MAIN(TestSplitDemoLive)
#include "tst_splitdemo_live.moc"
