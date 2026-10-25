#include "dispatch/JobDispatcher.h"

#include "core/Logging.h"
#include "workloads/VideoDecode.h"

#include <QCryptographicHash>
#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QProcessEnvironment>

#include <mutex>
#include <thread>

namespace gf::dispatch {

// ---- workers + jobs -------------------------------------------------------------------------------

QList<Worker> workersFrom(const hw::DetectionResult& detection)
{
    QList<Worker> out;
    for (const hw::Adapter& a : detection.adapters) {
        if (!a.dxgi || a.isSoftware())
            continue;
        out << Worker{ a.name(), a.dxgi->vendorId, a.dxgi->luid, a.dxgi->index, a.classification.value == hw::AdapterClass::Integrated };
    }
    std::sort(out.begin(), out.end(), [](const Worker& x, const Worker& y) { return x.dxgiIndex < y.dxgiIndex; });
    return out;
}

std::optional<QList<int>> selectWorkers(const QList<Worker>& workers, const QString& spec, QString* error)
{
    QList<int> out;
    const QString s = spec.trimmed().toLower();
    for (int i = 0; i < workers.size(); ++i) {
        if (s.isEmpty() || s == QLatin1StringView("all") || (s == QLatin1StringView("integrated") && workers[i].integrated) ||
            (s == QLatin1StringView("discrete") && !workers[i].integrated))
            out << i;
    }
    if (!s.isEmpty() && s != QLatin1StringView("all") && s != QLatin1StringView("integrated") && s != QLatin1StringView("discrete")) {
        out.clear();
        for (const QString& part : s.split(u',', Qt::SkipEmptyParts)) {
            bool ok = false;
            const int idx = part.trimmed().toInt(&ok);
            int found = -1;
            for (int i = 0; i < workers.size(); ++i) {
                if (ok && workers[i].dxgiIndex == idx)
                    found = i;
            }
            if (found < 0) {
                if (error) *error = QStringLiteral("No GPU with DXGI index \"%1\".").arg(part.trimmed());
                return std::nullopt;
            }
            if (!out.contains(found))
                out << found;
        }
    }
    if (out.isEmpty()) {
        if (error) *error = QStringLiteral("No GPU matches \"%1\".").arg(spec);
        return std::nullopt;
    }
    return out;
}

QStringList inputFiles(const QString& folderOrFiles)
{
    const QFileInfo fi(folderOrFiles);
    if (fi.isDir()) {
        QStringList out;
        for (const QFileInfo& f : QDir(folderOrFiles).entryInfoList(QDir::Files, QDir::Name))
            out << QDir::toNativeSeparators(f.absoluteFilePath());
        return out;
    }
    QStringList out;
    for (const QString& f : folderOrFiles.split(u';', Qt::SkipEmptyParts))
        out << QDir::toNativeSeparators(QFileInfo(f.trimmed()).absoluteFilePath());
    return out;
}

QList<Job> jobsFor(const QStringList& inputs, const QString& outputDir, const QString& outputPattern)
{
    QList<Job> jobs;
    for (int i = 0; i < inputs.size(); ++i) {
        Job j;
        j.index = i;
        j.input = inputs[i];
        if (!outputPattern.isEmpty()) {
            const QFileInfo in(inputs[i]);
            QString name = outputPattern;
            name.replace(QLatin1StringView("{stem}"), in.completeBaseName())
                .replace(QLatin1StringView("{name}"), in.fileName())
                .replace(QLatin1StringView("{ext}"), in.suffix())
                .replace(QLatin1StringView("{job}"), QString::number(i));
            j.output = QDir::toNativeSeparators(QDir(outputDir).filePath(name));
        }
        jobs << j;
    }
    return jobs;
}

int RunResult::failed() const
{
    return int(std::count_if(jobs.begin(), jobs.end(), [](const JobResult& r) { return !r.ok; }));
}

QMap<int, int> RunResult::jobsPerWorker() const
{
    QMap<int, int> m;
    for (const JobResult& r : jobs)
        if (r.worker >= 0)
            ++m[r.worker];
    return m;
}

bool shouldTakeJob(double myMs, double fastestOtherMs, int remaining)
{
    // Not taking it: the faster worker does all `remaining` jobs one after another.
    return myMs <= 0 || fastestOtherMs <= 0 || myMs <= remaining * fastestOtherMs;
}

RunResult runJobs(const QList<Worker>& workers, const QList<int>& use, const QList<Job>& jobs, const Executor& executor,
                  const std::atomic<bool>* cancel, const Progress& progress, const QMap<int, double>& expectedJobMs)
{
    RunResult run;
    run.workers = use;
    run.jobs.resize(jobs.size());
    int next = 0;            // guarded by `mutex`
    int done = 0;
    QMap<int, QPair<double, int>> measured; // worker -> (total ms, jobs) in this run
    std::mutex mutex;
    auto msPerJob = [&](int w) -> double { // caller holds `mutex`
        const auto m = measured.value(w);
        return m.second > 0 ? m.first / m.second : expectedJobMs.value(w, 0);
    };
    QElapsedTimer timer;
    timer.start();
    std::vector<std::thread> threads;
    for (int w : use) {
        threads.emplace_back([&, w] {
            for (;;) {
                if (cancel && cancel->load())
                    return;
                int j = -1;
                {
                    std::lock_guard lock(mutex);
                    if (next >= jobs.size())
                        return;
                    double fastestOther = 0;
                    for (int o : use)
                        if (o != w && msPerJob(o) > 0 && (fastestOther <= 0 || msPerJob(o) < fastestOther))
                            fastestOther = msPerJob(o);
                    if (!shouldTakeJob(msPerJob(w), fastestOther, int(jobs.size()) - next))
                        return; // remaining jobs only get fewer: this worker is done
                    j = next++;
                }
                QElapsedTimer t;
                t.start();
                JobResult r = executor(jobs[j], workers[w], cancel);
                r.job = j;
                r.worker = w;
                const double elapsed = double(t.nsecsElapsed()) / 1e6;
                if (r.ms <= 0)
                    r.ms = elapsed;
                std::lock_guard lock(mutex);
                run.jobs[j] = r;
                if (r.ok) {
                    measured[w].first += elapsed;
                    measured[w].second += 1;
                }
                ++done;
                if (progress)
                    progress(done, int(jobs.size()));
            }
        });
    }
    for (std::thread& t : threads)
        t.join();
    run.wallMs = double(timer.nsecsElapsed()) / 1e6;
    run.cancelled = cancel && cancel->load();
    for (int j = 0; j < run.jobs.size(); ++j) {
        if (run.jobs[j].job < 0) { // never started (cancelled)
            run.jobs[j].job = j;
            run.jobs[j].error = QStringLiteral("not run (cancelled)");
        }
    }
    return run;
}

// ---- executors --------------------------------------------------------------------------------------

QString expandPlaceholders(const QString& text, const Worker& w, const Job& j)
{
    QString s = text;
    s.replace(QLatin1StringView("{dxgi_index}"), QString::number(w.dxgiIndex))
        .replace(QLatin1StringView("{luid}"), hw::formatLuid(w.luid))
        .replace(QLatin1StringView("{vendor_id}"), QString::number(w.vendorId))
        .replace(QLatin1StringView("{vendor_hex}"), QStringLiteral("0x%1").arg(w.vendorId, 4, 16, QLatin1Char('0')))
        .replace(QLatin1StringView("{gpu_name}"), w.name)
        .replace(QLatin1StringView("{input}"), j.input)
        .replace(QLatin1StringView("{output}"), j.output)
        .replace(QLatin1StringView("{stem}"), QFileInfo(j.input).completeBaseName())
        .replace(QLatin1StringView("{job}"), QString::number(j.index));
    return s;
}

QStringList commandLine(const QString& commandTemplate, const Worker& w, const Job& j)
{
    QStringList out;
    for (const QString& part : QProcess::splitCommand(commandTemplate))
        out << expandPlaceholders(part, w, j);
    return out;
}

namespace {

std::optional<QString> fileSha1(const QString& path)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
        return std::nullopt;
    QCryptographicHash h(QCryptographicHash::Sha1);
    if (!h.addData(&f))
        return std::nullopt;
    return QString::fromLatin1(h.result().toHex());
}

} // namespace

