// Live multi-GPU workload tests: the hash image split between GPUs (and the CPU) must equal the CPU
// reference bit for bit in every mode; the generated H.264 test clip must decode on each GPU with
// the frames delivered as textures on that GPU, identical to the software decoder's frames, also
// when two decoders run at the same time.
#include "benchmarks/LabModel.h"
#include "benchmarks/LabRunner.h"
#include "hardware/AdapterDetector.h"
#include "workloads/SplitHash.h"
#include "workloads/VideoDecode.h"

#include <QDir>
#include <QJsonArray>
#include <QTest>

#include <thread>

using namespace gf;

class TestLabLive : public QObject {
    Q_OBJECT

    hw::DetectionResult m_detection;
    QList<const hw::Adapter*> m_gpus;
    QString m_clip;
    std::unique_ptr<work::ComScope> m_com;

    QString clipDir() const { return QDir(QDir::tempPath()).filePath(QStringLiteral("gpufusion-test-clips")); } // reused between runs

private Q_SLOTS:
    void initTestCase()
    {
        m_com = std::make_unique<work::ComScope>();
        m_detection = hw::detectAdapters(nullptr);
        for (const hw::Adapter& a : m_detection.adapters) {
            if (a.dxgi && !a.isSoftware() && a.dxgi->d3d11.device.ok())
                m_gpus << &a;
        }
        if (m_gpus.isEmpty())
            QSKIP("No physical GPU with Direct3D 11 on this machine");
    }

    void splitImageIsBitExactInEveryMode()
    {
        work::HashParams p;
        p.width = 512;
        p.height = 300; // not a multiple of the chunk or alignment: the remainder must be handled
        p.iterations = 64;
        p.bandRows = 64;
        const auto reference = work::hashRowsCpu(p, 0, p.height, 0);
        std::vector<std::unique_ptr<work::D3D11HashDevice>> devices;
        std::vector<work::SplitWorker> workers;
        for (const hw::Adapter* a : m_gpus) {
            QString error;
            devices.push_back(work::D3D11HashDevice::create(a->dxgi->luid, &error));
            QVERIFY2(devices.back(), qPrintable(error));
            workers.push_back({ a->name(), devices.back().get(), 0 });
        }
        std::vector<work::SplitWorker> withCpu = workers;
        withCpu.push_back({ QStringLiteral("CPU"), nullptr, 2 });

        struct Case { const char* name; std::vector<work::SplitWorker> workers; work::SplitSpec spec; };
        std::vector<double> equal(workers.size(), 1.0);
        std::vector<double> equal3(withCpu.size(), 1.0);
        std::vector<double> lopsided(workers.size(), 0.0);
        lopsided[0] = 0.9;
        if (lopsided.size() > 1)
            lopsided[1] = 0.1;
        const std::vector<Case> cases{
            { "static equal", workers, { work::SplitSpec::Mode::Static, equal, 16, 32 } },
            { "static lopsided", workers, { work::SplitSpec::Mode::Static, lopsided, 16, 32 } },
            { "dynamic 32", workers, { work::SplitSpec::Mode::Dynamic, {}, 16, 32 } },
            { "dynamic 7 with CPU", withCpu, { work::SplitSpec::Mode::Dynamic, {}, 16, 7 } },
            { "static with CPU", withCpu, { work::SplitSpec::Mode::Static, equal3, 16, 32 } },
        };
        for (const Case& c : cases) {
            const work::SplitResult r = work::runSplit(p, c.workers, c.spec);
            QVERIFY2(r.ok, qPrintable(QStringLiteral("%1: %2").arg(QLatin1StringView(c.name), r.error)));
            QCOMPARE(work::firstMismatch(r.image, reference), -1LL);
            std::uint32_t rows = 0;
            QStringList parts;
            for (std::size_t w = 0; w < r.workers.size(); ++w) {
                rows += r.workers[w].rows;
                parts << QStringLiteral("%1 %2 rows/%3 items").arg(c.workers[w].label).arg(r.workers[w].rows).arg(r.workers[w].items);
            }
            QCOMPARE(rows, p.height);
            qInfo().noquote() << c.name << QString::number(r.wallMs, 'f', 2) << "ms:" << parts.join(QStringLiteral(", "));
        }
    }

