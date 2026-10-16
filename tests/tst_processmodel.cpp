// Pure-logic tests for per-process GPU data: PDH instance parsing (real instance names seen on the
// reference machine), aggregation, the "memory alone is not usage" rule, and preference parsing.
#include "monitoring/ProcessModel.h"

#include <QTest>

using namespace gf::mon;

class TestProcessModel : public QObject {
    Q_OBJECT

    static PdhEngineReading engine(const char* name, MetricValue v) { return { *parsePdhEngineInstance(QString::fromLatin1(name)), v }; }
    static PdhMemoryReading memory(const char* name, double dedicated, double shared)
    {
        return { *parsePdhProcessInstance(QString::fromLatin1(name)), MetricValue::ok(dedicated), MetricValue::ok(shared) };
    }

private Q_SLOTS:
    void parsesEngineInstances()
    {
        const auto e = parsePdhEngineInstance(QStringLiteral("pid_1344_luid_0x00000000_0x00007eda_phys_0_eng_1_engtype_compute_0"));
        QVERIFY(e);
        QCOMPARE(e->pid, 1344u);
        QCOMPARE(e->luid, 0x7EDAull);
        QCOMPARE(e->phys, 0);
        QCOMPARE(e->engine, 1);
        QCOMPARE(e->engineType, QStringLiteral("compute_0"));
        const auto upper = parsePdhEngineInstance(QStringLiteral("pid_780_luid_0x00000001_0x00007EDA_phys_0_eng_0_engtype_3D"));
        QVERIFY(upper);
        QCOMPARE(upper->luid, (1ull << 32) | 0x7EDA);
        QVERIFY(parsePdhEngineInstance(QStringLiteral("pid_9_luid_0x00000000_0x00008946_phys_0_eng_4_engtype_")));
        QVERIFY(!parsePdhEngineInstance(QStringLiteral("luid_0x00000000_0x00007eda_phys_0"))); // adapter memory instance
        QVERIFY(!parsePdhEngineInstance(QStringLiteral("_Total")));
    }

    void parsesProcessMemoryInstances()
    {
        const auto p = parsePdhProcessInstance(QStringLiteral("pid_6224_luid_0x00000000_0x00007eda_phys_0"));
        QVERIFY(p);
        QCOMPARE(p->pid, 6224u);
        QCOMPARE(p->luid, 0x7EDAull);
        QVERIFY(!parsePdhProcessInstance(QStringLiteral("pid_6224_luid_0x00000000_0x00007eda_phys_0_eng_0_engtype_3D")));
    }

    void aggregatesPerProcessAndAdapter()
    {
        const QList<PdhEngineReading> engines{
            engine("pid_1344_luid_0x00000000_0x00007eda_phys_0_eng_0_engtype_3D", MetricValue::ok(1.5)),
            engine("pid_1344_luid_0x00000000_0x00007eda_phys_0_eng_1_engtype_compute_0", MetricValue::ok(9.7)),
            engine("pid_1344_luid_0x00000000_0x00007eda_phys_0_eng_6_engtype_Copy", MetricValue::ok(0.1)),
            engine("pid_780_luid_0x00000000_0x00007eda_phys_0_eng_0_engtype_3D", MetricValue::ok(1.9)),
            engine("pid_55_luid_0x00000000_0x00008946_phys_0_eng_0_engtype_3D", MetricValue::ok(40)), // WARP
            engine("pid_55_luid_0x00000000_0x0000ffff_phys_0_eng_0_engtype_3D", MetricValue::ok(80)), // unknown adapter
        };
        const QList<PdhMemoryReading> mem{
            memory("pid_1344_luid_0x00000000_0x00007eda_phys_0", 149420.0 * 1024, 2048),
            memory("pid_6224_luid_0x00000000_0x00007eda_phys_0", 107276.0 * 1024, 0), // memory, no engine instances
        };
        const auto byPid = aggregatePdh(engines, mem, { 0x7EDA, 0x8946 });
        QCOMPARE(byPid.size(), 4); // 1344, 780, 55 (WARP only), 6224; the unknown adapter is ignored

        const ProcessAdapterUsage& dwm = byPid.value(1344).first();
        QCOMPARE(dwm.utilization.value, 9.7);
        QCOMPARE(dwm.busiestEngine, QStringLiteral("compute_0"));
        QCOMPARE(dwm.engines.size(), 3);
        QCOMPARE(dwm.dedicated.value, 149420.0 * 1024);
        QCOMPARE(activityOf(dwm), Activity::Active);

        QCOMPARE(byPid.value(55).size(), 1);
        QCOMPARE(byPid.value(55).first().luid, 0x8946ull);

        const ProcessAdapterUsage& explorer = byPid.value(6224).first();
        QVERIFY(explorer.utilization.isOk());
        QCOMPARE(explorer.utilization.value, 0.0);
        QCOMPARE(activityOf(explorer), Activity::MemoryOnly); // memory alone is not GPU usage
        QVERIFY(explorer.busiestEngine.isEmpty());
    }

