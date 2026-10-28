// Pure tests for the Workload Lab: dividing rows between workers, the balanced split and its ideal
// time, and the interpretation of sessions (verdicts, findings, what is excluded).
#include "benchmarks/LabModel.h"
#include "workloads/SplitPlan.h"

#include <QJsonArray>
#include <QTest>

using namespace gf;
using namespace gf::bench;

namespace {

BenchmarkResult result(const QString& id, const QString& name, QList<double> ms, const QStringList& workers = {}, quint32 vendor = 0)
{
    BenchmarkResult r;
    r.testId = id;
    r.name = name;
    r.target.name = workers.size() == 1 ? workers.first() : workers.join(QStringLiteral(" + "));
    r.target.vendorId = vendor;
    r.params.insert(QStringLiteral("workers"), QJsonArray::fromStringList(workers));
    r.correctness = QStringLiteral("checked");
    for (double d : ms) {
        RunSample s{ d, {}, true, {} };
        for (qsizetype w = 0; w < workers.size(); ++w)
            s.metrics.insert(QStringLiteral("w%1.rowsPercent").arg(w), 100.0 / workers.size());
        r.samples << s;
    }
    summarize(r);
    return r;
}

BenchmarkResult decode(const QString& id, const QString& name, const QString& target, quint32 vendor, QList<double> ms, double fps)
{
    BenchmarkResult r;
    r.testId = id;
    r.name = name;
    r.target.name = target;
    r.target.vendorId = vendor;
    r.correctness = QStringLiteral("checked");
    for (double d : ms)
        r.samples << RunSample{ d, { { QStringLiteral("fps"), fps }, { QStringLiteral("s0.ms"), d }, { QStringLiteral("s1.ms"), d / 5 } }, true, {} };
    summarize(r);
    return r;
}

const QString GT = QStringLiteral("NVIDIA GeForce GT 730");
const QString HD = QStringLiteral("Intel(R) HD Graphics");
const QString CPU = QStringLiteral("CPU, 3 threads");

} // namespace