    void testClipIsCreated()
    {
        QString error;
        QVERIFY2(work::startMediaFoundation(&error), qPrintable(error));
        const auto t = std::chrono::steady_clock::now();
        m_clip = work::ensureTestClip(clipDir(), work::TestClipSpec{}, &error);
        QVERIFY2(!m_clip.isEmpty(), qPrintable(error));
        QVERIFY(QFileInfo(m_clip).size() > 100'000);
        qInfo().noquote() << "clip" << QDir::toNativeSeparators(m_clip) << QFileInfo(m_clip).size() / 1024 << "KB, ready in"
                          << std::chrono::duration<double>(std::chrono::steady_clock::now() - t).count() << "s";
    }

    void eachGpuDecodesInHardwareExactlyLikeSoftware()
    {
        QVERIFY(!m_clip.isEmpty());
        QString error;
        auto software = work::VideoDecoder::open(m_clip, std::nullopt, &error);
        QVERIFY2(software, qPrintable(error));
        const auto ref = software->decodeAll(true);
        QVERIFY2(ref.ok, qPrintable(ref.error));
        QCOMPARE(ref.frames, 300);
        QVERIFY(!ref.gpuOutput);
        const auto timedSw = software->decodeAll(false);
        qInfo().noquote() << "software:" << timedSw.frames << "frames in" << timedSw.wallMs << "ms";
        for (const hw::Adapter* a : m_gpus) {
            auto decoder = work::VideoDecoder::open(m_clip, a->dxgi->luid, &error);
            QVERIFY2(decoder, qPrintable(a->name() + QStringLiteral(": ") + error));
            const auto r = decoder->decodeAll(true);
            QVERIFY2(r.ok, qPrintable(a->name() + QStringLiteral(": ") + r.error));
            QCOMPARE(r.frames, ref.frames);
            int mismatches = 0;
            for (std::size_t i = 0; i < r.frameHashes.size(); ++i)
                mismatches += r.frameHashes[i] != ref.frameHashes[i];
            const auto timed = decoder->decodeAll(false);
            qInfo().noquote() << a->name() << "GPU output:" << r.gpuOutput << "frames differing from software:" << mismatches << "timed:" << timed.frames
                              << "frames in" << timed.wallMs << "ms";
            QVERIFY2(r.gpuOutput, qPrintable(a->name() + QStringLiteral(": frames were not decoded on this GPU")));
            QCOMPARE(mismatches, 0);
        }
    }

    void twoDecodersAtOnce()
    {
        QVERIFY(!m_clip.isEmpty());
        QString error;
        std::vector<std::unique_ptr<work::VideoDecoder>> decoders;
        for (const hw::Adapter* a : m_gpus) {
            decoders.push_back(work::VideoDecoder::open(m_clip, a->dxgi->luid, &error));
            QVERIFY2(decoders.back(), qPrintable(error));
        }
        if (decoders.size() < 2)
            decoders.push_back(work::VideoDecoder::open(m_clip, m_gpus.first()->dxgi->luid, &error));
        std::vector<work::VideoDecoder::Result> results(decoders.size());
        std::vector<std::thread> threads;
        const auto t = std::chrono::steady_clock::now();
        for (std::size_t i = 0; i < decoders.size(); ++i)
            threads.emplace_back([&, i] {
                work::ComScope com;
                results[i] = decoders[i]->decodeAll(false);
            });
        for (auto& th : threads)
            th.join();
        const double wall = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t).count();
        for (std::size_t i = 0; i < results.size(); ++i) {
            QVERIFY2(results[i].ok, qPrintable(results[i].error));
            QCOMPARE(results[i].frames, 300);
            qInfo().noquote() << decoders[i]->adapterName() << results[i].wallMs << "ms";
        }
        qInfo().noquote() << "both finished in" << wall << "ms";
    }

    void experimentsRunEndToEnd()
    {
        bench::LabOptions o = bench::labOptions(0, 2, QStringLiteral("quick"), QDir::tempPath());
        o.mediaDir = clipDir(); // reuse the clip made above
        for (const bool split : { true, false }) {
            int lastDone = -1, lastTotal = 0;
            const bench::Session s = split ? bench::runSplitExperiment(m_detection, o, [&](int d, int t, const QString&) { lastDone = d; lastTotal = t; }, nullptr)
                                           : bench::runDecodeExperiment(m_detection, o, [&](int d, int t, const QString&) { lastDone = d; lastTotal = t; }, nullptr);
            QVERIFY(!s.results.isEmpty());
            for (const bench::BenchmarkResult& r : s.results) {
                QVERIFY2(r.status == bench::Status::Completed, qPrintable(r.name + QStringLiteral(": ") + r.error));
                QVERIFY2(r.correct.value_or(false), qPrintable(r.name));
                QCOMPARE(r.duration.n, 2);
                QVERIFY(r.warmupRuns >= 1); // at least one second of warm-up even with 0 configured
                // Engine evidence is recorded for every mode. A CPU-only mode may legitimately leave every GPU
                // engine below 1 % (whether it does depends on unrelated desktop activity), so either form counts there.
                const bool cpuOnly = r.name.contains(QStringLiteral("Software (CPU)")) || r.name.startsWith(QStringLiteral("CPU"));
                QVERIFY2(std::any_of(r.notes.begin(), r.notes.end(), [cpuOnly](const QString& n) {
                             return n.contains(QStringLiteral("busiest engines")) || (cpuOnly && n.contains(QStringLiteral("no engine busier")));
                         }),
                         qPrintable(r.name + QStringLiteral(": ") + r.notes.join(QStringLiteral(" | "))));
            }
            QCOMPARE(lastDone, lastTotal); // progress reaches the end
            const bench::LabAnalysis a = split ? bench::analyzeSplit(s) : bench::analyzeDecode(s);
            QVERIFY(a.reference);
            QVERIFY(!a.headline.isEmpty());
            qInfo().noquote() << a.headline;
        }
    }

