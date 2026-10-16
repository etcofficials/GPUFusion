// Pure tests for the benchmark framework: statistics, comparable keys, baselines and the
// significance rule, CSV/JSON, retention — and the reference kernel's pinned values.
#include "benchmarks/BenchmarkModel.h"
#include "workloads/HashKernel.h"

#include <QJsonDocument>
#include <QTest>

using namespace gf;
using namespace gf::bench;

namespace {

BenchmarkResult result(const QString& id, QList<double> durations, QJsonObject params = {}, const QString& driver = QStringLiteral("1.0"))
{
    BenchmarkResult r;
    r.testId = id;
    r.name = id;
    r.target = Target{ QStringLiteral("GPU"), 0x10DE, 0x0F02, 0x098F10DE, driver };
    r.params = params;
    r.correctness = QStringLiteral("checked");
    for (double d : durations)
        r.samples << RunSample{ d, {}, true, {} };
    summarize(r);
    return r;
}

Session session(const QString& id, const QDateTime& when, QList<BenchmarkResult> results)
{
    Session s;
    s.id = id;
    s.started = when;
    s.results = std::move(results);
    return s;
}

} // namespace

class TestBenchmarkModel : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void statistics()
    {
        const Stats s = computeStats({ 14, 10, 12 });
        QCOMPARE(s.n, 3);
        QCOMPARE(s.mean, 12.0);
        QCOMPARE(s.stddev, 2.0); // sample standard deviation
        QCOMPARE(s.median, 12.0);
        QCOMPARE(s.min, 10.0);
        QCOMPARE(s.max, 14.0);
        QVERIFY(qFuzzyCompare(s.cv(), 2.0 / 12.0));
        QCOMPARE(computeStats({ 4, 1, 3, 2 }).median, 2.5);
        QCOMPARE(computeStats({ 5 }).stddev, 0.0);
        QCOMPARE(computeStats({}).n, 0);
    }

    void summarizeCollectsMetricsAndCorrectness()
    {
        BenchmarkResult r;
        r.correctness = QStringLiteral("checked");
        r.samples << RunSample{ 10, { { QStringLiteral("gpuMs"), 9 } }, true, {} } << RunSample{ 12, { { QStringLiteral("gpuMs"), 11 } }, false, {} };
        summarize(r);
        QCOMPARE(r.duration.mean, 11.0);
        QCOMPARE(r.metrics.value(QStringLiteral("gpuMs")).mean, 10.0);
        QVERIFY(r.correct.has_value() && !*r.correct); // one bad run makes the result incorrect
        BenchmarkResult unchecked;
        unchecked.correctness = QStringLiteral("Not checked");
        unchecked.samples << RunSample{ 1, {}, true, {} };
        summarize(unchecked);
        QVERIFY(!unchecked.correct.has_value());
    }

    void comparableKeysSeparateDifferentWork()
    {
        const QJsonObject p{ { "width", 1024 }, { "iterations", 512 } };
        const QJsonObject sameReordered{ { "iterations", 512 }, { "width", 1024 } };
        QCOMPARE(result(QStringLiteral("gpu.hash"), { 1 }, p).comparableKey(), result(QStringLiteral("gpu.hash"), { 2 }, sameReordered).comparableKey());
        QVERIFY(result(QStringLiteral("gpu.hash"), { 1 }, p).comparableKey() != result(QStringLiteral("gpu.hash"), { 1 }, QJsonObject{ { "width", 512 } }).comparableKey());
        QVERIFY(result(QStringLiteral("gpu.hash"), { 1 }, p).comparableKey() != result(QStringLiteral("gpu.hash"), { 1 }, p, QStringLiteral("2.0")).comparableKey());
        BenchmarkResult v2 = result(QStringLiteral("gpu.hash"), { 1 }, p);
        v2.testVersion = 2;
        QVERIFY(v2.comparableKey() != result(QStringLiteral("gpu.hash"), { 1 }, p).comparableKey());
    }

    void baselineIsTheLatestComparableEarlierResult()
    {
        const QDateTime t0(QDate(2026, 10, 1), QTime(12, 0));
        BenchmarkResult failed = result(QStringLiteral("a"), { 5 });
        failed.status = Status::Failed;
        BenchmarkResult wrong = result(QStringLiteral("a"), { 6 });
        wrong.correct = false;
        const QList<Session> history{
            session(QStringLiteral("s1"), t0, { result(QStringLiteral("a"), { 10, 11 }) }),
            session(QStringLiteral("s2"), t0.addDays(1), { result(QStringLiteral("a"), { 20, 21 }) }),
            session(QStringLiteral("s3"), t0.addDays(2), { failed, wrong, result(QStringLiteral("a"), { 7 }, QJsonObject{ { "other", 1 } }) }),
            session(QStringLiteral("current"), t0.addDays(3), { result(QStringLiteral("a"), { 30 }) }),
        };
        const auto base = findBaseline(result(QStringLiteral("a"), { 1 }), history, QStringLiteral("current"));
        QVERIFY(base.has_value());
        QCOMPARE(base->second, t0.addDays(1));        // s2: newest valid comparable
        QCOMPARE(base->first.duration.mean, 20.5);
        QVERIFY(!findBaseline(result(QStringLiteral("b"), { 1 }), history, {}).has_value());
        // Viewing an old result: later sessions are never its baseline.
        BenchmarkResult old = result(QStringLiteral("a"), { 1 });
        old.started = t0.addDays(1).addSecs(60);
        QCOMPARE(findBaseline(old, history, QStringLiteral("s2"))->second, t0);
    }

    void sessionSummaryComparesOnlyIdenticalVerifiedWork()
    {
        const QJsonObject hash{ { "width", 512 }, { "height", 512 }, { "iterations", 256 }, { "seed", 12345 }, { "bandRows", 128 } };
        auto withTarget = [](BenchmarkResult r, const QString& name, quint32 vendor, const QString& api) {
            r.target.name = name;
            r.target.vendorId = vendor;
            r.api = api;
            return r;
        };
        QJsonObject cpuParams = hash;
        cpuParams.insert(QStringLiteral("threads"), 4);
        BenchmarkResult wrong = withTarget(result(QStringLiteral("gpu.hash"), { 1 }, hash), QStringLiteral("Broken GPU"), 0x1234, {});
        wrong.correct = false;
        Session s = session(QStringLiteral("s"), QDateTime::currentDateTime(), {
            withTarget(result(QStringLiteral("gpu.hash"), { 7.1, 7.2, 7.3 }, hash), QStringLiteral("GT 730"), 0x10DE, {}),
            withTarget(result(QStringLiteral("gpu.hash"), { 38, 39, 38.5 }, hash), QStringLiteral("HD Graphics"), 0x8086, {}),
            withTarget(result(QStringLiteral("cpu.hash.mt"), { 7.2, 7.15, 7.25 }, cpuParams), QStringLiteral("CPU"), 0, QStringLiteral("CPU, 4 threads")),
            wrong,
            result(QStringLiteral("monitor.sample"), { 0.2 }),
        });
        const QStringList lines = sessionSummary(s);
        QCOMPARE(lines.size(), 1);
        const QString& line = lines.first();
        QVERIFY2(line.startsWith(QStringLiteral("Hash workload (512×512, 256 iterations): ")), qPrintable(line));
        QVERIFY2(line.contains(QStringLiteral("(fastest)")), qPrintable(line));
        QVERIFY2(line.contains(QStringLiteral("HD Graphics 38.50 ms (5.3× as long)")), qPrintable(line));
        QVERIFY2(line.contains(QStringLiteral("about the same")), qPrintable(line)); // GT 730 vs CPU: within noise
        QVERIFY(!line.contains(QStringLiteral("Broken GPU")));
        // Different workload sizes are never compared.
        QJsonObject other = hash;
        other.insert(QStringLiteral("width"), 1024);
        Session mixed = session(QStringLiteral("m"), QDateTime::currentDateTime(), {
            withTarget(result(QStringLiteral("gpu.hash"), { 7 }, hash), QStringLiteral("A"), 1, {}),
            withTarget(result(QStringLiteral("gpu.hash"), { 9 }, other), QStringLiteral("B"), 2, {}) });
        QVERIFY(sessionSummary(mixed).isEmpty());
    }

    void significanceRule()
    {
        const auto base = std::make_pair(result(QStringLiteral("a"), { 100, 101, 99, 100, 100 }), QDateTime(QDate(2026, 10, 1), QTime(9, 0)));
        const Comparison faster = compare(result(QStringLiteral("a"), { 80, 81, 79, 80, 80 }), base);
        QCOMPARE(faster.verdict, Comparison::Verdict::Faster);
        QVERIFY(faster.text.contains(QStringLiteral("20.0 % faster")));
        QCOMPARE(compare(result(QStringLiteral("a"), { 120, 121, 119, 120, 120 }), base).verdict, Comparison::Verdict::Slower);
        // A 5 % shift inside large run-to-run noise is not a result.
        const auto noisy = std::make_pair(result(QStringLiteral("a"), { 60, 140, 90, 110, 100 }), base.second);
        QCOMPARE(compare(result(QStringLiteral("a"), { 55, 135, 85, 105, 95 }), noisy).verdict, Comparison::Verdict::NoSignificantChange);
        // Below 1 % is never called a change, however tight the runs.
        QCOMPARE(compare(result(QStringLiteral("a"), { 99.5, 99.5, 99.5 }), std::make_pair(result(QStringLiteral("a"), { 100, 100, 100 }), base.second)).verdict,
                 Comparison::Verdict::NoSignificantChange);
        QCOMPARE(compare(result(QStringLiteral("a"), { 1 }), std::nullopt).verdict, Comparison::Verdict::NoBaseline);
    }

    void csvIsWellFormed()
    {
        BenchmarkResult r = result(QStringLiteral("gpu.hash"), { 1.5, 2.5 }, QJsonObject{ { "width", 1024 }, { "height", 1024 } });
        r.name = QStringLiteral("Hash, \"quoted\"");
        const QString rows = csvRows(session(QStringLiteral("s"), QDateTime::currentDateTime(), { r }));
        QCOMPARE(rows.count(u'\n'), 1);
        QVERIFY(rows.contains(QStringLiteral("\"Hash, \"\"quoted\"\"\"")));
        QVERIFY(rows.contains(QStringLiteral("\"{\"\"height\"\":1024,\"\"width\"\":1024}\"")));
        QCOMPARE(csvField(QStringLiteral("plain")), QStringLiteral("plain"));
        QCOMPARE(csvHeader().count(u','), 18);
        // Same number of columns in every row as in the header (outside quotes).
        int commas = 0;
        bool quoted = false;
        for (QChar c : rows) {
            if (c == u'"') quoted = !quoted;
            else if (c == u',' && !quoted) ++commas;
        }
        QCOMPARE(commas, 18);
    }

    void sessionJsonRoundTrip()
    {
        BenchmarkResult r = result(QStringLiteral("gpu.hash"), { 3, 4 }, QJsonObject{ { "width", 64 } });
        r.samples[0].metrics.insert(QStringLiteral("gpuTimestampMs"), 2.5);
        summarize(r);
        r.gpuBusyPercent = mon::MetricValue::ok(97);
        r.cpuPercent = mon::MetricValue::unsupported(QStringLiteral("n/a"));
        r.warnings << QStringLiteral("w");
        Session s = session(QStringLiteral("20261016-220000"), QDateTime(QDate(2026, 10, 16), QTime(22, 0)), { r });
        s.cpu = QStringLiteral("i5");
        const auto back = Session::fromJson(QJsonDocument::fromJson(QJsonDocument(s.toJson()).toJson()).object());
        QVERIFY(back);
        QCOMPARE(back->results.size(), 1);
        const BenchmarkResult& b = back->results.first();
        QCOMPARE(b.comparableKey(), r.comparableKey());
        QCOMPARE(b.duration.mean, 3.5);
        QCOMPARE(b.metrics.value(QStringLiteral("gpuTimestampMs")).mean, 2.5);
        QCOMPARE(b.gpuBusyPercent.value, 97.0);
        QCOMPARE(b.cpuPercent.state, mon::MetricValue::State::Unsupported); // states survive, never become 0
        QVERIFY(b.correct.has_value() && *b.correct);
        QCOMPARE(b.warnings, QStringList{ QStringLiteral("w") });
    }

    void retention()
    {
        const QDateTime now(QDate(2026, 10, 16), QTime(12, 0));
        const QList<std::pair<QString, QDateTime>> files{ { QStringLiteral("old"), now.addDays(-100) }, { QStringLiteral("new"), now.addDays(-10) } };
        QCOMPARE(filesToPrune(files, now, 90), QStringList{ QStringLiteral("old") });
        QVERIFY(filesToPrune(files, now, 0).isEmpty()); // keep forever
    }

    void referenceKernelIsPinned()
    {
        // Values computed independently (arbitrary-precision arithmetic, not this C++ code).
        QCOMPARE(work::hashPixel(0, 0, 12345, 0), 4221321151u);
        QCOMPARE(work::hashPixel(1, 2, 12345, 10), 1370742869u);
        QCOMPARE(work::hashPixel(511, 511, 12345, 256), 1601088597u);
        QCOMPARE(work::hashPixel(0, 0, 0, 1), 478185513u); // zero state takes the fallback seed
    }

    void cpuThreadingDoesNotChangeTheResult()
    {
        work::HashParams p;
        p.width = 64;
        p.height = 48;
        p.iterations = 32;
        const auto one = work::hashRowsCpu(p, 0, p.height, 1);
        const auto many = work::hashRowsCpu(p, 0, p.height, 4);
        QCOMPARE(work::firstMismatch(one, many), -1LL);
        const auto part = work::hashRowsCpu(p, 10, 5, 3); // a band: same values as rows 10..14
        QCOMPARE(part.size(), size_t(64 * 5));
        QVERIFY(std::equal(part.begin(), part.end(), one.begin() + 10 * 64));
        auto broken = many;
        broken[100] ^= 1u;
        QCOMPARE(work::firstMismatch(one, broken), 100LL);
    }
};

QTEST_GUILESS_MAIN(TestBenchmarkModel)
#include "tst_benchmarkmodel.moc"
