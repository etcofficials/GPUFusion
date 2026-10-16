// Live workload and benchmark tests on the real GPUs: the pixel-shader hash must match the CPU
// bit for bit (whole image and a band in the middle, which is how Phase 8 splits work), transfers
// must return what was sent, and a short benchmark session must complete, be correct and be saved.
#include "benchmarks/BenchmarkRunner.h"
#include "benchmarks/BenchmarkService.h"
#include "hardware/AdapterDetector.h"
#include "workloads/D3D11HashDevice.h"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace gf;

class TestWorkloadLive : public QObject {
    Q_OBJECT

    hw::DetectionResult m_detection;
    QList<const hw::Adapter*> m_gpus;

private Q_SLOTS:
    void initTestCase()
    {
        m_detection = hw::detectAdapters(nullptr);
        for (const hw::Adapter& a : m_detection.adapters) {
            if (a.dxgi && !a.isSoftware() && a.dxgi->d3d11.device.ok())
                m_gpus << &a;
        }
        if (m_gpus.isEmpty())
            QSKIP("No physical GPU with Direct3D 11 on this machine");
    }

    void gpuHashIsBitExactOnEveryGpu()
    {
        work::HashParams p;
        p.width = 256;
        p.height = 256;
        p.iterations = 64;
        p.bandRows = 32;
        const auto reference = work::hashRowsCpu(p, 0, p.height, 0);
        for (const hw::Adapter* a : m_gpus) {
            QString error;
            auto device = work::D3D11HashDevice::create(a->dxgi->luid, &error);
            QVERIFY2(device, qPrintable(error));
            const auto whole = device->run(p, 0, p.height, true);
            QVERIFY2(whole.ok, qPrintable(whole.error));
            QCOMPARE(work::firstMismatch(whole.output, reference), -1LL);
            // A band from the middle — the unit Phase 8 distributes between GPUs.
            const auto band = device->run(p, 100, 37, true);
            QVERIFY2(band.ok, qPrintable(band.error));
            QVERIFY(std::equal(band.output.begin(), band.output.end(), reference.begin() + 100 * p.width));
            qInfo().noquote() << a->name() << "FL" << device->featureLevel() << "256x256x64:" << whole.wallMs << "ms wall,"
                              << (whole.gpuMs ? QString::number(*whole.gpuMs) : QStringLiteral("no")) << "ms GPU timestamps";
        }
    }

    void transferReturnsTheSameData()
    {
        for (const hw::Adapter* a : m_gpus) {
            QString error;
            auto device = work::D3D11HashDevice::create(a->dxgi->luid, &error);
            QVERIFY(device);
            const auto t = device->transfer(4ull << 20);
            QVERIFY2(t.ok, qPrintable(t.error));
            QVERIFY(t.dataIntact);
            QVERIFY(t.uploadMs > 0 && t.readbackMs > 0);
            qInfo().noquote() << a->name() << "4 MB: upload" << t.uploadMs << "ms, read-back" << t.readbackMs << "ms";
        }
    }

    void shortSessionCompletesCorrectlyAndIsSaved()
    {
        bench::RunOptions options;
        options.warmupRuns = 1;
        options.measuredRuns = 3;
        options.hash = bench::hashPreset(QStringLiteral("quick"));
        options.transferBytes = 8ull << 20;
        QList<bench::BenchmarkSpec> specs;
        for (const bench::BenchmarkSpec& s : bench::availableBenchmarks(m_detection, {})) {
            if (s.id != QLatin1StringView("cpu.hash.1t") && s.id != QLatin1StringView("detect.full")) // keep the test short
                specs << s;
        }
        QTemporaryDir dir;
        bench::BenchmarkService service(dir.path());
        QSignalSpy done(&service, &bench::BenchmarkService::finished);
        service.start(specs, options, m_detection, 90);
        QVERIFY(done.wait(180000));
        const bench::Session session = done.first().first().value<bench::Session>();
        QCOMPARE(session.results.size(), specs.size());
        for (const bench::BenchmarkResult& r : session.results) {
            QVERIFY2(r.status == bench::Status::Completed, qPrintable(r.name + QStringLiteral(": ") + r.error));
            QCOMPARE(r.duration.n, 3);
            QVERIFY(r.duration.mean > 0);
            if (r.correct.has_value())
                QVERIFY2(*r.correct, qPrintable(r.name));
            qInfo().noquote() << QStringLiteral("%1 [%2]: %3 ms ± %4").arg(r.name, r.api).arg(r.duration.mean, 0, 'f', 3).arg(r.duration.stddev, 0, 'f', 3);
        }
        QCOMPARE(service.lastSaveError(), QString());
        QVERIFY(QFile::exists(QDir(dir.path()).filePath(QStringLiteral("sessions/%1.json").arg(session.id))));
        QFile csv(QDir(dir.path()).filePath(QStringLiteral("results.csv")));
        QVERIFY(csv.open(QIODevice::ReadOnly));
        QCOMPARE(csv.readAll().count('\n'), qsizetype(1 + specs.size())); // header + one row per result
        QCOMPARE(service.history().size(), 1);
    }

    void cancellationStopsBetweenRuns()
    {
        bench::RunOptions options;
        options.warmupRuns = 0;
        options.measuredRuns = 2;
        options.hash = bench::hashPreset(QStringLiteral("quick"));
        std::atomic<bool> cancel{ true };
        const auto specs = bench::availableBenchmarks(m_detection, {});
        const bench::Session s = bench::runBenchmarks(specs, options, m_detection, {}, &cancel);
        for (const bench::BenchmarkResult& r : s.results)
            QCOMPARE(r.status, bench::Status::Cancelled);
    }
};

QTEST_GUILESS_MAIN(TestWorkloadLive)
#include "tst_workload_live.moc"
