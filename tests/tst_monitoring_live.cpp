// Hardware-dependent monitoring tests. The central check: put real work on each physical GPU and
// verify the monitor sees that GPU get busy — this validates readings on adapters Task Manager
// cannot show (e.g. WDDM 1.x drivers), instead of trusting an API because it returned a number.
#include "core/Settings.h"
#include "hardware/AdapterDetector.h"
#include "hardware/HardwareService.h"
#include "monitoring/MetricModel.h"
#include "monitoring/MonitorService.h"
#include "platform/windows/KmtStatsProvider.h"
#include "platform/windows/NvmlProvider.h"

#include "GpuLoad.h"

#include <QElapsedTimer>
#include <QSignalSpy>
#include <QTest>
#include <QThread>

#include <atomic>
#include <thread>

#include <d3d11.h>
#include <dxgi.h>
#include <pdh.h>
#include <pdhmsg.h>
#include <wrl/client.h>

#include <QHash>
#include <vector>

using Microsoft::WRL::ComPtr;
using namespace gf;
using namespace gf::mon;

namespace {

QList<AdapterSample> blankSamples(const QList<MonitorTarget>& targets)
{
    QList<AdapterSample> out;
    for (const MonitorTarget& t : targets) {
        AdapterSample s;
        s.luid = t.luid;
        s.name = t.name;
        out << s;
    }
    return out;
}

QList<std::pair<QString, double>> counterArray(PDH_HCOUNTER counter)
{
    QList<std::pair<QString, double>> out;
    DWORD bytes = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, nullptr) != PDH_MORE_DATA)
        return out;
    std::vector<BYTE> buffer(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    if (PdhGetFormattedCounterArrayW(counter, PDH_FMT_DOUBLE, &bytes, &count, items) != ERROR_SUCCESS)
        return out;
    for (DWORD i = 0; i < count; ++i) {
        if (items[i].FmtValue.CStatus == PDH_CSTATUS_VALID_DATA || items[i].FmtValue.CStatus == PDH_CSTATUS_NEW_DATA)
            out.append({ QString::fromWCharArray(items[i].szName), items[i].FmtValue.doubleValue });
    }
    return out;
}

const AdapterSample* find(const QList<AdapterSample>& samples, quint64 luid)
{
    for (const AdapterSample& s : samples) {
        if (s.luid == luid)
            return &s;
    }
    return nullptr;
}

} // namespace

class TestMonitoringLive : public QObject {
    Q_OBJECT

    QList<MonitorTarget> m_targets;

private Q_SLOTS:
    void initTestCase()
    {
        for (const MonitorTarget& t : targetsFrom(hw::detectAdapters(nullptr))) {
            if (!t.software)
                m_targets << t;
        }
        if (m_targets.isEmpty())
            QSKIP("No physical GPU is available to DirectX on this machine");
    }

    void kmtNeedsTwoSamplesThenReportsEveryAdapter()
    {
        win::KmtStatsProvider kmt;
        kmt.setTargets(m_targets);
        QList<AdapterSample> first = blankSamples(m_targets);
        QVERIFY(kmt.sample(first).isEmpty());
        for (const AdapterSample& s : first) {
            QCOMPARE(s.utilization.state, MetricValue::State::Unknown);
            QVERIFY2(!s.engines.isEmpty(), qPrintable(s.name));
            QVERIFY(s.dedicatedUsed.isOk());
        }
        QThread::msleep(500);
        QList<AdapterSample> second = blankSamples(m_targets);
        QVERIFY(kmt.sample(second).isEmpty());
        for (const AdapterSample& s : second) {
            QVERIFY2(s.utilization.isOk(), qPrintable(s.name + QStringLiteral(": ") + s.utilization.note));
            QVERIFY(s.utilization.value >= 0 && s.utilization.value <= 100);
            for (const EngineSample& e : s.engines)
                QVERIFY(e.utilization.isOk());
            qInfo().noquote() << s.name << "idle-ish utilization" << formatMetric(s.utilization, Unit::Percent)
                              << "dedicated" << formatMetric(s.dedicatedUsed, Unit::Bytes) << "shared" << formatMetric(s.sharedUsed, Unit::Bytes)
                              << "engines" << s.engines.size();
        }
    }