    void duplicateEngineTypesAreNumbered()
    {
        const auto byPid = aggregatePdh({ engine("pid_7_luid_0x00000000_0x00007eda_phys_0_eng_5_engtype_VideoDecode", MetricValue::ok(0)),
                                          engine("pid_7_luid_0x00000000_0x00007eda_phys_0_eng_3_engtype_VideoDecode", MetricValue::ok(12)),
                                          engine("pid_7_luid_0x00000000_0x00007eda_phys_0_eng_0_engtype_3D", MetricValue::ok(1)) },
                                        {}, { 0x7EDA });
        const ProcessAdapterUsage& u = byPid.value(7).first();
        QCOMPARE(u.engines.size(), 3);
        QCOMPARE(u.engines[0].name, QStringLiteral("3D"));
        QCOMPARE(u.engines[1].name, QStringLiteral("Video Decode 1")); // engine 3 (sorted by engine index)
        QCOMPARE(u.engines[2].name, QStringLiteral("Video Decode 2")); // engine 5
        QCOMPARE(u.busiestEngine, QStringLiteral("Video Decode 1"));
    }

    void firstSampleIsUnknownNotZero()
    {
        const auto byPid = aggregatePdh({ engine("pid_1_luid_0x00000000_0x00007eda_phys_0_eng_0_engtype_3D", MetricValue::unknown(QStringLiteral("x"))) },
                                        {}, { 0x7EDA });
        const ProcessAdapterUsage& u = byPid.value(1).first();
        QCOMPARE(u.utilization.state, MetricValue::State::Unknown);
        QCOMPARE(activityOf(u), Activity::None);
        QCOMPARE(u.dedicated.state, MetricValue::State::Unavailable); // no counter, not 0 bytes
    }

    void activityThreshold()
    {
        ProcessAdapterUsage u;
        u.engines << EngineSample{ 0, QStringLiteral("3D"), MetricValue::ok(0.05) };
        u.dedicated = MetricValue::ok(0);
        u.shared = MetricValue::ok(0);
        QCOMPARE(activityOf(u), Activity::None);
        u.engines[0].utilization = MetricValue::ok(kActiveThresholdPercent);
        QCOMPARE(activityOf(u), Activity::Active);
    }

    void engineDisplayNames()
    {
        QCOMPARE(pdhEngineDisplayName(QStringLiteral("3D"), 0), QStringLiteral("3D"));
        QCOMPARE(pdhEngineDisplayName(QStringLiteral("videodecode"), 3), QStringLiteral("Video Decode"));
        QCOMPARE(pdhEngineDisplayName(QStringLiteral("legacyoverlay"), 2), QStringLiteral("Overlay"));
        QCOMPARE(pdhEngineDisplayName(QStringLiteral("Compute_0"), 1), QStringLiteral("Compute_0"));
        QCOMPARE(pdhEngineDisplayName(QString(), 4), QStringLiteral("Engine 4"));
    }

    void parsesWindowsGpuPreference()
    {
        QCOMPARE(parseGpuPreference(QStringLiteral("GpuPreference=2;")), GpuPreference::HighPerformance);
        QCOMPARE(parseGpuPreference(QStringLiteral("GpuPreference=1;")), GpuPreference::PowerSaving);
        QCOMPARE(parseGpuPreference(QStringLiteral("GpuPreference=0;")), GpuPreference::LetWindowsDecide);
        QCOMPARE(parseGpuPreference(QStringLiteral("SwapEffectUpgradeEnable=1;GpuPreference=2;")), GpuPreference::HighPerformance);
        QCOMPARE(parseGpuPreference(QStringLiteral("GpuPreference=1;SwapEffectUpgradeEnable=1;")), GpuPreference::PowerSaving);
        QCOMPARE(parseGpuPreference(QStringLiteral("SwapEffectUpgradeEnable=1;")), GpuPreference::Unrecognised);
        QCOMPARE(parseGpuPreference(QStringLiteral("GpuPreference=7;")), GpuPreference::Unrecognised);
        QCOMPARE(parseGpuPreference(QStringLiteral("XGpuPreference=2;")), GpuPreference::Unrecognised);
        QCOMPARE(parseGpuPreference(QString()), GpuPreference::NotSet);
    }
};

QTEST_GUILESS_MAIN(TestProcessModel)
#include "tst_processmodel.moc"
