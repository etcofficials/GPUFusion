#include "benchmarks/BenchmarkModel.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QLocale>

#include <algorithm>
#include <cmath>

namespace gf::bench {

Stats computeStats(QList<double> v)
{
    Stats s;
    s.n = int(v.size());
    if (v.isEmpty())
        return s;
    std::sort(v.begin(), v.end());
    s.min = v.first();
    s.max = v.last();
    s.median = v.size() % 2 ? v[v.size() / 2] : (v[v.size() / 2 - 1] + v[v.size() / 2]) / 2.0;
    double sum = 0;
    for (double x : v)
        sum += x;
    s.mean = sum / double(v.size());
    if (v.size() > 1) {
        double sq = 0;
        for (double x : v)
            sq += (x - s.mean) * (x - s.mean);
        s.stddev = std::sqrt(sq / double(v.size() - 1));
    }
    return s;
}

QString Target::key() const
{
    return QStringLiteral("%1|%2:%3:%4|%5").arg(name).arg(vendorId, 4, 16, QLatin1Char('0')).arg(deviceId, 4, 16, QLatin1Char('0'))
        .arg(subSysId, 8, 16, QLatin1Char('0')).arg(driverVersion);
}

QString statusName(Status s)
{
    switch (s) {
    case Status::Completed: return QStringLiteral("Completed");
    case Status::Failed:    return QStringLiteral("Failed");
    case Status::Skipped:   return QStringLiteral("Skipped");
    case Status::Cancelled: return QStringLiteral("Cancelled");
    }
    return {};
}

QString BenchmarkResult::comparableKey() const
{
    // Canonical parameter text: QJsonDocument sorts object keys.
    return QStringLiteral("%1|v%2|%3|%4").arg(testId).arg(testVersion)
        .arg(QString::fromUtf8(QJsonDocument(params).toJson(QJsonDocument::Compact)), target.key());
}

void summarize(BenchmarkResult& r)
{
    QList<double> durations;
    QMap<QString, QList<double>> metricValues;
    bool allCorrect = true, anyChecked = false;
    for (const RunSample& s : r.samples) {
        durations << s.durationMs;
        for (auto it = s.metrics.cbegin(); it != s.metrics.cend(); ++it)
            metricValues[it.key()] << it.value();
        allCorrect = allCorrect && s.correct;
        anyChecked = true;
    }
    r.duration = computeStats(durations);
    r.metrics.clear();
    for (auto it = metricValues.cbegin(); it != metricValues.cend(); ++it)
        r.metrics.insert(it.key(), computeStats(it.value()));
    if (anyChecked && r.correctness != QLatin1StringView("Not checked"))
        r.correct = allCorrect;
}

// ---- JSON ---------------------------------------------------------------------------------------

namespace {

QJsonObject statsJson(const Stats& s)
{
    return QJsonObject{ { "n", s.n }, { "mean", s.mean }, { "stddev", s.stddev }, { "min", s.min }, { "max", s.max }, { "median", s.median } };
}

Stats statsFrom(const QJsonObject& o)
{
    Stats s;
    s.n = o.value(QLatin1StringView("n")).toInt();
    s.mean = o.value(QLatin1StringView("mean")).toDouble();
    s.stddev = o.value(QLatin1StringView("stddev")).toDouble();
    s.min = o.value(QLatin1StringView("min")).toDouble();
    s.max = o.value(QLatin1StringView("max")).toDouble();
    s.median = o.value(QLatin1StringView("median")).toDouble();
    return s;
}

QJsonValue metricJson(const mon::MetricValue& v)
{
    if (!v.isOk())
        return QJsonObject{ { "state", mon::stateName(v.state) }, { "note", v.note } };
    return v.value;
}

mon::MetricValue metricFrom(const QJsonValue& v)
{
    if (v.isDouble())
        return mon::MetricValue::ok(v.toDouble());
    const QJsonObject o = v.toObject();
    const QString state = o.value(QLatin1StringView("state")).toString();
    const QString note = o.value(QLatin1StringView("note")).toString();
    if (state == QLatin1StringView("Unsupported"))
        return mon::MetricValue::unsupported(note);
    if (state == QLatin1StringView("Unavailable"))
        return mon::MetricValue::unavailable(note);
    return mon::MetricValue::unknown(note);
}

QJsonObject targetJson(const Target& t)
{
    return QJsonObject{ { "name", t.name }, { "vendorId", qint64(t.vendorId) }, { "deviceId", qint64(t.deviceId) },
                        { "subSysId", qint64(t.subSysId) }, { "driverVersion", t.driverVersion } };
}

Target targetFrom(const QJsonObject& o)
{
    return Target{ o.value(QLatin1StringView("name")).toString(), quint32(o.value(QLatin1StringView("vendorId")).toInteger()),
                   quint32(o.value(QLatin1StringView("deviceId")).toInteger()), quint32(o.value(QLatin1StringView("subSysId")).toInteger()),
                   o.value(QLatin1StringView("driverVersion")).toString() };
}

Status statusFrom(const QString& s)
{
    for (Status st : { Status::Completed, Status::Failed, Status::Skipped, Status::Cancelled }) {
        if (statusName(st) == s)
            return st;
    }
    return Status::Failed;
}

} // namespace

QJsonObject BenchmarkResult::toJson() const
{
    QJsonArray samplesJson;
    for (const RunSample& s : samples) {
        QJsonObject m;
        for (auto it = s.metrics.cbegin(); it != s.metrics.cend(); ++it)
            m.insert(it.key(), it.value());
        samplesJson << QJsonObject{ { "durationMs", s.durationMs }, { "metrics", m }, { "correct", s.correct }, { "note", s.note } };
    }
    QJsonObject metricsJson;
    for (auto it = metrics.cbegin(); it != metrics.cend(); ++it)
        metricsJson.insert(it.key(), statsJson(it.value()));
    return QJsonObject{
        { "testId", testId }, { "testVersion", testVersion }, { "name", name }, { "category", category }, { "measures", measures },
        { "target", targetJson(target) }, { "api", api }, { "params", params },
        { "warmupRuns", warmupRuns }, { "measuredRuns", measuredRuns }, { "samples", samplesJson },
        { "duration", statsJson(duration) }, { "metrics", metricsJson },
        { "correct", correct ? QJsonValue(*correct) : QJsonValue(QJsonValue::Null) }, { "correctness", correctness },
        { "cpuPercent", metricJson(cpuPercent) }, { "gpuBusyPercent", metricJson(gpuBusyPercent) },
        { "peakWorkingSetMB", metricJson(peakWorkingSetMB) }, { "gpuDedicatedMB", metricJson(gpuDedicatedMB) },
        { "warnings", QJsonArray::fromStringList(warnings) }, { "notes", QJsonArray::fromStringList(notes) }, { "error", error }, { "status", statusName(status) },
        { "started", started.toString(Qt::ISODateWithMs) }, { "totalMs", totalMs },
    };
}

std::optional<BenchmarkResult> BenchmarkResult::fromJson(const QJsonObject& o)
{
    if (!o.value(QLatin1StringView("testId")).isString())
        return std::nullopt;
    BenchmarkResult r;
    r.testId = o.value(QLatin1StringView("testId")).toString();
    r.testVersion = o.value(QLatin1StringView("testVersion")).toInt(1);
    r.name = o.value(QLatin1StringView("name")).toString();
    r.category = o.value(QLatin1StringView("category")).toString();
    r.measures = o.value(QLatin1StringView("measures")).toString();
    r.target = targetFrom(o.value(QLatin1StringView("target")).toObject());
    r.api = o.value(QLatin1StringView("api")).toString();
    r.params = o.value(QLatin1StringView("params")).toObject();
    r.warmupRuns = o.value(QLatin1StringView("warmupRuns")).toInt();
    r.measuredRuns = o.value(QLatin1StringView("measuredRuns")).toInt();
    for (const QJsonValue& v : o.value(QLatin1StringView("samples")).toArray()) {
        const QJsonObject s = v.toObject();
        RunSample rs;
        rs.durationMs = s.value(QLatin1StringView("durationMs")).toDouble();
        const QJsonObject m = s.value(QLatin1StringView("metrics")).toObject();
        for (auto it = m.begin(); it != m.end(); ++it)
            rs.metrics.insert(it.key(), it.value().toDouble());
        rs.correct = s.value(QLatin1StringView("correct")).toBool(true);
        rs.note = s.value(QLatin1StringView("note")).toString();
        r.samples << rs;
    }
    r.duration = statsFrom(o.value(QLatin1StringView("duration")).toObject());
    const QJsonObject m = o.value(QLatin1StringView("metrics")).toObject();
    for (auto it = m.begin(); it != m.end(); ++it)
        r.metrics.insert(it.key(), statsFrom(it.value().toObject()));
    const QJsonValue c = o.value(QLatin1StringView("correct"));
    if (c.isBool())
        r.correct = c.toBool();
    r.correctness = o.value(QLatin1StringView("correctness")).toString();
    r.cpuPercent = metricFrom(o.value(QLatin1StringView("cpuPercent")));
    r.gpuBusyPercent = metricFrom(o.value(QLatin1StringView("gpuBusyPercent")));
    r.peakWorkingSetMB = metricFrom(o.value(QLatin1StringView("peakWorkingSetMB")));
    r.gpuDedicatedMB = metricFrom(o.value(QLatin1StringView("gpuDedicatedMB")));
    for (const QJsonValue& w : o.value(QLatin1StringView("warnings")).toArray())
        r.warnings << w.toString();
    for (const QJsonValue& n : o.value(QLatin1StringView("notes")).toArray())
        r.notes << n.toString();
    r.error = o.value(QLatin1StringView("error")).toString();
    r.status = statusFrom(o.value(QLatin1StringView("status")).toString());
    r.started = QDateTime::fromString(o.value(QLatin1StringView("started")).toString(), Qt::ISODateWithMs);
    r.totalMs = o.value(QLatin1StringView("totalMs")).toDouble();
    return r;
}

QJsonObject Session::toJson() const
{
    QJsonArray rs;
    for (const BenchmarkResult& r : results)
        rs << r.toJson();
    return QJsonObject{ { "schemaVersion", 1 }, { "id", id }, { "started", started.toString(Qt::ISODateWithMs) }, { "appVersion", appVersion },
                        { "os", os }, { "cpu", cpu }, { "ramGB", ramGB }, { "adapters", QJsonArray::fromStringList(adapters) }, { "results", rs } };
}

std::optional<Session> Session::fromJson(const QJsonObject& o)
{
    if (o.value(QLatin1StringView("schemaVersion")).toInt() != 1 || !o.value(QLatin1StringView("id")).isString())
        return std::nullopt;
    Session s;
    s.id = o.value(QLatin1StringView("id")).toString();
    s.started = QDateTime::fromString(o.value(QLatin1StringView("started")).toString(), Qt::ISODateWithMs);
    s.appVersion = o.value(QLatin1StringView("appVersion")).toString();
    s.os = o.value(QLatin1StringView("os")).toString();
    s.cpu = o.value(QLatin1StringView("cpu")).toString();
    s.ramGB = o.value(QLatin1StringView("ramGB")).toDouble();
    for (const QJsonValue& a : o.value(QLatin1StringView("adapters")).toArray())
        s.adapters << a.toString();
    for (const QJsonValue& v : o.value(QLatin1StringView("results")).toArray()) {
        if (auto r = BenchmarkResult::fromJson(v.toObject()))
            s.results << *r;
    }
    return s;
}

// ---- comparison ---------------------------------------------------------------------------------

std::optional<std::pair<BenchmarkResult, QDateTime>> findBaseline(const BenchmarkResult& r, const QList<Session>& history,
                                                                   const QString& excludeSessionId)
{
    std::optional<std::pair<BenchmarkResult, QDateTime>> best;
    const QString key = r.comparableKey();
    for (const Session& s : history) {
        if (s.id == excludeSessionId || (r.started.isValid() && s.started >= r.started)) // only earlier sessions
            continue;
        for (const BenchmarkResult& b : s.results) {
            if (b.status != Status::Completed || b.duration.n == 0 || b.comparableKey() != key || (b.correct.has_value() && !*b.correct))
                continue;
            if (!best || s.started > best->second)
                best = std::make_pair(b, s.started);
        }
    }
    return best;
}

Difference compareStats(const Stats& a, const Stats& b)
{
    Difference d;
    if (a.n == 0 || b.n == 0 || a.mean <= 0 || b.mean <= 0)
        return d;
    d.speedup = b.mean / a.mean;
    d.deltaPercent = 100.0 * (a.mean - b.mean) / b.mean;
    // Welch-style test: the difference must exceed ~2 standard errors of the difference, and 1 %.
    const double se = std::sqrt(a.stddev * a.stddev / a.n + b.stddev * b.stddev / b.n);
    const double diff = a.mean - b.mean;
    if (std::abs(diff) <= 2.0 * se || std::abs(d.deltaPercent) < 1.0)
        d.verdict = Difference::Verdict::NoSignificantChange;
    else
        d.verdict = diff < 0 ? Difference::Verdict::Faster : Difference::Verdict::Slower;
    return d;
}

Comparison compare(const BenchmarkResult& current, const std::optional<std::pair<BenchmarkResult, QDateTime>>& baseline)
{
    Comparison c;
    if (!baseline || current.duration.n == 0 || baseline->first.duration.mean <= 0) {
        c.text = QStringLiteral("No earlier comparable result");
        return c;
    }
    c.baselineTime = baseline->second;
    const Difference d = compareStats(current.duration, baseline->first.duration);
    c.deltaPercent = d.deltaPercent;
    const QString when = baseline->second.toString(QStringLiteral("yyyy-MM-dd HH:mm"));
    if (d.verdict == Difference::Verdict::NoSignificantChange) {
        c.verdict = Comparison::Verdict::NoSignificantChange;
        c.text = QStringLiteral("No significant change vs %1 (%2%3 %)").arg(when, d.deltaPercent >= 0 ? QStringLiteral("+") : QString())
                     .arg(c.deltaPercent, 0, 'f', 1);
    } else if (d.verdict == Difference::Verdict::Faster) {
        c.verdict = Comparison::Verdict::Faster;
        c.text = QStringLiteral("%1 % faster than %2").arg(-c.deltaPercent, 0, 'f', 1).arg(when);
    } else {
        c.verdict = Comparison::Verdict::Slower;
        c.text = QStringLiteral("%1 % slower than %2").arg(c.deltaPercent, 0, 'f', 1).arg(when);
    }
    return c;
}

// ---- CSV ------------------------------------------------------------------------------------------

QString csvField(const QString& value)
{
    if (value.contains(u',') || value.contains(u'"') || value.contains(u'\n') || value.contains(u'\r')) {
        QString v = value;
        v.replace(u'"', QStringLiteral("\"\""));
        return u'"' + v + u'"';
    }
    return value;
}

QString csvHeader()
{
    return QStringLiteral("session,started,test_id,test_version,name,target,driver,api,params,status,correct,"
                          "runs,mean_ms,stddev_ms,min_ms,max_ms,median_ms,cpu_percent,gpu_busy_percent\n");
}

QString csvRows(const Session& s)
{
    const QLocale c = QLocale::c();
    auto num = [&](double v) { return c.toString(v, 'f', 3); };
    auto metric = [&](const mon::MetricValue& v) { return v.isOk() ? c.toString(v.value, 'f', 1) : QString(); };
    QString out;
    for (const BenchmarkResult& r : s.results) {
        const QStringList fields{
            s.id, r.started.toString(Qt::ISODate), r.testId, QString::number(r.testVersion), r.name, r.target.name, r.target.driverVersion,
            r.api, QString::fromUtf8(QJsonDocument(r.params).toJson(QJsonDocument::Compact)), statusName(r.status),
            r.correct ? (*r.correct ? QStringLiteral("yes") : QStringLiteral("no")) : QStringLiteral("not checked"),
            QString::number(r.duration.n), num(r.duration.mean), num(r.duration.stddev), num(r.duration.min), num(r.duration.max),
            num(r.duration.median), metric(r.cpuPercent), metric(r.gpuBusyPercent) };
        QStringList escaped;
        for (const QString& f : fields)
            escaped << csvField(f);
        out += escaped.join(u',') + u'\n';
    }
    return out;
}

QStringList sessionSummary(const Session& s)
{
    struct Entry { QString label; const BenchmarkResult* r; };
    QMap<QString, QList<Entry>> groups;
    QMap<QString, QString> titles;
    for (const BenchmarkResult& r : s.results) {
        if (r.status != Status::Completed || r.duration.n == 0 || (r.correct.has_value() && !*r.correct))
            continue;
        QJsonObject p = r.params;
        QString key, title;
        if (r.testId == QLatin1StringView("gpu.hash") || r.testId.startsWith(QLatin1StringView("cpu.hash"))) {
            p.remove(QLatin1StringView("threads"));
            p.remove(QLatin1StringView("bandRows")); // GPU submission detail; the computed image is the same
            key = QStringLiteral("hash|%1|").arg(r.testVersion);
            title = QStringLiteral("Hash workload (%1×%2, %3 iterations)").arg(p.value(QLatin1StringView("width")).toInteger())
                        .arg(p.value(QLatin1StringView("height")).toInteger()).arg(p.value(QLatin1StringView("iterations")).toInteger());
        } else if (r.testId == QLatin1StringView("gpu.transfer")) {
            key = QStringLiteral("transfer|%1|").arg(r.testVersion);
            title = QStringLiteral("Upload + read-back of %1 MB").arg(p.value(QLatin1StringView("bytes")).toInteger() >> 20);
        } else {
            continue;
        }
        key += QString::fromUtf8(QJsonDocument(p).toJson(QJsonDocument::Compact));
        groups[key] << Entry{ r.target.vendorId ? r.target.name : r.api, &r };
        titles[key] = title;
    }
    QStringList out;
    for (auto it = groups.begin(); it != groups.end(); ++it) {
        QList<Entry> e = it.value();
        if (e.size() < 2)
            continue;
        std::sort(e.begin(), e.end(), [](const Entry& a, const Entry& b) { return a.r->duration.mean < b.r->duration.mean; });
        const Stats& best = e.first().r->duration;
        QStringList parts{ QStringLiteral("%1 %2 ms (fastest)").arg(e.first().label).arg(best.mean, 0, 'f', 2) };
        for (qsizetype i = 1; i < e.size(); ++i) {
            const Stats& d = e[i].r->duration;
            const double se = std::sqrt(best.stddev * best.stddev / std::max(1, best.n) + d.stddev * d.stddev / std::max(1, d.n));
            const QString rel = (d.mean - best.mean) > 2 * se && d.mean > best.mean * 1.01
                ? QStringLiteral("%1× as long").arg(d.mean / best.mean, 0, 'f', 1)
                : QStringLiteral("about the same");
            parts << QStringLiteral("%1 %2 ms (%3)").arg(e[i].label).arg(d.mean, 0, 'f', 2).arg(rel);
        }
        out << QStringLiteral("%1: %2.").arg(titles.value(it.key()), parts.join(QStringLiteral("; ")));
    }
    return out;
}

QStringList filesToPrune(const QList<std::pair<QString, QDateTime>>& files, const QDateTime& now, int retentionDays)
{
    QStringList out;
    if (retentionDays <= 0)
        return out;
    const QDateTime cutoff = now.addDays(-retentionDays);
    for (const auto& [path, time] : files) {
        if (time.isValid() && time < cutoff)
            out << path;
    }
    return out;
}

} // namespace gf::bench
