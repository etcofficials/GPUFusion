// Pure-logic tests for monitoring: metric states, utilization arithmetic, memory split, history
// bounds and staleness. The key property: a value that cannot be read never becomes a zero.
#include "core/RingBuffer.h"
#include "monitoring/History.h"
#include "monitoring/MetricModel.h"

#include <QTest>

#include <cmath>

using namespace gf;
using namespace gf::mon;

class TestMetricModel : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void ringBufferKeepsNewestInOrder()
    {
        RingBuffer<int> rb(3);
        QVERIFY(rb.isEmpty());
        for (int i = 1; i <= 5; ++i)
            rb.push(i);
        QCOMPARE(rb.size(), 3);
        QCOMPARE(rb.at(0), 3);
        QCOMPARE(rb.at(2), 5);
        QCOMPARE(rb.last(), 5);
        rb.setCapacity(2); // shrink keeps the newest
        QCOMPARE(rb.size(), 2);
        QCOMPARE(rb.at(0), 4);
        rb.setCapacity(4); // grow keeps all
        rb.push(6);
        QCOMPARE(rb.size(), 3);
        QCOMPARE(rb.at(0), 4);
        QCOMPARE(rb.last(), 6);
        rb.clear();
        QVERIFY(rb.isEmpty());
        RingBuffer<int> zero(0);
        zero.push(1); // no-op, no crash
        QCOMPARE(zero.size(), 0);
    }

    void nonOkValuesAreNeverFormattedAsNumbers()
    {
        for (auto v : { MetricValue::unsupported(QStringLiteral("x")), MetricValue::unavailable(QStringLiteral("x")),
                        MetricValue::unknown(QStringLiteral("x")) }) {
            for (Unit u : { Unit::Percent, Unit::Bytes, Unit::Celsius, Unit::Watts, Unit::MHz, Unit::PState }) {
                const QString text = formatMetric(v, u);
                QVERIFY2(!text.contains(QLatin1Char('0')), qPrintable(text));
                QCOMPARE(text, stateName(v.state));
            }
            QVERIFY(std::isnan(v.valueOrNaN()));
        }
    }

    void realZeroIsShownAsZero()
    {
        QCOMPARE(formatMetric(MetricValue::ok(0), Unit::Percent), QStringLiteral("0 %"));
        QCOMPARE(MetricValue::ok(0).valueOrNaN(), 0.0);
        QCOMPARE(formatMetric(MetricValue::ok(4.26), Unit::Percent), QStringLiteral("4.3 %"));
        QCOMPARE(formatMetric(MetricValue::ok(42.4), Unit::Percent), QStringLiteral("42 %"));
        QCOMPARE(formatMetric(MetricValue::ok(40), Unit::Celsius), QStringLiteral("40 °C"));
        QCOMPARE(formatMetric(MetricValue::ok(8), Unit::PState), QStringLiteral("P8"));
        QCOMPARE(formatMetric(MetricValue::ok(double(355ull << 20)), Unit::Bytes), QStringLiteral("355 MB"));
    }

    void utilizationNeedsTwoSamples()
    {
        const MetricValue first = utilizationFromRunningTime(std::nullopt, 1000, 0);
        QCOMPARE(first.state, MetricValue::State::Unknown);
    }

    void utilizationIsBusyShareOfElapsedTime()
    {
        // 9 411 000 x 100 ns busy in 10 000 000 x 100 ns elapsed = 94.11 % (the Phase 0 Intel load measurement).
        const MetricValue v = utilizationFromRunningTime(quint64(1'000'000), quint64(10'411'000), 10'000'000);
        QVERIFY(v.isOk());
        QVERIFY(qFuzzyCompare(v.value, 94.11));
        QCOMPARE(utilizationFromRunningTime(quint64(5), 5, 10'000'000).value, 0.0); // idle is a real 0
        QCOMPARE(utilizationFromRunningTime(quint64(0), 20'000'000, 10'000'000).value, 100.0); // clamped
    }

    void counterResetIsUnknownNotNegative()
    {
        const MetricValue v = utilizationFromRunningTime(quint64(5'000'000), 1'000, 10'000'000);
        QCOMPARE(v.state, MetricValue::State::Unknown);
        QVERIFY(v.note.contains(QStringLiteral("restarted")));
        QCOMPARE(utilizationFromRunningTime(quint64(1), 2, 0).state, MetricValue::State::Unknown);
    }

    void busiestEngineMatchesTaskManager()
    {
        const MetricValue v = busiestEngine({ MetricValue::ok(3), MetricValue::unknown({}), MetricValue::ok(42), MetricValue::ok(0) });
        QVERIFY(v.isOk());
        QCOMPARE(v.value, 42.0);
        QCOMPARE(busiestEngine({ MetricValue::ok(0), MetricValue::ok(0) }).value, 0.0);
        QCOMPARE(busiestEngine({ MetricValue::unknown(QStringLiteral("a")), MetricValue::unavailable(QStringLiteral("b")) }).state,
                 MetricValue::State::Unknown);
        QCOMPARE(busiestEngine({}).state, MetricValue::State::Unavailable);
    }

    void segmentsSplitIntoDedicatedAndShared()
    {
        // NVIDIA layout seen in Phase 0: aperture 0, local 355 MB, aperture 0, aperture 22 MB.
        const MemoryUsage m = splitSegments({ { true, 0 }, { false, 355ull << 20 }, { true, 0 }, { true, 22ull << 20 } });
        QCOMPARE(m.dedicated, 355ull << 20);
        QCOMPARE(m.shared, 22ull << 20);
    }

    void engineNames()
    {
        QCOMPARE(engineTypeName(1, 0), QStringLiteral("3D"));
        QCOMPARE(engineTypeName(2, 3), QStringLiteral("Video Decode"));
        QCOMPARE(engineTypeName(6, 6), QStringLiteral("Copy"));
        QCOMPARE(engineTypeName(0, 4), QStringLiteral("Engine 4"));
        QCOMPARE(engineTypeName(99, 5), QStringLiteral("Engine 5"));
        QCOMPARE(disambiguate({ QStringLiteral("3D"), QStringLiteral("Video Decode"), QStringLiteral("Video Decode"), QStringLiteral("Copy") }),
                 (QStringList{ QStringLiteral("3D"), QStringLiteral("Video Decode 1"), QStringLiteral("Video Decode 2"), QStringLiteral("Copy") }));
    }

    void staleness()
    {
        QVERIFY(!isStale(0, 2500, 1000));  // under the 3 s floor
        QVERIFY(isStale(0, 3001, 1000));
        QVERIFY(!isStale(0, 25'000, 10'000)); // 3 x interval for slow intervals
        QVERIFY(isStale(0, 30'001, 10'000));
    }

    void historyCapacityIsBounded()
    {
        QCOMPARE(historyCapacity(300, 1000), 301);
        QCOMPARE(historyCapacity(60, 250), 241);
        QCOMPARE(historyCapacity(3600, 250), 14401); // the largest allowed by settings
        QCOMPARE(historyCapacity(3600, 100), 14401); // capped
        QCOMPARE(historyCapacity(10, 0), 2);
    }

    void historyStoresGapsAsNaNAndDropsVanishedAdapters()
    {
        History h;
        h.setCapacity(3);
        auto snap = [](qint64 t, MetricValue util, quint64 luid) {
            Snapshot s;
            s.steadyMs = t;
            AdapterSample a;
            a.luid = luid;
            a.utilization = util;
            a.dedicatedUsed = MetricValue::ok(100);
            a.sharedUsed = MetricValue::ok(5);
            a.temperature = MetricValue::unsupported(QStringLiteral("no sensor"));
            s.adapters << a;
            return s;
        };
        h.append(snap(0, MetricValue::unknown({}), 1));
        h.append(snap(1000, MetricValue::ok(0), 1));
        h.append(snap(2000, MetricValue::ok(50), 1));
        h.append(snap(3000, MetricValue::unavailable({}), 1));
        const auto* util = h.series(1, Series::Utilization);
        QVERIFY(util);
        QCOMPARE(util->size(), 3);
        QCOMPARE(util->at(0).value, 0.0f);         // a real zero stays zero
        QCOMPARE(util->at(1).value, 50.0f);
        QVERIFY(std::isnan(util->at(2).value));     // unavailable is a gap, not zero
        const auto* temp = h.series(1, Series::Temperature);
        for (int i = 0; i < temp->size(); ++i)
            QVERIFY(std::isnan(temp->at(i).value)); // unsupported sensor never graphs as 0 °C

        h.append(snap(4000, MetricValue::ok(1), 2)); // adapter 1 vanished (e.g. new LUID after reset)
        QVERIFY(!h.series(1, Series::Utilization));
        QVERIFY(h.series(2, Series::Utilization));
    }

    void targetsFlagSoftwareAndSkipDeviceManagerOnly()
    {
        hw::DetectionResult r;
        hw::Adapter gpu;
        gpu.dxgi = hw::DxgiAdapter{};
        gpu.dxgi->luid = 7;
        gpu.dxgi->vendorId = 0x10DE;
        gpu.dxgi->dedicatedVideoMemory = 4ull << 30;
        gpu.classification.value = hw::AdapterClass::Discrete;
        hw::Adapter warp;
        warp.dxgi = hw::DxgiAdapter{};
        warp.dxgi->luid = 9;
        warp.dxgi->softwareFlag = true;
        warp.classification.value = hw::AdapterClass::Software;
        hw::Adapter pnpOnly;
        pnpOnly.pnp = hw::PnpDevice{};
        r.adapters = { gpu, warp, pnpOnly };
        const auto targets = targetsFrom(r);
        QCOMPARE(targets.size(), 2); // the Device-Manager-only entry has nothing to monitor
        QCOMPARE(targets[0].luid, 7ull);
        QVERIFY(!targets[0].software);
        QCOMPARE(targets[0].dedicatedTotal, 4ull << 30);
        QCOMPARE(targets[1].luid, 9ull);
        QVERIFY(targets[1].software); // kept only for per-process software-rendering detection
    }
};

QTEST_GUILESS_MAIN(TestMetricModel)
#include "tst_metricmodel.moc"