    // The adaptive scheduler on the real GPUs: every mode's image bit-exact, every row exactly once,
    // the fastest GPU is the primary, the helper either did rows or was switched off by measurement
    // (both are valid outcomes — never assumed faster), and the fallback re-ran the helper's rows.
    void adaptiveSchedulerEndToEnd()
    {
        if (m_gpus.size() < 2)
            QSKIP("Needs two GPUs");
        bench::LabOptions o = bench::labOptions(0, 3, QStringLiteral("quick"), QDir::tempPath());
        o.policies = { QStringLiteral("conservative"), QStringLiteral("performance") };
        const bench::Session s = bench::runAdaptiveExperiment(m_detection, o, {}, nullptr);
        QCOMPARE(bench::labKind(s), QStringLiteral("adaptive"));
        QCOMPARE(s.results.size(), qsizetype(m_gpus.size()) + 2 + 1);
        double singleGt = 0;
        for (const bench::BenchmarkResult& r : s.results) {
            QVERIFY2(r.status == bench::Status::Completed, qPrintable(r.name + u' ' + r.error));
            QVERIFY2(r.correct.value_or(false), qPrintable(r.name));
            if (r.testId == QLatin1StringView("lab.split.single") && r.target.vendorId == 0x10DE)
                singleGt = r.duration.mean;
            if (!r.testId.startsWith(QLatin1StringView("lab.adaptive.")))
                continue;
            const QJsonArray workers = r.params.value(QLatin1StringView("workers")).toArray();
            QCOMPARE(workers.size(), 2);
            if (qEnvironmentVariableIntValue("GPUFUSION_EXPECT_REFERENCE_MACHINE") == 1)
                QVERIFY2(workers[0].toString().contains(QStringLiteral("NVIDIA")), qPrintable(workers[0].toString())); // NVIDIA first
            if (r.testId == QLatin1StringView("lab.adaptive.fallback")) {
                QVERIFY(r.metrics.value(QStringLiteral("w1.rerunRows")).mean > 0);
                QCOMPARE(r.metrics.value(QStringLiteral("w1.rowsPercent")).mean, 0.0);
                continue;
            }
            // Rows add up to the whole image in every run.
            for (const bench::RunSample& sample : r.samples)
                QVERIFY(qAbs(sample.metrics.value(QStringLiteral("w0.rowsPercent")) + sample.metrics.value(QStringLiteral("w1.rowsPercent")) - 100.0) < 1e-6);
            const bool helped = r.metrics.value(QStringLiteral("w1.rowsPercent")).max > 0;
            const bool switchedOff = r.params.value(QLatin1StringView("helperEnabledAtEnd")).toBool(true) == false;
            QVERIFY2(helped || switchedOff, qPrintable(r.name));
            QVERIFY(r.metrics.value(QStringLiteral("w1.rowsPercent")).max <= 50.0 + 1e-9); // the primary always does most
            QVERIFY(r.cpuPercent.isOk());
            QVERIFY(r.peakWorkingSetMB.isOk());
        }
        const bench::LabAnalysis a = bench::analyzeAdaptive(s);
        QVERIFY(a.reference);
        QVERIFY(a.verdict.has_value());
        qInfo().noquote() << a.headline << "(GT 730 alone:" << singleGt << "ms)";
        for (const QString& f : a.findings)
            qInfo().noquote() << "  " << f;
    }

    void experimentCancels()
    {
        std::atomic<bool> cancel{ true };
        const bench::Session s = bench::runSplitExperiment(m_detection, bench::labOptions(1, 2, QStringLiteral("quick"), QDir::tempPath()), {}, &cancel);
        for (const bench::BenchmarkResult& r : s.results)
            QCOMPARE(r.status, bench::Status::Cancelled);
    }
};

QTEST_GUILESS_MAIN(TestLabLive)
#include "tst_lab_live.moc"
