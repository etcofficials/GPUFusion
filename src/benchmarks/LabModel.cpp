#include "benchmarks/LabModel.h"

#include "workloads/SplitPlan.h"

#include <QJsonArray>

#include <QRegularExpression>

#include <algorithm>

namespace gf::bench {

namespace {

bool usable(const BenchmarkResult& r)
{
    return r.status == Status::Completed && r.duration.n > 0 && r.correct.value_or(false);
}

QStringList workersOf(const BenchmarkResult& r)
{
    QStringList out;
    for (const QJsonValue& v : r.params.value(QLatin1StringView("workers")).toArray())
        out << v.toString();
    return out;
}

QString ms(double v)
{
    return QString::number(v, 'f', v < 10 ? 2 : 1);
}

double metricMean(const BenchmarkResult& r, const QString& key)
{
    return r.metrics.value(key).mean;
}

// From the runner's NVML evidence ("<GPU>: performance state P8 at the start of the measured runs,
// P8 at the end"): say when a GPU was below full speed for a whole mode, because that is part of
// the mode's measured cost (e.g. a GPU left mostly idle waiting for a slower partner clocks down).
void addPowerStateFindings(const Session& session, QStringList& findings)
{
    static const QRegularExpression re(QStringLiteral("^(.+): performance state P(\\d+) at the start of the measured runs, P(\\d+) at the end$"));
    for (const BenchmarkResult& r : session.results) {
        if (!usable(r))
            continue;
        for (const QString& note : r.notes) {
            const QRegularExpressionMatch m = re.match(note);
            // Only GPUs that worked in this mode (an idle GPU in a low-power state is expected).
            if (m.hasMatch() && r.target.name.contains(m.captured(1)) && m.captured(2) != QLatin1StringView("0") && m.captured(3) != QLatin1StringView("0"))
                findings << QStringLiteral("During “%1”, %2 stayed in performance state P%3 (P0 is full speed).").arg(r.name, m.captured(1), m.captured(3));
        }
    }
}

} // namespace

QString describeDifference(const Difference& d)
{
    switch (d.verdict) {
    case Difference::Verdict::Faster:
        return QStringLiteral("%1× faster").arg(d.speedup, 0, 'f', 2);
    case Difference::Verdict::Slower:
        return QStringLiteral("%1× slower").arg(1.0 / d.speedup, 0, 'f', 2);
    case Difference::Verdict::NoSignificantChange:
        break;
    }
    return QStringLiteral("no significant difference");
}

LabAnalysis analyzeSplit(const Session& session)
{
    LabAnalysis a;
    // Reference: the fastest correct single GPU (what you get without combining anything).
    QHash<QString, double> singleMs; // worker label -> mean time for the whole image alone
    for (const BenchmarkResult& r : session.results) {
        if (r.testId != QLatin1StringView("lab.split.single") || !usable(r))
            continue;
        const QStringList w = workersOf(r);
        if (w.size() == 1)
            singleMs.insert(w.first(), r.duration.mean);
        if (r.target.vendorId && (!a.reference || r.duration.mean < a.reference->duration.mean))
            a.reference = &r;
    }
    if (a.reference)
        a.referenceLabel = a.reference->name;

    const LabRow* best = nullptr;
    for (const BenchmarkResult& r : session.results) {
        LabRow row;
        row.result = &r;
        row.reference = &r == a.reference;
        const QStringList workers = workersOf(r);
        if (usable(r) && a.reference && !row.reference)
            row.vsReference = compareStats(r.duration, a.reference->duration);
        if (workers.size() > 1) {
            std::vector<double> times;
            for (const QString& w : workers)
                times.push_back(singleMs.value(w, 0));
            if (std::all_of(times.begin(), times.end(), [](double t) { return t > 0; }))
                row.idealMs = work::idealSplitMs(times);
            QStringList shares;
            for (qsizetype i = 0; i < workers.size(); ++i)
                shares << QStringLiteral("%1 %2 %").arg(workers[i]).arg(metricMean(r, QStringLiteral("w%1.rowsPercent").arg(i)), 0, 'f', 0);
            if (usable(r))
                row.detail = shares.join(QStringLiteral(" · "));
        } else if (usable(r) && r.metrics.contains(QStringLiteral("w0.readbackMs"))) {
            row.detail = QStringLiteral("includes %1 ms copying the image to CPU memory").arg(ms(metricMean(r, QStringLiteral("w0.readbackMs"))));
        }
        a.rows << row;
    }
    for (const LabRow& row : a.rows) {
        if (row.vsReference && workersOf(*row.result).size() > 1 && (!best || row.result->duration.mean < best->result->duration.mean))
            best = &row;
    }

    if (!a.reference) {
        a.headline = QStringLiteral("No GPU produced a correct image on its own, so there is nothing to compare with.");
        return a;
    }
    if (!best) {
        a.headline = QStringLiteral("No combination of workers completed correctly, so combining could not be evaluated.");
        return a;
    }
    const BenchmarkResult& b = *best->result;
    const BenchmarkResult& ref = *a.reference;
    a.verdict = best->vsReference->verdict;
    switch (best->vsReference->verdict) {
    case Difference::Verdict::Faster:
        a.headline = QStringLiteral("Combining helped: “%1” took %2 ms, %3× faster than %4 (%5 ms).")
                         .arg(b.name, ms(b.duration.mean)).arg(best->vsReference->speedup, 0, 'f', 2).arg(ref.name, ms(ref.duration.mean));
        break;
    case Difference::Verdict::NoSignificantChange:
        a.headline = QStringLiteral("Combining did not help: the best combination (“%1”, %2 ms) was not significantly faster than %3 (%4 ms).")
                         .arg(b.name, ms(b.duration.mean), ref.name, ms(ref.duration.mean));
        break;
    case Difference::Verdict::Slower:
        a.headline = QStringLiteral("Combining made it slower: even the best combination (“%1”, %2 ms) took longer than %3 (%4 ms).")
                         .arg(b.name, ms(b.duration.mean), ref.name, ms(ref.duration.mean));
        break;
    }
    if (best->idealMs)
        a.findings << QStringLiteral("A perfect split between the same workers, with no overhead at all, would take %1 ms (%2× faster than %3); "
                                     "“%4” reached %5 % of that ideal.")
                          .arg(ms(*best->idealMs)).arg(ref.duration.mean / *best->idealMs, 0, 'f', 2).arg(ref.name, b.name)
                          .arg(100.0 * *best->idealMs / b.duration.mean, 0, 'f', 0);
    for (const LabRow& row : a.rows) {
        if (&row == best || !row.vsReference || workersOf(*row.result).size() < 2)
            continue;
        if (row.vsReference->verdict == Difference::Verdict::Slower)
            a.findings << QStringLiteral("“%1” was %2 than %3 alone (%4 ms vs %5 ms).")
                              .arg(row.result->name, describeDifference(*row.vsReference), ref.target.name, ms(row.result->duration.mean), ms(ref.duration.mean));
    }
    for (const BenchmarkResult& r : session.results) {
        if (r.status == Status::Failed)
            a.findings << QStringLiteral("“%1” could not run: %2").arg(r.name, r.error);
        else if (r.correct.has_value() && !*r.correct)
            a.findings << QStringLiteral("“%1” produced a wrong image; its time is not used.").arg(r.name);
    }
    addPowerStateFindings(session, a.findings);
    return a;
}

QString labKind(const Session& session)
{
    for (const BenchmarkResult& r : session.results) {
        if (r.testId.startsWith(QLatin1StringView("lab.decode")))
            return QStringLiteral("decode");
        if (r.testId.startsWith(QLatin1StringView("lab.adaptive")))
            return QStringLiteral("adaptive");
    }
    return QStringLiteral("split");
}

namespace {
bool isPolicyRow(const BenchmarkResult& r)
{
    return r.testId.startsWith(QLatin1StringView("lab.adaptive.")) && r.testId != QLatin1StringView("lab.adaptive.fallback");
}
} // namespace

LabAnalysis analyzeAdaptive(const Session& session)
{
    LabAnalysis a;
    QHash<QString, double> singleMs;
    for (const BenchmarkResult& r : session.results) {
        if (r.testId != QLatin1StringView("lab.split.single") || !usable(r))
            continue;
        const QStringList w = workersOf(r);
        if (w.size() == 1)
            singleMs.insert(w.first(), r.duration.mean);
        if (r.target.vendorId && (!a.reference || r.duration.mean < a.reference->duration.mean))
            a.reference = &r;
    }
    if (a.reference)
        a.referenceLabel = a.reference->name;

    for (const BenchmarkResult& r : session.results) {
        LabRow row;
        row.result = &r;
        row.reference = &r == a.reference;
        const QStringList workers = workersOf(r);
        if (usable(r) && a.reference && !row.reference)
            row.vsReference = compareStats(r.duration, a.reference->duration);
        if (workers.size() > 1) {
            std::vector<double> times;
            for (const QString& w : workers)
                times.push_back(singleMs.value(w, 0));
            if (std::all_of(times.begin(), times.end(), [](double t) { return t > 0; }))
                row.idealMs = work::idealSplitMs(times);
            if (usable(r)) {
                QStringList parts;
                for (qsizetype i = 0; i < workers.size(); ++i)
                    parts << QStringLiteral("%1 %2 %").arg(workers[i]).arg(metricMean(r, QStringLiteral("w%1.rowsPercent").arg(i)), 0, 'f', 1);
                if (isPolicyRow(r)) {
                    parts << QStringLiteral("helper used in %1 % of runs").arg(100.0 * metricMean(r, QStringLiteral("split")), 0, 'f', 0);
                    // Runs where the scheduler measured the primary alone (its baseline) are part of the
                    // result; the split runs on their own are shown too.
                    QList<double> splitRuns;
                    for (const RunSample& sample : r.samples)
                        if (sample.metrics.value(QStringLiteral("split")) > 0)
                            splitRuns << sample.durationMs;
                    if (!splitRuns.isEmpty() && splitRuns.size() < r.samples.size())
                        parts << QStringLiteral("split runs alone %1 ms").arg(ms(computeStats(splitRuns).mean));
                }
                else
                    parts << QStringLiteral("%1 rows re-run").arg(metricMean(r, QStringLiteral("w1.rerunRows")), 0, 'f', 0);
                parts << QStringLiteral("overhead %1 ms").arg(ms(metricMean(r, QStringLiteral("overheadMs"))));
                row.detail = parts.join(QStringLiteral(" · "));
            }
        } else if (usable(r) && r.metrics.contains(QStringLiteral("w0.readbackMs"))) {
            row.detail = QStringLiteral("includes %1 ms copying the image to CPU memory").arg(ms(metricMean(r, QStringLiteral("w0.readbackMs"))));
        }
        a.rows << row;
    }
    const LabRow* best = nullptr;
    for (const LabRow& row : a.rows) {
        if (row.vsReference && isPolicyRow(*row.result) && (!best || row.result->duration.mean < best->result->duration.mean))
            best = &row;
    }

    if (!a.reference) {
        a.headline = QStringLiteral("No GPU produced a correct image on its own, so there is nothing to compare with.");
        return a;
    }
    if (!best) {
        a.headline = QStringLiteral("No adaptive run completed correctly, so the scheduler could not be evaluated.");
        return a;
    }
    const BenchmarkResult& b = *best->result;
    const BenchmarkResult& ref = *a.reference;
    const QStringList bw = workersOf(b);
    const QString helperName = bw.size() > 1 ? bw[1] : QStringLiteral("the helper");
    a.verdict = best->vsReference->verdict;
    switch (best->vsReference->verdict) {
    case Difference::Verdict::Faster:
        a.headline = QStringLiteral("The adaptive scheduler helped: “%1” took %2 ms, %3× faster than %4 (%5 ms); %6 computed %7 % of the rows.")
                         .arg(b.name, ms(b.duration.mean)).arg(best->vsReference->speedup, 0, 'f', 2).arg(ref.name, ms(ref.duration.mean), helperName)
                         .arg(metricMean(b, QStringLiteral("w1.rowsPercent")), 0, 'f', 1);
        break;
    case Difference::Verdict::NoSignificantChange:
        a.headline = QStringLiteral("The adaptive scheduler did not measurably help: the best policy (“%1”, %2 ms) was not significantly faster than %3 (%4 ms).")
                         .arg(b.name, ms(b.duration.mean), ref.name, ms(ref.duration.mean));
        break;
    case Difference::Verdict::Slower:
        a.headline = QStringLiteral("The adaptive scheduler was slower: even the best policy (“%1”, %2 ms) took longer than %3 (%4 ms).")
                         .arg(b.name, ms(b.duration.mean), ref.name, ms(ref.duration.mean));
        break;
    }
    if (best->idealMs)
        a.findings << QStringLiteral("A perfect split between the same GPUs with no overhead would take %1 ms; “%2” reached %3 % of that ideal.")
                          .arg(ms(*best->idealMs), b.name).arg(100.0 * *best->idealMs / b.duration.mean, 0, 'f', 0);
    for (const LabRow& row : a.rows) {
        const BenchmarkResult& r = *row.result;
        if (!r.testId.startsWith(QLatin1StringView("lab.adaptive.")) || !usable(r))
            continue;
        const QString vs = row.vsReference ? describeDifference(*row.vsReference) : QStringLiteral("not compared");
        if (!isPolicyRow(r)) {
            a.findings << QStringLiteral("Fallback check: every batch of %1 failed on purpose; %2 re-ran %3 rows per run and the image was correct "
                                         "in every run (%4 ms; compared with %6 alone: %5).")
                              .arg(helperName, workersOf(r).value(0)).arg(metricMean(r, QStringLiteral("w1.rerunRows")), 0, 'f', 0)
                              .arg(ms(r.duration.mean), vs, ref.target.name);
            continue;
        }
        QString line = QStringLiteral("“%1”: %2 ms (%3 than %4 alone); %5 did %6 % of the rows on average; the scheduler's own time was %7 ms per run")
                           .arg(r.name, ms(r.duration.mean), vs, ref.target.name, helperName)
                           .arg(metricMean(r, QStringLiteral("w1.rowsPercent")), 0, 'f', 1).arg(metricMean(r, QStringLiteral("scheduleMs")), 0, 'f', 3);
        if (r.cpuPercent.isOk())
            line += QStringLiteral(", GPU Fusion's CPU use %1 % of all cores").arg(r.cpuPercent.value, 0, 'f', 1);
        a.findings << line + u'.';
        for (const QString& note : r.notes) {
            if (note.contains(QLatin1StringView("after the last run")))
                a.findings << QStringLiteral("  %1").arg(note);
        }
    }
    for (const BenchmarkResult& r : session.results) {
        if (r.status == Status::Failed)
            a.findings << QStringLiteral("“%1” could not run: %2").arg(r.name, r.error);
        else if (r.correct.has_value() && !*r.correct)
            a.findings << QStringLiteral("“%1” produced a wrong image; its time is not used.").arg(r.name);
    }
    addPowerStateFindings(session, a.findings);
    return a;
}

LabAnalysis analyzeDecode(const Session& session)
{
    LabAnalysis a;
    QList<const BenchmarkResult*> one;
    const BenchmarkResult* split = nullptr;
    for (const BenchmarkResult& r : session.results) {
        if (!usable(r))
            continue;
        if (r.testId == QLatin1StringView("lab.decode.one"))
            one << &r;
        else if (r.testId == QLatin1StringView("lab.decode.two-same") && (!a.reference || r.duration.mean < a.reference->duration.mean))
            a.reference = &r;
        else if (r.testId == QLatin1StringView("lab.decode.two-split"))
            split = &r;
    }
    if (a.reference)
        a.referenceLabel = a.reference->name;

    for (const BenchmarkResult& r : session.results) {
        LabRow row;
        row.result = &r;
        row.reference = &r == a.reference;
        if (usable(r)) {
            row.detail = QStringLiteral("%1 frames/s").arg(metricMean(r, QStringLiteral("fps")), 0, 'f', 0);
            if (a.reference && !row.reference && r.testId != QLatin1StringView("lab.decode.one"))
                row.vsReference = compareStats(r.duration, a.reference->duration);
            if (r.testId == QLatin1StringView("lab.decode.two-split")) {
                const QStringList gpus = r.target.name.split(QStringLiteral(" + "));
                if (gpus.size() == 2)
                    row.detail += QStringLiteral(" · %1 finished its stream in %2 ms, %3 in %4 ms")
                                      .arg(gpus[0], ms(metricMean(r, QStringLiteral("s0.ms"))), gpus[1], ms(metricMean(r, QStringLiteral("s1.ms"))));
            }
        }
        a.rows << row;
    }

    std::sort(one.begin(), one.end(), [](const BenchmarkResult* x, const BenchmarkResult* y) { return x->duration.mean < y->duration.mean; });
    if (!one.isEmpty()) {
        QStringList parts;
        for (const BenchmarkResult* r : one)
            parts << QStringLiteral("%1 %2 frames/s").arg(r->target.vendorId ? r->target.name : QStringLiteral("software (CPU)"))
                         .arg(metricMean(*r, QStringLiteral("fps")), 0, 'f', 0);
        a.findings << QStringLiteral("One stream at a time: %1.").arg(parts.join(QStringLiteral(", ")));
    }

    if (!a.reference) {
        a.headline = QStringLiteral("No device decoded two streams correctly, so there is nothing to compare with.");
    } else if (!split) {
        a.headline = QStringLiteral("Decoding one stream per GPU needs two working GPU decoders; the fastest way to decode two streams here was “%1” (%2 ms).")
                         .arg(a.reference->name, ms(a.reference->duration.mean));
    } else {
        const Difference d = compareStats(split->duration, a.reference->duration);
        a.verdict = d.verdict;
        const QString refText = QStringLiteral("%1 (%2 ms)").arg(a.reference->name, ms(a.reference->duration.mean));
        switch (d.verdict) {
        case Difference::Verdict::Faster:
            a.headline = QStringLiteral("Using both GPUs helped: one stream on each GPU finished in %1 ms, %2× faster than %3.")
                             .arg(ms(split->duration.mean)).arg(d.speedup, 0, 'f', 2).arg(refText);
            break;
        case Difference::Verdict::NoSignificantChange:
            a.headline = QStringLiteral("Using both GPUs did not help: one stream on each GPU (%1 ms) was not significantly faster than %2.")
                             .arg(ms(split->duration.mean), refText);
            break;
        case Difference::Verdict::Slower:
            a.headline = QStringLiteral("Using both GPUs was slower: one stream on each GPU took %1 ms, %2 than %3 — the slower GPU sets the pace.")
                             .arg(ms(split->duration.mean), describeDifference(d), refText);
            break;
        }
    }
    for (const BenchmarkResult& r : session.results) {
        if (r.status == Status::Failed)
            a.findings << QStringLiteral("“%1” could not run: %2").arg(r.name, r.error);
        else if (r.correct.has_value() && !*r.correct)
            a.findings << QStringLiteral("“%1” produced frames that differ from the software decoder; its time is not used.").arg(r.name);
        for (const QString& w : r.warnings) {
            if (w.contains(QLatin1StringView("fallen back")) && !a.findings.contains(w))
                a.findings << w;
        }
    }
    addPowerStateFindings(session, a.findings);
    return a;
}

} // namespace gf::bench