Executor commandExecutor(const QString& commandTemplate, int timeoutMs)
{
    return [commandTemplate, timeoutMs](const Job& job, const Worker& w, const std::atomic<bool>* cancel) {
        JobResult r;
        const QStringList cmd = commandLine(commandTemplate, w, job);
        if (cmd.isEmpty()) {
            r.error = QStringLiteral("empty command");
            return r;
        }
        if (!job.output.isEmpty()) {
            QDir().mkpath(QFileInfo(job.output).absolutePath());
            QFile::remove(job.output); // a stale file must not count as this job's output
        }
        QProcess p;
        QProcessEnvironment env = QProcessEnvironment::systemEnvironment();
        env.insert(QStringLiteral("GPUFUSION_MODE"), w.integrated ? QStringLiteral("integrated") : QStringLiteral("discrete"));
        env.insert(QStringLiteral("GPUFUSION_ADAPTERS"), hw::formatLuid(w.luid));
        p.setProcessEnvironment(env);
        p.setProcessChannelMode(QProcess::SeparateChannels);
        p.setStandardOutputFile(QProcess::nullDevice());
        QElapsedTimer t;
        t.start();
        p.start(cmd.first(), cmd.mid(1));
        if (!p.waitForStarted(30000)) {
            r.error = QStringLiteral("could not start %1: %2").arg(cmd.first(), p.errorString());
            return r;
        }
        QByteArray err;
        while (!p.waitForFinished(200)) {
            err += p.readAllStandardError();
            if (err.size() > 64 * 1024)
                err = err.right(16 * 1024);
            if ((cancel && cancel->load()) || t.elapsed() > timeoutMs) {
                p.kill();
                p.waitForFinished(5000);
                r.error = cancel && cancel->load() ? QStringLiteral("cancelled") : QStringLiteral("timed out");
                return r;
            }
        }
        err += p.readAllStandardError();
        r.ms = double(t.nsecsElapsed()) / 1e6;
        if (p.exitStatus() != QProcess::NormalExit || p.exitCode() != 0) {
            const QStringList lines = QString::fromLocal8Bit(err).split(u'\n', Qt::SkipEmptyParts);
            r.error = QStringLiteral("exit code %1%2").arg(p.exitCode()).arg(lines.isEmpty() ? QString() : QStringLiteral(": ") + lines.last().trimmed());
            return r;
        }
        if (!job.output.isEmpty()) {
            if (QFileInfo(job.output).size() <= 0) {
                r.error = QStringLiteral("no output written to %1").arg(job.output);
                return r;
            }
            r.checksum = fileSha1(job.output);
        }
        r.ok = true;
        return r;
    };
}

