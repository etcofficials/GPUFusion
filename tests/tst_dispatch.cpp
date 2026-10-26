// Tests for the generic multi-GPU job dispatcher: GPU selection, jobs and placeholders, the dynamic
// queue (a faster worker takes more jobs), the command executor (with cmd.exe as the "program") and
// the interpretation of a comparison (significance, failures, output consistency).
#include "dispatch/JobDispatcher.h"

#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTest>
#include <QThread>

using namespace gf;
using namespace gf::dispatch;

namespace {

const Worker kNv{ QStringLiteral("NVIDIA GeForce GT 730"), 0x10DE, 0x7DEC, 0, false };
const Worker kIntel{ QStringLiteral("Intel(R) HD Graphics"), 0x8086, 0x870A, 1, true };
const QList<Worker> kWorkers{ kNv, kIntel };

RunResult fakeRun(const QList<int>& workers, double wall, int jobs, const QList<QString>& checksums = {}, int failJob = -1)
{
    RunResult r;
    r.workers = workers;
    r.wallMs = wall;
    for (int j = 0; j < jobs; ++j) {
        JobResult jr;
        jr.job = j;
        jr.worker = workers[j % workers.size()];
        jr.ok = j != failJob;
        jr.error = jr.ok ? QString() : QStringLiteral("boom");
        if (j < checksums.size())
            jr.checksum = checksums[j];
        r.jobs << jr;
    }
    return r;
}

ConfigResult config(const QString& label, const QList<int>& workers, const QList<double>& walls, int jobs = 4,
                    const QList<QString>& checksums = {}, int failJob = -1)
{
    ConfigResult c;
    c.label = label;
    c.workers = workers;
    for (double w : walls)
        c.runs << fakeRun(workers, w, jobs, checksums, failJob);
    return c;
}

} // namespace