    void loadOnEachGpuIsSeenOnThatGpu()
    {
        for (const MonitorTarget& t : m_targets) {
            win::KmtStatsProvider kmt;
            kmt.setTargets(m_targets);
            GpuLoad load(t.luid);
            if (!load.ok())
                QSKIP(qPrintable(QStringLiteral("Could not create a Direct3D 11 load on %1").arg(t.name)));
            QThread::msleep(300);
            QList<AdapterSample> a = blankSamples(m_targets);
            kmt.sample(a);
            QThread::msleep(1000);
            QList<AdapterSample> b = blankSamples(m_targets);
            kmt.sample(b);
            const AdapterSample* s = find(b, t.luid);
            QVERIFY(s);
            qInfo().noquote() << t.name << "under load:" << formatMetric(s->utilization, Unit::Percent);
            QVERIFY2(s->utilization.isOk() && s->utilization.value >= 30.0,
                     qPrintable(QStringLiteral("%1 reported %2 under a full clear loop").arg(t.name, formatMetric(s->utilization, Unit::Percent))));
            // Attribution: the load must not be reported on the other adapter(s).
            for (const AdapterSample& other : b) {
                if (other.luid == t.luid)
                    continue;
                qInfo().noquote() << "  meanwhile" << other.name << formatMetric(other.utilization, Unit::Percent);
                QVERIFY2(other.utilization.isOk() && other.utilization.value < 50.0,
                         qPrintable(QStringLiteral("load on %1 also appeared on %2").arg(t.name, other.name)));
            }
        }
    }