Executor decodeExecutor()
{
    return [](const Job& job, const Worker& w, const std::atomic<bool>*) {
        JobResult r;
        work::ComScope com;
        QString error;
        if (!work::startMediaFoundation(&error)) {
            r.error = error;
            return r;
        }
        auto decoder = work::VideoDecoder::open(job.input, w.luid, &error);
        if (!decoder) {
            r.error = error;
            return r;
        }
        const work::VideoDecoder::Result d = decoder->decodeAll(false);
        r.ms = d.wallMs;
        if (!d.ok) {
            r.error = d.error;
            return r;
        }
        if (!d.gpuOutput) {
            r.error = QStringLiteral("decoded in software, not on %1").arg(w.name);
            return r;
        }
        r.ok = d.frames > 0;
        r.checksum = QStringLiteral("%1 frames").arg(d.frames);
        if (!r.ok)
            r.error = QStringLiteral("no frames decoded");
        return r;
    };
}

// ---- comparison ---------------------------------------------------------------------------------------

Experiment compareConfigurations(const QList<Worker>& workers, const QList<int>& use, const QStringList& inputs,
                                 const QString& outputDir, const QString& outputPattern, const Executor& executor,
                                 int repetitions, const std::atomic<bool>* cancel, const std::function<void(const QString&)>& progress)
{
    Experiment e;
    QMap<int, double> expected; // worker -> ms per job, measured on that GPU alone
    auto measure = [&](const QList<int>& set, const QString& label, const QString& sub, const QMap<int, double>& hint = {}) {
        ConfigResult c;
        c.workers = set;
        c.label = label;
        const QList<Job> jobs = jobsFor(inputs, QDir(outputDir).filePath(sub), outputPattern);
        for (int rep = 0; rep < repetitions; ++rep) {
            if (cancel && cancel->load())
                break;
            if (progress)
                progress(QStringLiteral("%1, run %2 of %3").arg(c.label).arg(rep + 1).arg(repetitions));
            c.runs << runJobs(workers, c.workers, jobs, executor, cancel, {}, hint);
        }
        QList<double> walls;
        for (const RunResult& r : c.runs)
            if (r.failed() == 0 && !r.cancelled)
                walls << r.wallMs;
        c.wall = bench::computeStats(walls);
        c.allOk = !c.runs.isEmpty() && walls.size() == c.runs.size();
        e.configs << c;
    };
    for (int w : use) {
        measure({ w }, QStringLiteral("%1 alone").arg(workers[w].name), QStringLiteral("gpu%1").arg(workers[w].dxgiIndex));
        if (e.configs.last().allOk && !inputs.isEmpty())
            expected[w] = e.configs.last().wall.mean / inputs.size();
    }
    // Control: the fastest GPU alone running as many jobs at once as the multi-GPU run does, so that a
    // gain from running jobs concurrently (CPU work, I/O) is not credited to the second GPU.
    std::optional<int> best;
    for (int i = 0; i < e.configs.size(); ++i)
        if (e.configs[i].allOk && (!best || e.configs[i].wall.mean < e.configs[*best].wall.mean))
            best = i;
    if (best && use.size() > 1) {
        const int w = e.configs[*best].workers.first();
        measure(QList<int>(use.size(), w), QStringLiteral("%1 alone, %2 jobs at a time").arg(workers[w].name).arg(use.size()),
                QStringLiteral("gpu%1-concurrent").arg(workers[w].dxgiIndex));
    }
    measure(use, QStringLiteral("All %1 GPUs").arg(use.size()), QStringLiteral("all-gpus"), expected);
    analyze(e, workers);
    return e;
}

