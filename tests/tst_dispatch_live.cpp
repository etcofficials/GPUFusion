// Live check of the dispatcher on this PC's GPUs: a small synthetic H.264 clip (generated with Media
// Foundation's software encoder) is decoded by the built-in executor on each GPU and on all of them.
#include "dispatch/JobDispatcher.h"
#include "hardware/AdapterDetector.h"
#include "workloads/VideoDecode.h"

#include <QTemporaryDir>
#include <QTest>

using namespace gf;
using namespace gf::dispatch;

class TestDispatchLive : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void decodeJobsRunOnEveryGpu()
    {
        const QList<Worker> workers = workersFrom(hw::detectAdapters(nullptr));
        if (workers.isEmpty())
            QSKIP("No physical GPU");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        work::ComScope com;
        QString error;
        QVERIFY2(work::startMediaFoundation(&error), qPrintable(error));
        work::TestClipSpec spec;
        spec.width = 320;
        spec.height = 240;
        spec.frames = 60;
        spec.bitrate = 500'000;
        const QString clip = work::ensureTestClip(dir.path(), spec, &error);
        QVERIFY2(!clip.isEmpty(), qPrintable(error));

        const Executor decode = decodeExecutor();
        const QList<Job> jobs = jobsFor({ clip, clip, clip, clip }, {}, {});
        for (int w = 0; w < workers.size(); ++w) {
            const RunResult r = runJobs(workers, { w }, jobs, decode);
            for (const JobResult& j : r.jobs) {
                if (!j.ok && j.error.contains(QStringLiteral("software")))
                    QSKIP(qPrintable(QStringLiteral("%1 has no hardware H.264 decoding: %2").arg(workers[w].name, j.error)));
                QVERIFY2(j.ok, qPrintable(QStringLiteral("%1: %2").arg(workers[w].name, j.error)));
                QCOMPARE(*j.checksum, QStringLiteral("60 frames"));
            }
            qInfo().noquote() << workers[w].name << "decoded 4 clips in" << int(r.wallMs) << "ms";
        }
        QList<int> all;
        for (int w = 0; w < workers.size(); ++w)
            all << w;
        const RunResult r = runJobs(workers, all, jobs, decode);
        QCOMPARE(r.failed(), 0);
        qInfo().noquote() << "All GPUs:" << int(r.wallMs) << "ms," << r.jobsPerWorker();
    }
};

QTEST_GUILESS_MAIN(TestDispatchLive)
#include "tst_dispatch_live.moc"