    // Cross-check against Windows' own GPU performance counters (Task Manager's source), which
    // exist only for WDDM 2.0+ drivers: same engine-busy window, same memory moment.
    void agreesWithWindowsPerformanceCounters()
    {
        for (const MonitorTarget& t : m_targets) {
            const QString luidTag = QStringLiteral("luid_0x%1_0x%2").arg(quint32(t.luid >> 32), 8, 16, QLatin1Char('0'))
                                                                     .arg(quint32(t.luid), 8, 16, QLatin1Char('0'));
            PDH_HQUERY query = nullptr;
            QVERIFY(PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS);
            PDH_HCOUNTER engines = nullptr, dedicated = nullptr;
            PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &engines);
            PdhAddEnglishCounterW(query, L"\\GPU Adapter Memory(*)\\Dedicated Usage", 0, &dedicated);

            win::KmtStatsProvider kmt;
            kmt.setTargets(m_targets);
            GpuLoad load(t.luid);
            QThread::msleep(300);
            QList<AdapterSample> a = blankSamples(m_targets);
            kmt.sample(a);
            PdhCollectQueryData(query);
            QThread::msleep(1500);
            QList<AdapterSample> b = blankSamples(m_targets);
            kmt.sample(b);
            PdhCollectQueryData(query);

            // PDH engine instances are per process: sum per engine for this LUID, then take the busiest.
            QHash<QString, double> perEngine;
            int instances = 0;
            for (const auto& [name, value] : counterArray(engines)) {
                if (!name.contains(luidTag, Qt::CaseInsensitive))
                    continue;
                ++instances;
                perEngine[name.section(QStringLiteral("_eng_"), 1)] += value;
            }
            double pdhDedicated = -1;
            for (const auto& [name, value] : counterArray(dedicated)) {
                if (name.contains(luidTag, Qt::CaseInsensitive))
                    pdhDedicated = value;
            }
            PdhCloseQuery(query);

            const AdapterSample* s = find(b, t.luid);
            if (instances == 0) {
                qInfo().noquote() << t.name << ": no Windows GPU performance counter instances (driver below WDDM 2.0);"
                                  << "GPU Fusion reads" << formatMetric(s->utilization, Unit::Percent) << "via D3DKMT";
                continue;
            }
            double pdhBusiest = 0;
            for (double v : perEngine)
                pdhBusiest = std::max(pdhBusiest, v);
            qInfo().noquote() << t.name << ": utilization D3DKMT" << formatMetric(s->utilization, Unit::Percent)
                              << "vs PDH" << QString::number(pdhBusiest, 'f', 1) << "%; dedicated memory D3DKMT"
                              << formatMetric(s->dedicatedUsed, Unit::Bytes) << "vs PDH" << hw::formatBytes(quint64(std::max(0.0, pdhDedicated)));
            QVERIFY(std::abs(s->utilization.value - std::min(pdhBusiest, 100.0)) < 15.0);
            if (pdhDedicated >= 0)
                QVERIFY(std::abs(s->dedicatedUsed.value - pdhDedicated) < double(96ull << 20)); // allocations move during the load
        }
    }

    void referenceMachineCounterCoverage()
    {
        if (qEnvironmentVariableIntValue("GPUFUSION_EXPECT_REFERENCE_MACHINE") != 1)
            QSKIP("Set GPUFUSION_EXPECT_REFERENCE_MACHINE=1 to check the development PC");
        // The Intel HD 2000 (WDDM 1.2) must have no PDH GPU Engine instances; that is why GPU Fusion uses D3DKMT.
        PDH_HQUERY query = nullptr;
        QVERIFY(PdhOpenQueryW(nullptr, 0, &query) == ERROR_SUCCESS);
        PDH_HCOUNTER engines = nullptr;
        PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &engines);
        PdhCollectQueryData(query);
        QThread::msleep(200);
        PdhCollectQueryData(query);
        int intel = 0, nvidia = 0;
        for (const MonitorTarget& t : m_targets) {
            const QString tag = QStringLiteral("luid_0x%1_0x%2").arg(quint32(t.luid >> 32), 8, 16, QLatin1Char('0')).arg(quint32(t.luid), 8, 16, QLatin1Char('0'));
            for (const auto& [name, value] : counterArray(engines)) {
                if (name.contains(tag, Qt::CaseInsensitive))
                    (t.vendorId == 0x8086 ? intel : nvidia)++;
            }
        }
        PdhCloseQuery(query);
        QCOMPARE(intel, 0);
        QVERIFY(nvidia > 0);
    }

    void nvmlSensorsAreStatedNotGuessed()
    {
        win::NvmlProvider nvml;
        const bool hasNvidia = std::any_of(m_targets.begin(), m_targets.end(), [](const MonitorTarget& t) { return t.vendorId == 0x10DE; });
        if (!hasNvidia || !nvml.isActive())
            QSKIP(qPrintable(QStringLiteral("NVML not usable here: %1").arg(nvml.status().detail)));
        nvml.setTargets(m_targets);
        QList<AdapterSample> s = blankSamples(m_targets);
        nvml.sample(s);
        for (const AdapterSample& a : s) {
            if (!a.sources.contains(QStringLiteral("NVML")))
                continue;
            for (const MetricValue* v : { &a.temperature, &a.fanPercent, &a.powerWatts, &a.coreClockMHz, &a.memoryClockMHz, &a.pstate })
                QVERIFY2(v->state != MetricValue::State::Unknown || v->note.contains(QStringLiteral("performance state")), qPrintable(v->note));
            if (a.temperature.isOk())
                QVERIFY(a.temperature.value > 5 && a.temperature.value < 120);
            qInfo().noquote() << a.name << "NVML: temp" << formatMetric(a.temperature, Unit::Celsius) << "fan" << formatMetric(a.fanPercent, Unit::Percent)
                              << "power" << formatMetric(a.powerWatts, Unit::Watts) << "core" << formatMetric(a.coreClockMHz, Unit::MHz)
                              << "mem" << formatMetric(a.memoryClockMHz, Unit::MHz) << "pstate" << formatMetric(a.pstate, Unit::PState);
        }
        qInfo().noquote() << "NVML:" << nvml.status().detail << "unsupported:" << nvml.status().unsupported.join(QStringLiteral(", "));
    }

    void referenceMachineSensorSupport()
    {
        if (qEnvironmentVariableIntValue("GPUFUSION_EXPECT_REFERENCE_MACHINE") != 1)
            QSKIP("Set GPUFUSION_EXPECT_REFERENCE_MACHINE=1 to check the development PC's known sensor support");
        win::NvmlProvider nvml;
        QVERIFY2(nvml.isActive(), qPrintable(nvml.status().detail));
        nvml.setTargets(m_targets);
        QList<AdapterSample> s = blankSamples(m_targets);
        nvml.sample(s);
        const auto gt730 = std::find_if(s.begin(), s.end(), [](const AdapterSample& a) { return a.sources.contains(QStringLiteral("NVML")); });
        QVERIFY(gt730 != s.end());
        // Measured in Phase 0 with nvidia-smi on driver 391.35 (GF108):
        QVERIFY(gt730->temperature.isOk());
        QVERIFY(gt730->fanPercent.isOk());
        QVERIFY(gt730->pstate.isOk());
        QCOMPARE(gt730->powerWatts.state, MetricValue::State::Unsupported);
        QCOMPARE(gt730->coreClockMHz.state, MetricValue::State::Unsupported);
    }

    // The real services end to end: start, pause (no samples while paused), resume, change the
    // interval live, and shut down cleanly with the worker thread running.
    void serviceStartsPausesResumesAndStops()
    {
        Settings settings;
        settings.refreshIntervalMs = 250;
        hw::HardwareService hardware;
        auto monitor = std::make_unique<MonitorService>(hardware, settings);
        QSignalSpy updates(monitor.get(), &MonitorService::updated);
        hardware.refresh(true);
        QTRY_VERIFY_WITH_TIMEOUT(updates.count() >= 4, 10000);
        QVERIFY(monitor->latest().has_value());
        QCOMPARE(monitor->latest()->adapters.size(), m_targets.size());
        QVERIFY(!monitor->isStale());

        monitor->setPaused(true);
        QTest::qWait(400); // let an in-flight sample drain
        const int whilePaused = int(updates.count());
        QTest::qWait(1500);
        QCOMPARE(int(updates.count()), whilePaused); // no samples while paused
        QVERIFY(!monitor->isStale());                // paused is not "stale"

        monitor->setPaused(false);
        QTRY_VERIFY_WITH_TIMEOUT(updates.count() >= whilePaused + 3, 5000);
        // After resuming, utilization restarts from a fresh baseline (Unknown first), never an average over the pause.
        bool sawOk = false;
        for (const AdapterSample& a : monitor->latest()->adapters)
            sawOk = sawOk || a.utilization.isOk();
        QVERIFY(sawOk);

        settings.refreshIntervalMs = 1000;
        monitor->applySettings(settings);
        QCOMPARE(monitor->history().capacity(), historyCapacity(settings.historySeconds, 1000));
        const int before = int(updates.count());
        QTest::qWait(2600);
        const int delta = int(updates.count()) - before;
        QVERIFY2(delta >= 1 && delta <= 4, qPrintable(QString::number(delta))); // ~2-3 samples at 1 s, not ~10 at 250 ms

        QElapsedTimer stop;
        stop.start();
        monitor.reset(); // joins the monitor thread
        QVERIFY(stop.elapsed() < 3000);
    }

    // Per-process sampling runs only while requested (the Applications page is open), is
    // reference-counted, and stops completely when the last requester releases it.
    void processTrackingRunsOnlyWhileRequested()
    {
        Settings settings;
        settings.refreshIntervalMs = 500;
        hw::HardwareService hardware;
        MonitorService monitor(hardware, settings);
        QSignalSpy processes(&monitor, &MonitorService::processesUpdated);
        hardware.refresh(true);
        QTest::qWait(3000);
        QCOMPARE(processes.count(), 0); // nobody asked: no per-process work at all

        monitor.setProcessTracking(true);
        monitor.setProcessTracking(true); // second requester
        QTRY_VERIFY_WITH_TIMEOUT(processes.count() >= 3, 10000);
        QVERIFY(monitor.latestProcesses()->ratesReady);
        QVERIFY(monitor.latestProcesses()->processCount > 10);

        monitor.setProcessTracking(false); // one requester left: still running
        const int afterFirstRelease = int(processes.count());
        QTRY_VERIFY_WITH_TIMEOUT(processes.count() > afterFirstRelease, 5000);

        monitor.setProcessTracking(false); // last requester gone
        QTest::qWait(300);
        const int stopped = int(processes.count());
        QTest::qWait(3000);
        QCOMPARE(int(processes.count()), stopped);
    }

    void collectionIsCheap()
    {
        win::KmtStatsProvider kmt;
        kmt.setTargets(m_targets);
        QList<AdapterSample> warm = blankSamples(m_targets);
        kmt.sample(warm);
        QElapsedTimer t;
        t.start();
        constexpr int kRuns = 50;
        for (int i = 0; i < kRuns; ++i) {
            QList<AdapterSample> s = blankSamples(m_targets);
            kmt.sample(s);
        }
        const double perSampleMs = double(t.nsecsElapsed()) / 1e6 / kRuns;
        qInfo() << "D3DKMT collection for" << m_targets.size() << "adapter(s):" << perSampleMs << "ms per sample";
        QVERIFY(perSampleMs < 20.0);
    }
};

QTEST_GUILESS_MAIN(TestMonitoringLive)
#include "tst_monitoring_live.moc"