class TestLabModel : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void staticRangesCoverEveryRowOnce()
    {
        const auto r = work::staticRanges(1024, { 0.833, 0.167 }, 16);
        QCOMPARE(r.size(), size_t(2));
        QCOMPARE(r[0].first, 0u);
        QCOMPARE(r[0].rows % 16, 0u);           // boundary aligned
        QCOMPARE(r[1].first, r[0].rows);
        QCOMPARE(r[0].rows + r[1].rows, 1024u);
        QCOMPARE(r[0].rows, 848u);              // 853.0 rounded to a multiple of 16
        // Odd height: the last non-empty worker takes the remainder.
        const auto odd = work::staticRanges(300, { 1, 1, 1 }, 16);
        QCOMPARE(odd[0].rows + odd[1].rows + odd[2].rows, 300u);
        QCOMPARE(odd[2].first + odd[2].rows, 300u);
        // Zero and negative shares get nothing; trailing zero share does not lose rows.
        const auto z = work::staticRanges(100, { 1, 0, -2 }, 8);
        QCOMPARE(z[0].rows, 100u);
        QCOMPARE(z[1].rows, 0u);
        QCOMPARE(z[2].rows, 0u);
        QCOMPARE(work::staticRanges(100, { 0, 0 }, 8)[0].rows, 100u); // nothing to go by: all to the first
        QVERIFY(work::staticRanges(100, {}, 8).empty());
    }

    void balancedSharesAndIdealTime()
    {
        const auto s = work::balancedShares({ 60, 300 });
        QVERIFY(qFuzzyCompare(s[0], 5.0 / 6.0));
        QVERIFY(qFuzzyCompare(s[1], 1.0 / 6.0));
        QVERIFY(qFuzzyCompare(work::idealSplitMs({ 60, 300 }), 50.0)); // 1 / (1/60 + 1/300)
        QVERIFY(qFuzzyCompare(work::idealSplitMs({ 60 }), 60.0));
        QCOMPARE(work::balancedShares({ 60, 0 })[1], 0.0);             // a worker without a time gets nothing
        QCOMPARE(work::idealSplitMs({}), 0.0);
    }

    void differenceWording()
    {
        QCOMPARE(describeDifference(compareStats(computeStats({ 50, 51, 52 }), computeStats({ 60, 61, 59 }))), QStringLiteral("1.18× faster"));
        QCOMPARE(describeDifference(compareStats(computeStats({ 120, 121, 119 }), computeStats({ 60, 61, 59 }))), QStringLiteral("2.00× slower"));
        QCOMPARE(describeDifference(compareStats(computeStats({ 60, 70, 50 }), computeStats({ 61, 59, 60 }))), QStringLiteral("no significant difference"));
    }

    void splitThatHelps()
    {
        Session s;
        s.results << result(QStringLiteral("lab.split.single"), GT + QStringLiteral(" alone"), { 60, 61, 59 }, { GT }, 0x10DE)
                  << result(QStringLiteral("lab.split.single"), HD + QStringLiteral(" alone"), { 300, 301, 299 }, { HD }, 0x8086)
                  << result(QStringLiteral("lab.split.single"), CPU + QStringLiteral(" alone"), { 850, 851, 849 }, { CPU }, 0)
                  << result(QStringLiteral("lab.split.equal"), QStringLiteral("Even split between the GPUs"), { 156, 157, 155 }, { GT, HD })
                  << result(QStringLiteral("lab.split.balanced"), QStringLiteral("Split by measured speed"), { 51, 52, 51.5 }, { GT, HD });
        BenchmarkResult wrong = result(QStringLiteral("lab.split.dynamic"), QStringLiteral("Shared queue"), { 10, 10, 10 }, { GT, HD });
        wrong.samples[1].correct = false;
        summarize(wrong);
        s.results << wrong;
        BenchmarkResult failed = result(QStringLiteral("lab.split.dynamic-cpu"), QStringLiteral("Queue + CPU"), {}, { GT, HD, CPU });
        failed.status = Status::Failed;
        failed.error = QStringLiteral("device lost");
        s.results << failed;
        s.results[3].notes << GT + QStringLiteral(": performance state P8 at the start of the measured runs, P8 at the end");
        s.results[2].notes << GT + QStringLiteral(": performance state P8 at the start of the measured runs, P8 at the end"); // CPU-only mode: GT idle
        s.results[4].notes << GT + QStringLiteral(": performance state P0 at the start of the measured runs, P0 at the end");

        const LabAnalysis a = analyzeSplit(s);
        QVERIFY(a.reference);
        QCOMPARE(a.reference->name, GT + QStringLiteral(" alone")); // fastest GPU; the CPU is never the reference
        QCOMPARE(a.verdict, Difference::Verdict::Faster);
        QVERIFY2(a.headline.startsWith(QStringLiteral("Combining helped: “Split by measured speed” took 51.5 ms, 1.17× faster")), qPrintable(a.headline));
        QCOMPARE(a.rows.size(), s.results.size());
        QVERIFY(a.rows[0].reference);
        QVERIFY(a.rows[4].idealMs && qFuzzyCompare(*a.rows[4].idealMs, 50.0));
        QCOMPARE(a.rows[4].detail, GT + QStringLiteral(" 50 % · ") + HD + QStringLiteral(" 50 %"));
        QVERIFY(!a.rows[5].vsReference);  // wrong output: never compared, never "fastest"
        const QString all = a.findings.join(u'\n');
        QVERIFY2(all.contains(QStringLiteral("would take 50.0 ms")), qPrintable(all));
        QVERIFY(all.contains(QStringLiteral("“Even split between the GPUs” was 2.60× slower")));
        QVERIFY(all.contains(QStringLiteral("“Shared queue” produced a wrong image")));
        QVERIFY(all.contains(QStringLiteral("“Queue + CPU” could not run: device lost")));
        QVERIFY(all.contains(QStringLiteral("During “Even split between the GPUs”, NVIDIA GeForce GT 730 stayed in performance state P8")));
        QVERIFY(!all.contains(QStringLiteral("During “Split by measured speed”"))); // P0: nothing to say
        QVERIFY(!all.contains(QStringLiteral("During “CPU, 3 threads alone”")));    // the GT 730 did no work there
    }

    void splitThatDoesNotHelp()
    {
        Session s;
        s.results << result(QStringLiteral("lab.split.single"), GT + QStringLiteral(" alone"), { 60, 62, 58 }, { GT }, 0x10DE)
                  << result(QStringLiteral("lab.split.single"), HD + QStringLiteral(" alone"), { 300, 301, 299 }, { HD }, 0x8086)
                  << result(QStringLiteral("lab.split.dynamic"), QStringLiteral("Shared queue"), { 59, 61, 60 }, { GT, HD });
        const LabAnalysis a = analyzeSplit(s);
        QCOMPARE(a.verdict, Difference::Verdict::NoSignificantChange);
        QVERIFY2(a.headline.startsWith(QStringLiteral("Combining did not help")), qPrintable(a.headline));

        Session none; // only a CPU result: no GPU reference
        none.results << result(QStringLiteral("lab.split.single"), CPU, { 850 }, { CPU }, 0);
        QVERIFY(!analyzeSplit(none).reference);
        QVERIFY(!analyzeSplit(none).verdict);
    }

    // The adaptive scheduler session: the best policy against the fastest GPU alone, the helper's
    // share and how often it took part, split runs shown apart from baseline runs, the fallback check;
    // a policy that ended up slower is reported as slower, never as a gain.
    void adaptiveScheduler()
    {
        auto adaptive = [](const QString& id, const QString& name, QList<double> ms, QList<double> split, double helperRows) {
            BenchmarkResult r = result(id, name, ms, { GT, HD });
            for (qsizetype i = 0; i < r.samples.size(); ++i) {
                r.samples[i].metrics.insert(QStringLiteral("w0.rowsPercent"), 100 - helperRows * split[i]);
                r.samples[i].metrics.insert(QStringLiteral("w1.rowsPercent"), helperRows * split[i]);
                r.samples[i].metrics.insert(QStringLiteral("split"), split[i]);
                r.samples[i].metrics.insert(QStringLiteral("overheadMs"), 0.1);
                r.samples[i].metrics.insert(QStringLiteral("scheduleMs"), 0.004);
                r.samples[i].metrics.insert(QStringLiteral("w1.rerunRows"), id.endsWith(QLatin1StringView("fallback")) ? 16 : 0);
            }
            summarize(r);
            return r;
        };
        Session s;
        s.results << result(QStringLiteral("lab.split.single"), GT + QStringLiteral(" alone"), { 65, 66, 65.5, 65, 66 }, { GT }, 0x10DE)
                  << result(QStringLiteral("lab.split.single"), HD + QStringLiteral(" alone"), { 318, 319, 318 }, { HD }, 0x8086)
                  << adaptive(QStringLiteral("lab.adaptive.conservative"), QStringLiteral("Adaptive, conservative"), { 57, 57.5, 56.5, 57, 65.5 },
                              { 1, 1, 1, 1, 0 }, 13)
                  << adaptive(QStringLiteral("lab.adaptive.performance"), QStringLiteral("Adaptive, performance"), { 70, 71, 70.5, 70, 71 },
                              { 1, 1, 1, 1, 1 }, 30)
                  << adaptive(QStringLiteral("lab.adaptive.fallback"), QStringLiteral("Fallback"), { 66, 66.5, 66 }, { 1, 1, 1 }, 0);
        s.results[2].notes << HD + QStringLiteral(" after the last run: in use, next planned share 20.0 % — the split was 15.0 % faster than the primary alone.");
        QCOMPARE(labKind(s), QStringLiteral("adaptive"));
        const LabAnalysis a = analyzeAdaptive(s);
        QVERIFY(a.reference);
        QCOMPARE(a.reference->name, GT + QStringLiteral(" alone"));
        QCOMPARE(a.verdict, Difference::Verdict::Faster);
        QVERIFY2(a.headline.startsWith(QStringLiteral("The adaptive scheduler helped: “Adaptive, conservative”")), qPrintable(a.headline));
        QVERIFY2(a.headline.contains(HD + QStringLiteral(" computed 10.4 % of the rows")), qPrintable(a.headline));
        QVERIFY2(a.rows[2].detail.contains(QStringLiteral("helper used in 80 % of runs")) && a.rows[2].detail.contains(QStringLiteral("split runs alone 57.0 ms")),
                 qPrintable(a.rows[2].detail));
        const QString all = a.findings.join(u'\n');
        QVERIFY2(all.contains(QStringLiteral("“Adaptive, performance”: 70.5 ms (1.08× slower than NVIDIA GeForce GT 730 alone)")), qPrintable(all));
        QVERIFY(all.contains(QStringLiteral("after the last run: in use")));
        QVERIFY(all.contains(QStringLiteral("Fallback check")) && all.contains(QStringLiteral("re-ran 16 rows")));

        // Every policy slower: the headline says so.
        Session slow;
        slow.results << s.results[0] << s.results[1] << s.results[3];
        const LabAnalysis b = analyzeAdaptive(slow);
        QCOMPARE(b.verdict, Difference::Verdict::Slower);
        QVERIFY2(b.headline.startsWith(QStringLiteral("The adaptive scheduler was slower")), qPrintable(b.headline));
        QCOMPARE(labKind(Session{}), QStringLiteral("split"));

        // Since 1.12.1 each GPU alone is "lab.adaptive.single" (sessions of 1.12.0, above, used
        // "lab.split.single"): same analysis, and never counted as a policy.
        Session renamed = s;
        renamed.results[0].testId = renamed.results[1].testId = QStringLiteral("lab.adaptive.single");
        const LabAnalysis c = analyzeAdaptive(renamed);
        QCOMPARE(c.reference->name, GT + QStringLiteral(" alone"));
        QCOMPARE(c.headline, a.headline);
        QVERIFY(!c.findings.join(u'\n').contains(QStringLiteral("“NVIDIA GeForce GT 730 alone”:")));

        // A PC with one GPU: only its single run; filed as the adaptive experiment, and said plainly.
        Session one;
        one.results << renamed.results[0];
        QCOMPARE(labKind(one), QStringLiteral("adaptive"));
        const LabAnalysis d = analyzeAdaptive(one);
        QVERIFY2(d.headline.startsWith(QStringLiteral("Only one GPU could run the workload, so there was nothing to divide")), qPrintable(d.headline));

        // A real helper failure is reported in the findings.
        Session failed = renamed;
        failed.results[2].notes << HD + QStringLiteral(" failed a batch (The GPU stopped responding); the primary re-ran its rows.");
        QVERIFY(analyzeAdaptive(failed).findings.join(u'\n').contains(QStringLiteral("failed a batch (The GPU stopped responding)")));
    }

    void decodeOnePerGpuSlowerThanBestDevice()
    {
        Session s;
        s.results << decode(QStringLiteral("lab.decode.one"), QStringLiteral("One stream — Software (CPU)"), QStringLiteral("CPU: i5"), 0, { 640, 645, 642 }, 467)
                  << decode(QStringLiteral("lab.decode.one"), QStringLiteral("One stream — GT 730"), GT, 0x10DE, { 1586, 1590, 1580 }, 189)
                  << decode(QStringLiteral("lab.decode.one"), QStringLiteral("One stream — HD"), HD, 0x8086, { 340, 345, 342 }, 884)
                  << decode(QStringLiteral("lab.decode.two-same"), QStringLiteral("Two streams — both on GT 730"), GT, 0x10DE, { 3146, 3150, 3140 }, 191)
                  << decode(QStringLiteral("lab.decode.two-same"), QStringLiteral("Two streams — both on HD"), HD, 0x8086, { 700, 705, 703 }, 854)
                  << decode(QStringLiteral("lab.decode.two-split"), QStringLiteral("Two streams — one on each GPU"), GT + QStringLiteral(" + ") + HD, 0,
                            { 1584, 1586, 1582 }, 379);
        const LabAnalysis a = analyzeDecode(s);
        QCOMPARE(a.reference->name, QStringLiteral("Two streams — both on HD")); // fastest way to decode two streams
        QCOMPARE(a.verdict, Difference::Verdict::Slower);
        QVERIFY2(a.headline.startsWith(QStringLiteral("Using both GPUs was slower: one stream on each GPU took 1584.0 ms, 2.25× slower")), qPrintable(a.headline));
        QVERIFY(!a.rows[0].vsReference);  // one-stream rows are not compared with a two-stream reference
        QVERIFY(a.rows[5].detail.contains(QStringLiteral("NVIDIA GeForce GT 730 finished its stream in 1584.0 ms")));
        QVERIFY2(a.findings.first().startsWith(QStringLiteral("One stream at a time: Intel(R) HD Graphics 884 frames/s, software (CPU) 467 frames/s, NVIDIA GeForce GT 730 189")),
                 qPrintable(a.findings.first()));
    }
};

QTEST_GUILESS_MAIN(TestLabModel)
#include "tst_labmodel.moc"