class TestDispatch : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void selectingGpus()
    {
        QString error;
        QCOMPARE(*selectWorkers(kWorkers, QStringLiteral("all"), &error), (QList<int>{ 0, 1 }));
        QCOMPARE(*selectWorkers(kWorkers, QStringLiteral("integrated"), &error), QList<int>{ 1 });
        QCOMPARE(*selectWorkers(kWorkers, QStringLiteral("discrete"), &error), QList<int>{ 0 });
        QCOMPARE(*selectWorkers(kWorkers, QStringLiteral("1, 0"), &error), (QList<int>{ 1, 0 }));
        QVERIFY(!selectWorkers(kWorkers, QStringLiteral("5"), &error));
        QVERIFY(error.contains(QStringLiteral("5")));
        QVERIFY(!selectWorkers({ kNv }, QStringLiteral("integrated"), &error));
    }

    void jobsAndPlaceholders()
    {
        const QList<Job> jobs = jobsFor({ QStringLiteral("C:\\in\\My Clip.mp4"), QStringLiteral("C:\\in\\b.mp4") }, QStringLiteral("D:\\out"),
                                        QStringLiteral("{stem}-{job}.mkv"));
        QCOMPARE(jobs.size(), 2);
        QCOMPARE(jobs[0].output, QStringLiteral("D:\\out\\My Clip-0.mkv"));
        QVERIFY(jobsFor({ QStringLiteral("C:\\a.mp4") }, QStringLiteral("D:\\out"), QString())[0].output.isEmpty());

        QCOMPARE(expandPlaceholders(QStringLiteral("{dxgi_index}|{luid}|{vendor_id}|{vendor_hex}|{gpu_name}|{stem}|{job}"), kIntel, jobs[1]),
                 QStringLiteral("1|00000000_0000870A|32902|0x8086|Intel(R) HD Graphics|b|1"));
        QCOMPARE(expandPlaceholders(QStringLiteral("{luid_pair} {luid_high} {luid_low}"), kIntel, jobs[1]), QStringLiteral("0,34570 0 34570"));
        const QStringList cmd = commandLine(QStringLiteral("\"C:\\Program Files\\tool.exe\" --gpu {dxgi_index} -i {input} -o {output}"), kNv, jobs[0]);
        QCOMPARE(cmd, (QStringList{ QStringLiteral("C:\\Program Files\\tool.exe"), QStringLiteral("--gpu"), QStringLiteral("0"), QStringLiteral("-i"),
                                    QStringLiteral("C:\\in\\My Clip.mp4"), QStringLiteral("-o"), QStringLiteral("D:\\out\\My Clip-0.mkv") }));
    }

    // A worker that is three times as fast should end up with about three times as many jobs.
    void dynamicQueueFavoursTheFasterWorker()
    {
        QStringList inputs;
        for (int i = 0; i < 24; ++i)
            inputs << QStringLiteral("in%1").arg(i);
        const Executor executor = [](const Job&, const Worker& w, const std::atomic<bool>*) {
            QThread::msleep(w.integrated ? 30 : 10);
            JobResult r;
            r.ok = true;
            return r;
        };
        const RunResult r = runJobs(kWorkers, { 0, 1 }, jobsFor(inputs, {}, {}), executor);
        QCOMPARE(r.failed(), 0);
        QCOMPARE(r.jobs.size(), 24);
        const QMap<int, int> per = r.jobsPerWorker();
        QCOMPARE(per.value(0) + per.value(1), 24);
        QVERIFY2(per.value(0) > per.value(1), qPrintable(QStringLiteral("%1 vs %2").arg(per.value(0)).arg(per.value(1))));
        for (int j = 0; j < r.jobs.size(); ++j)
            QCOMPARE(r.jobs[j].job, j);
    }

    // A much slower GPU must not take the last jobs when the faster one would finish them all sooner.
    void stragglersAreAvoided()
    {
        QVERIFY(shouldTakeJob(30, 10, 3));   // 30 ms <= 3 jobs x 10 ms
        QVERIFY(!shouldTakeJob(30, 10, 2));  // the fast GPU finishes both in 20 ms
        QVERIFY(shouldTakeJob(30, 0, 1));    // nothing known about the others yet
        QVERIFY(shouldTakeJob(0, 10, 1));    // nothing known about this worker yet

        const Executor executor = [](const Job&, const Worker& w, const std::atomic<bool>*) {
            QThread::msleep(w.integrated ? 10 : 60);
            JobResult r;
            r.ok = true;
            return r;
        };
        const QMap<int, double> expected{ { 0, 60.0 }, { 1, 10.0 } }; // measured alone
        const RunResult r = runJobs(kWorkers, { 0, 1 }, jobsFor({ "a", "b", "c" }, {}, {}), executor, nullptr, {}, expected);
        QCOMPARE(r.failed(), 0);
        QCOMPARE(r.jobsPerWorker().value(0), 0); // 60 ms > 3 x 10 ms: the slow GPU stays out
        QCOMPARE(r.jobsPerWorker().value(1), 3);

        const RunResult same = runJobs(kWorkers, { 1, 1 }, jobsFor({ "a", "b", "c", "d" }, {}, {}), executor, nullptr, {}, expected);
        QCOMPARE(same.failed(), 0); // two slots of the same GPU never hold each other back
    }

    // The last worker still taking jobs never stops early: a worker that already stepped aside will
    // not do the remaining jobs. (Before 1.11.1 the remaining jobs here were never run.)
    void lastActiveWorkerFinishesTheQueue()
    {
        // Worker 1 is expected to be 5x slower: it steps aside at once (50 ms > 3 x 10 ms).
        // Worker 0's first job then takes 300 ms, so its average rises far above 2 x 50 ms.
        const Executor executor = [](const Job& j, const Worker& w, const std::atomic<bool>*) {
            QThread::msleep(w.integrated ? 50 : (j.index == 0 ? 300 : 10));
            JobResult r;
            r.ok = true;
            return r;
        };
        const RunResult r = runJobs(kWorkers, { 0, 1 }, jobsFor({ "a", "b", "c" }, {}, {}), executor, nullptr, {},
                                    QMap<int, double>{ { 0, 10.0 }, { 1, 50.0 } });
        QCOMPARE(r.failed(), 0);
        QCOMPARE(r.jobsPerWorker().value(0), 3);
    }

    // A job's output is deleted before it runs, so an output that is an input file (or another job's
    // output) is refused: the input must survive.
    void outputsThatWouldOverwriteInputsAreRefused()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString a = QDir::toNativeSeparators(dir.filePath(QStringLiteral("a.txt")));
        const QString b = QDir::toNativeSeparators(dir.filePath(QStringLiteral("b.txt")));
        for (const QString& f : { a, b }) {
            QFile file(f);
            QVERIFY(file.open(QIODevice::WriteOnly));
            file.write("precious");
        }
        const QString cmd = QDir(qEnvironmentVariable("SystemRoot")).filePath(QStringLiteral("System32\\cmd.exe"));
        const Executor copy = commandExecutor(QStringLiteral("\"%1\" /c copy /y {input} {output}").arg(QDir::toNativeSeparators(cmd)));
        // Pattern {name} into the input folder: every output IS its input.
        const QList<Job> sameFolder = jobsFor({ a, b }, dir.path(), QStringLiteral("{name}"));
        QCOMPARE(unsafeOutputs(sameFolder).size(), 2);
        const RunResult r = runJobs(kWorkers, { 0 }, sameFolder, copy);
        QCOMPARE(r.failed(), 2);
        QVERIFY(r.jobs[0].error.startsWith(QStringLiteral("refused")));
        for (const QString& f : { a, b }) {
            QFile file(f);
            QVERIFY(file.open(QIODevice::ReadOnly));
            QCOMPARE(file.readAll(), QByteArray("precious"));                 // untouched
        }
        // Two inputs mapping to one output name.
        const QList<Job> clash = jobsFor({ a, QDir::toNativeSeparators(dir.filePath(QStringLiteral("sub/a.txt"))) }, dir.filePath(QStringLiteral("out")),
                                         QStringLiteral("{stem}.out"));
        QCOMPARE(unsafeOutputs(clash).size(), 2);
        QVERIFY(unsafeOutputs(jobsFor({ a, b }, dir.filePath(QStringLiteral("out")), QStringLiteral("{stem}.out"))).isEmpty());
    }

    // Placeholders are expanded in one pass: text inserted by a value is not expanded again.
    void placeholdersExpandOnce()
    {
        const QList<Job> jobs = jobsFor({ QStringLiteral("C:\\in\\clip{job}.mp4") }, QStringLiteral("D:\\out"), QStringLiteral("{stem}-{job}.mkv"));
        QCOMPARE(jobs[0].output, QStringLiteral("D:\\out\\clip{job}-0.mkv"));
        QCOMPARE(expandPlaceholders(QStringLiteral("{input}|{job}|{unknown}"), kIntel, jobs[0]),
                 QStringLiteral("C:\\in\\clip{job}.mp4|0|{unknown}"));
    }

    void cancellationStopsTakingJobs()
    {
        std::atomic<bool> cancel{ false };
        const Executor executor = [&cancel](const Job& j, const Worker&, const std::atomic<bool>*) {
            if (j.index == 1)
                cancel = true;
            JobResult r;
            r.ok = true;
            return r;
        };
        const RunResult r = runJobs(kWorkers, { 0 }, jobsFor({ "a", "b", "c", "d" }, {}, {}), executor, &cancel);
        QVERIFY(r.cancelled);
        QCOMPARE(r.failed(), 2);
        QVERIFY(r.jobs[3].error.contains(QStringLiteral("cancelled")));
    }

    // Any program can be a job: cmd.exe copies the input to the output; the checksum is the output's hash.
    void commandExecutorRunsAnyProgram()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString in = QDir::toNativeSeparators(dir.filePath(QStringLiteral("in file.txt")));
        QFile f(in);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("hello");
        f.close();
        const QString cmd = QDir(qEnvironmentVariable("SystemRoot")).filePath(QStringLiteral("System32\\cmd.exe"));
        const Executor copy = commandExecutor(QStringLiteral("\"%1\" /c copy /y {input} {output}").arg(QDir::toNativeSeparators(cmd)));
        const QList<Job> jobs = jobsFor({ in }, dir.filePath(QStringLiteral("out")), QStringLiteral("{stem}.copy"));
        const RunResult r = runJobs(kWorkers, { 1 }, jobs, copy);
        QVERIFY2(r.jobs[0].ok, qPrintable(r.jobs[0].error));
        QCOMPARE(*r.jobs[0].checksum, QStringLiteral("aaf4c61ddcc5e8a2dabede0f3b482cd9aea9434d")); // sha1("hello")

        const Executor fails = commandExecutor(QStringLiteral("\"%1\" /c exit 3").arg(QDir::toNativeSeparators(cmd)));
        const RunResult bad = runJobs(kWorkers, { 0 }, jobs, fails);
        QVERIFY(!bad.jobs[0].ok);
        QVERIFY(bad.jobs[0].error.startsWith(QStringLiteral("exit code 3")) || bad.jobs[0].error.contains(QStringLiteral("no output")));

        // The GPU assignment reaches SDK-integrated programs through the environment.
        const Executor echo = commandExecutor(QStringLiteral("\"%1\" /c echo %GPUFUSION_ADAPTERS%>{output}").arg(QDir::toNativeSeparators(cmd)));
        // (cmd.exe redirection: an output path without spaces, so the argument is not quoted)
        const QList<Job> envJobs = jobsFor({ in }, dir.filePath(QStringLiteral("out")), QStringLiteral("env.txt"));
        const RunResult env = runJobs(kWorkers, { 1 }, envJobs, echo);
        QVERIFY2(env.jobs[0].ok, qPrintable(env.jobs[0].error));
        QFile out(envJobs[0].output);
        QVERIFY(out.open(QIODevice::ReadOnly));
        QCOMPARE(out.readAll().trimmed(), QByteArray("00000000_0000870A"));
    }

    // ---- interpretation -------------------------------------------------------------------------
    void significantGainWithConsistentOutputs()
    {
        const QStringList sums{ "a", "b", "c", "d" };
        Experiment e;
        e.configs << config(QStringLiteral("NV alone"), { 0 }, { 1000, 1010, 990 }, 4, sums)
                  << config(QStringLiteral("Intel alone"), { 1 }, { 2000, 2020, 1990 }, 4, sums)
                  << config(QStringLiteral("NV alone, 2 at a time"), { 0, 0 }, { 950, 960, 940 }, 4, sums)
                  << config(QStringLiteral("All 2 GPUs"), { 0, 1 }, { 700, 710, 690 }, 4, sums);
        analyze(e, kWorkers);
        QCOMPARE(*e.bestSingle, 2); // the concurrency control is the bar to beat
        QVERIFY(e.outputsConsistent);
        QVERIFY2(e.verdict.startsWith(QStringLiteral("Faster")), qPrintable(e.verdict));
        QVERIFY(e.verdict.contains(QStringLiteral("2 at a time")));
    }

    void noGainAndInconsistentAndFailed()
    {
        Experiment same;
        same.configs << config(QStringLiteral("NV alone"), { 0 }, { 1000, 1100, 900 }) << config(QStringLiteral("Intel alone"), { 1 }, { 3000, 3000, 3000 })
                     << config(QStringLiteral("All"), { 0, 1 }, { 990, 1090, 950 });
        analyze(same, kWorkers);
        QVERIFY2(same.verdict.startsWith(QStringLiteral("No significant difference")), qPrintable(same.verdict));
        QVERIFY(same.notes.join(u' ').contains(QStringLiteral("could not be compared")));

        Experiment differ;
        differ.configs << config(QStringLiteral("NV alone"), { 0 }, { 1000, 1000 }, 4, { "a", "b", "c", "d" })
                       << config(QStringLiteral("Intel alone"), { 1 }, { 2000, 2000 }, 4, { "a", "b", "c", "d" })
                       << config(QStringLiteral("All"), { 0, 1 }, { 500, 500 }, 4, { "a", "X", "c", "d" });
        analyze(differ, kWorkers);
        QVERIFY(!differ.outputsConsistent);
        QVERIFY(differ.verdict.startsWith(QStringLiteral("Not comparable: outputs differ")));

        Experiment failed;
        failed.configs << config(QStringLiteral("NV alone"), { 0 }, { 1000, 1000 }) << config(QStringLiteral("Intel alone"), { 1 }, { 2000, 2000 })
                       << config(QStringLiteral("All"), { 0, 1 }, { 500, 500 }, 4, {}, 2);
        analyze(failed, kWorkers);
        QVERIFY(failed.verdict.contains(QStringLiteral("some jobs failed")));
        QVERIFY(failed.notes.join(u' ').contains(QStringLiteral("boom")));

        Experiment once;
        once.configs << config(QStringLiteral("NV alone"), { 0 }, { 1000 }) << config(QStringLiteral("Intel alone"), { 1 }, { 2000 })
                     << config(QStringLiteral("All"), { 0, 1 }, { 700 });
        analyze(once, kWorkers);
        QVERIFY(once.verdict.startsWith(QStringLiteral("Measured once")));
    }
};

QTEST_GUILESS_MAIN(TestDispatch)
#include "tst_dispatch.moc"