void analyze(Experiment& e, const QList<Worker>& workers)
{
    e.notes.clear();
    e.bestSingle.reset();
    e.difference.reset();
    e.outputsConsistent = true;
    for (ConfigResult& c : e.configs) {
        QList<double> walls;
        c.allOk = !c.runs.isEmpty();
        for (const RunResult& r : c.runs) {
            if (r.failed() == 0 && !r.cancelled)
                walls << r.wallMs;
            else
                c.allOk = false;
        }
        c.wall = bench::computeStats(walls);
        if (!c.allOk) {
            for (const RunResult& r : c.runs)
                for (const JobResult& j : r.jobs)
                    if (!j.ok) {
                        e.notes << QStringLiteral("%1: job %2 failed: %3").arg(c.label).arg(j.job).arg(j.error);
                        break;
                    }
        }
    }
    if (e.configs.size() < 2) {
        e.verdict = QStringLiteral("Needs at least two GPUs to compare.");
        return;
    }
    const int allIndex = int(e.configs.size()) - 1;
    for (int i = 0; i < allIndex; ++i) {
        if (e.configs[i].allOk && (!e.bestSingle || e.configs[i].wall.mean < e.configs[*e.bestSingle].wall.mean))
            e.bestSingle = i;
    }
    const ConfigResult& all = e.configs[allIndex];

    // Each job's output in the multi-GPU run must equal that job's output on the same GPU alone.
    QMap<int, int> singleConfigOf; // worker -> config index
    for (int i = 0; i < allIndex; ++i)
        if (e.configs[i].workers.size() == 1)
            singleConfigOf[e.configs[i].workers.first()] = i;
    int compared = 0;
    for (const RunResult& r : all.runs) {
        for (const JobResult& j : r.jobs) {
            if (!j.ok || !j.checksum || !singleConfigOf.contains(j.worker))
                continue;
            const ConfigResult& single = e.configs[singleConfigOf[j.worker]];
            for (const RunResult& sr : single.runs) {
                if (j.job < sr.jobs.size() && sr.jobs[j.job].ok && sr.jobs[j.job].checksum) {
                    ++compared;
                    if (*sr.jobs[j.job].checksum != *j.checksum) {
                        e.outputsConsistent = false;
                        e.notes << QStringLiteral("Job %1: output on %2 differs between the multi-GPU run and %2 alone.")
                                       .arg(j.job).arg(workers.value(j.worker).name);
                    }
                    break;
                }
            }
        }
    }
    if (compared == 0)
        e.notes << QStringLiteral("Outputs could not be compared (the jobs report no output identity).");

    if (!all.allOk) {
        e.verdict = QStringLiteral("Not comparable: some jobs failed when run on all GPUs.");
        return;
    }
    if (!e.bestSingle) {
        e.verdict = QStringLiteral("Not comparable: no single GPU completed every job.");
        return;
    }
    if (!e.outputsConsistent) {
        e.verdict = QStringLiteral("Not comparable: outputs differ between the multi-GPU run and the single-GPU runs.");
        return;
    }
    const ConfigResult& best = e.configs[*e.bestSingle];
    const bench::Difference d = bench::compareStats(all.wall, best.wall);
    e.difference = d;
    const QString ratio = QString::number(d.speedup, 'f', 2);
    if (all.wall.n < 2 || best.wall.n < 2) {
        e.verdict = QStringLiteral("Measured once each (no significance test): all GPUs %1 ms vs %2 %3 ms (ratio %4).")
                        .arg(all.wall.mean, 0, 'f', 0).arg(best.label).arg(best.wall.mean, 0, 'f', 0).arg(ratio);
        return;
    }
    switch (d.verdict) {
    case bench::Difference::Verdict::Faster:
        e.verdict = QStringLiteral("Faster: the batch finished %1× faster on all GPUs than in the best single-GPU configuration (%2), with "
                                   "the same outputs.").arg(ratio, best.label);
        break;
    case bench::Difference::Verdict::Slower:
        e.verdict = QStringLiteral("Slower: the batch took %1× as long on all GPUs as in the best single-GPU configuration (%2).")
                        .arg(QString::number(1.0 / d.speedup, 'f', 2), best.label);
        break;
    case bench::Difference::Verdict::NoSignificantChange:
        e.verdict = QStringLiteral("No significant difference between all GPUs and the best single-GPU configuration (%1).").arg(best.label);
        break;
    }
    QStringList perWorker;
    if (!all.runs.isEmpty()) {
        const QMap<int, int> m = all.runs.last().jobsPerWorker();
        for (auto it = m.begin(); it != m.end(); ++it)
            perWorker << QStringLiteral("%1: %2 job(s)").arg(workers.value(it.key()).name).arg(it.value());
    }
    if (!perWorker.isEmpty())
        e.notes.prepend(QStringLiteral("Last multi-GPU run: %1.").arg(perWorker.join(QStringLiteral(", "))));
}

} // namespace gf::dispatch
