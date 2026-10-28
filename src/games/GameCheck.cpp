#include "games/GameCheck.h"

#include "core/Logging.h"
#include "frames/FrameMeasure.h"
#include "monitoring/ProcessModel.h"
#include "platform/windows/KmtProcessProvider.h"
#include "platform/windows/PdhGpuProcessProvider.h"
#include "platform/windows/ProcessList.h"

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <QThread>

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <algorithm>
#include <future>

namespace gf::games {

namespace win = mon::win;

QString indicationText(Indication i)
{
    switch (i) {
    case Indication::GpuBound: return QStringLiteral("GPU-limited (indication)");
    case Indication::Contention: return QStringLiteral("Other programs use the game's GPU (indication)");
    case Indication::CpuBound: return QStringLiteral("CPU-limited (indication)");
    case Indication::NoneClear: return QStringLiteral("No clear limit (indication)");
    }
    return {};
}

namespace {
QString pct(double v) { return QString::number(v, 'f', 0) + QStringLiteral(" %"); }

QJsonObject gpuJson(const ProcessGpu& g)
{
    return { { QStringLiteral("pid"), qint64(g.pid) }, { QStringLiteral("name"), g.name }, { QStringLiteral("path"), g.path },
             { QStringLiteral("luid"), QString::number(g.luid, 16) }, { QStringLiteral("adapter"), g.adapter }, { QStringLiteral("samples"), g.samples },
             { QStringLiteral("meanPercent"), g.meanPercent }, { QStringLiteral("maxPercent"), g.maxPercent }, { QStringLiteral("busiestEngine"), g.busiestEngine } };
}
ProcessGpu gpuFrom(const QJsonObject& o)
{
    ProcessGpu g;
    g.pid = quint32(o.value(QLatin1StringView("pid")).toDouble());
    g.name = o.value(QLatin1StringView("name")).toString();
    g.path = o.value(QLatin1StringView("path")).toString();
    g.luid = o.value(QLatin1StringView("luid")).toString().toULongLong(nullptr, 16);
    g.adapter = o.value(QLatin1StringView("adapter")).toString();
    g.samples = o.value(QLatin1StringView("samples")).toInt();
    g.meanPercent = o.value(QLatin1StringView("meanPercent")).toDouble();
    g.maxPercent = o.value(QLatin1StringView("maxPercent")).toDouble();
    g.busiestEngine = o.value(QLatin1StringView("busiestEngine")).toString();
    return g;
}
} // namespace

GameCheck analyze(quint32 pid, const QString& name, const QString& path, const QList<Sample>& samples,
                  const std::function<compat::Assessment(const QString& exePath)>& assessExe)
{
    GameCheck c;
    c.pid = pid;
    c.name = name;
    c.path = path;
    c.samples = int(samples.size());
    if (samples.isEmpty()) {
        c.error = QStringLiteral("No samples were taken.");
        return c;
    }
    // Per process and adapter: mean over all samples (absent = 0), max, most frequent busiest engine.
    struct Acc {
        ProcessGpu g;
        double sum = 0;
        QHash<QString, int> engines;
    };
    QHash<QString, Acc> acc;
    for (const Sample& s : samples) {
        for (const ProcessGpu& u : s.gpu) {
            Acc& a = acc[QStringLiteral("%1:%2").arg(u.pid).arg(u.luid)];
            if (a.g.samples == 0) {
                a.g = u;
                a.g.samples = 0;
                a.g.maxPercent = 0;
            }
            ++a.g.samples;
            a.sum += u.meanPercent;
            a.g.maxPercent = std::max(a.g.maxPercent, u.meanPercent);
            if (!u.busiestEngine.isEmpty())
                ++a.engines[u.busiestEngine];
        }
    }
    for (auto it = acc.begin(); it != acc.end(); ++it) {
        Acc& a = it.value();
        a.g.meanPercent = a.sum / samples.size();
        int best = 0;
        for (auto e = a.engines.cbegin(); e != a.engines.cend(); ++e)
            if (e.value() > best) {
                best = e.value();
                a.g.busiestEngine = e.key();
            }
        if (a.g.maxPercent > 0)
            c.processes << a.g;
    }
    std::sort(c.processes.begin(), c.processes.end(), [](const ProcessGpu& a, const ProcessGpu& b) { return a.meanPercent > b.meanPercent; });

    // The game's GPU: the adapter it used most.
    for (const ProcessGpu& g : c.processes) {
        if (g.pid == pid && g.meanPercent > c.gameGpuMean) {
            c.gameGpuMean = g.meanPercent;
            c.gameGpuMax = g.maxPercent;
            c.gameLuid = g.luid;
            c.gameAdapter = g.adapter;
        }
    }
    if (c.gameLuid == 0)
        c.notes << QStringLiteral("%1 did no GPU work that Windows' counters showed during the check (minimized, paused, or rendering on the CPU?).").arg(name);

    // Competitors: other processes on the game's GPU.
    QHash<QString, compat::Assessment> cache;
    for (const ProcessGpu& g : c.processes) {
        if (g.pid == pid || g.luid != c.gameLuid || c.gameLuid == 0)
            continue;
        if (g.meanPercent < 0.5 && g.maxPercent < 2) // traces (a moment of work) are not competition
            continue;
        c.othersOnGameGpuMean += g.meanPercent;
        Competitor comp;
        comp.use = g;
        if (!g.path.isEmpty() && assessExe) {
            const QString key = g.path.toLower();
            if (!cache.contains(key))
                cache.insert(key, assessExe(g.path));
            comp.move = cache.value(key).answer(compat::Capability::MoveOffGameGpu);
            for (const QString& r : comp.move.reasons)
                if (r.startsWith(QLatin1StringView("How: ")))
                    comp.mechanism = r.mid(5).chopped(r.endsWith(u'.') ? 1 : 0);
        } else {
            comp.move.capability = compat::Capability::MoveOffGameGpu;
            comp.move.status = compat::Status::NotTested;
            comp.move.summary = QStringLiteral("Its executable could not be read (protected or system process).");
        }
        c.competitors << comp;
    }

    // CPU.
    double totalSum = 0, busiestSum = 0, gameCpuSum = 0;
    int gameCpuSamples = 0;
    for (const Sample& s : samples) {
        if (!s.corePercent.isEmpty()) {
            double sum = 0, busiest = 0;
            for (double v : s.corePercent) {
                sum += v;
                busiest = std::max(busiest, v);
            }
            totalSum += sum / s.corePercent.size();
            busiestSum += busiest;
            c.cpu.maxBusiestCorePercent = std::max(c.cpu.maxBusiestCorePercent, busiest);
            c.cpu.cores = int(s.corePercent.size());
        }
        if (s.gameCpuPercentOfOneCore >= 0) {
            gameCpuSum += s.gameCpuPercentOfOneCore;
            ++gameCpuSamples;
        }
    }
    c.cpu.meanTotalPercent = totalSum / samples.size();
    c.cpu.meanBusiestCorePercent = busiestSum / samples.size();
    c.cpu.gameCpuPercentOfOneCore = gameCpuSamples ? gameCpuSum / gameCpuSamples : -1;

    // Indications. Utilization does not prove a limit: these say where to look, and why.
    if (c.gameLuid != 0 && c.gameGpuMean >= 85)
        c.indications << std::make_pair(Indication::GpuBound,
                                        QStringLiteral("%1 kept %2's busiest engine at %3 on average (max %4): the GPU is probably what limits its frame rate.")
                                            .arg(name, c.gameAdapter, pct(c.gameGpuMean), pct(c.gameGpuMax)));
    if (c.othersOnGameGpuMean >= 5)
        c.indications << std::make_pair(Indication::Contention,
                                        QStringLiteral("Other programs used %1 of %2 on average (sum of their busiest engines). GPU work of other programs is "
                                                       "time-sliced with the game's: moving it to the other GPU can give the game that time back (measured on "
                                                       "this PC: a 3D program on the GT 730 cost a GPU-bound program 77 % more time per frame; on the Intel GPU, nothing).")
                                            .arg(pct(c.othersOnGameGpuMean), c.gameAdapter));
    const bool gameThreadBusy = c.cpu.gameCpuPercentOfOneCore >= 85;
    const bool coreBusy = c.cpu.meanBusiestCorePercent >= 90;
    if (c.gameLuid != 0 && c.gameGpuMean < 70 && (gameThreadBusy || coreBusy))
        c.indications << std::make_pair(Indication::CpuBound,
                                        QStringLiteral("The GPU was not saturated (%1) while %2: the CPU is probably what limits the frame rate.")
                                            .arg(pct(c.gameGpuMean), gameThreadBusy ? QStringLiteral("the game used %1 of one core's time").arg(pct(c.cpu.gameCpuPercentOfOneCore))
                                                                                   : QStringLiteral("the busiest core averaged %1").arg(pct(c.cpu.meanBusiestCorePercent))));
    if (c.indications.isEmpty())
        c.indications << std::make_pair(Indication::NoneClear,
                                        QStringLiteral("Neither the GPU nor a CPU core was saturated and little else used the game's GPU: the game may be limited by "
                                                       "vertical sync or a frame-rate cap, or simply not demanding."));
    return c;
}

// ---- running a check ---------------------------------------------------------------------------------

namespace {
// Per-core CPU load from Windows' performance counters (no administrator rights needed).
class CoreSampler {
public:
    CoreSampler()
    {
        if (PdhOpenQueryW(nullptr, 0, &m_query) != ERROR_SUCCESS)
            m_query = nullptr;
        else if (PdhAddEnglishCounterW(m_query, L"\\Processor(*)\\% Processor Time", 0, &m_counter) != ERROR_SUCCESS)
            m_counter = nullptr;
        if (m_query)
            PdhCollectQueryData(m_query);
    }
    ~CoreSampler()
    {
        if (m_query)
            PdhCloseQuery(m_query);
    }
    QList<double> sample()
    {
        QList<double> out;
        if (!m_query || !m_counter || PdhCollectQueryData(m_query) != ERROR_SUCCESS)
            return out;
        DWORD size = 0, count = 0;
        if (PdhGetFormattedCounterArrayW(m_counter, PDH_FMT_DOUBLE, &size, &count, nullptr) != PDH_MORE_DATA)
            return out;
        std::vector<BYTE> buf(size);
        auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buf.data());
        if (PdhGetFormattedCounterArrayW(m_counter, PDH_FMT_DOUBLE, &size, &count, items) != ERROR_SUCCESS)
            return out;
        for (DWORD i = 0; i < count; ++i)
            if (std::wstring(items[i].szName) != L"_Total" && items[i].FmtValue.CStatus == ERROR_SUCCESS)
                out << items[i].FmtValue.doubleValue;
        return out;
    }

private:
    PDH_HQUERY m_query = nullptr;
    PDH_HCOUNTER m_counter = nullptr;
};

double processCpuMs(HANDLE h)
{
    FILETIME c, e, k, u;
    if (!h || !GetProcessTimes(h, &c, &e, &k, &u))
        return -1;
    auto v = [](const FILETIME& f) { return double((quint64(f.dwHighDateTime) << 32) | f.dwLowDateTime) / 1e4; };
    return v(k) + v(u);
}
} // namespace

GameCheck runCheck(const QList<mon::MonitorTarget>& targets, const CheckOptions& o,
                   const std::function<compat::Assessment(const QString& exePath)>& assessExe, const std::atomic<bool>* cancel,
                   const CheckProgress& progress)
{
    QList<quint64> pdhLuids;
    QList<win::KmtProcessProvider::Adapter> legacy;
    QHash<quint64, QString> names;
    for (const mon::MonitorTarget& t : targets) {
        if (t.software)
            continue;
        names.insert(t.luid, t.name);
        if (t.wddmVersion >= 2000)
            pdhLuids << t.luid;
        else
            legacy << win::KmtProcessProvider::Adapter{ t.luid, t.name };
    }
    // Optional frame-time capture, in parallel (its helper runs through the administrator prompt).
    std::future<frames::FrameSession> framesFuture;
    if (o.measureFrames) {
        frames::MeasureOptions fo;
        fo.pid = o.pid;
        fo.processName = o.name;
        fo.processPath = o.path;
        fo.seconds = o.seconds;
        fo.label = o.label;
        fo.helperPath = o.helperPath;
        fo.workDir = o.workDir;
        framesFuture = std::async(std::launch::async, [targets, fo, cancel] { return frames::measureFrames(targets, fo, cancel); });
    }

    win::ProcessList list;
    win::PdhGpuProcessProvider pdh;
    win::KmtProcessProvider kmt;
    kmt.setAdapters(legacy);
    CoreSampler cores;
    HANDLE game = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, o.pid);
    double lastGameCpu = processCpuMs(game);
    const double cpu0 = processCpuMs(GetCurrentProcess());
    QList<Sample> samples;
    QStringList notes;
    QElapsedTimer clock;
    clock.start();
    qint64 lastSampleMs = 0;
    // Pass 0 primes the rate counters; passes 1..seconds are the samples.
    for (int pass = 0; pass <= o.seconds; ++pass) {
        if (cancel && cancel->load())
            break;
        const QList<win::ProcessInfo> processes = list.snapshot();
        QHash<quint32, QList<mon::ProcessAdapterUsage>> usage;
        if (!pdhLuids.isEmpty()) {
            QList<mon::PdhEngineReading> engines;
            QList<mon::PdhMemoryReading> memory;
            if (pdh.collect(&engines, &memory))
                usage = mon::aggregatePdh(engines, memory, pdhLuids);
        }
        int inspectable = 0, notInspectable = 0;
        QList<win::ProcessInfo> skipped;
        kmt.sample(processes, usage, &inspectable, &notInspectable, &skipped);
        const QList<double> core = cores.sample();
        const double gameCpu = processCpuMs(game);
        const qint64 now = clock.elapsed();
        if (pass > 0) {
            Sample s;
            s.corePercent = core;
            if (gameCpu >= 0 && lastGameCpu >= 0 && now > lastSampleMs)
                s.gameCpuPercentOfOneCore = 100.0 * (gameCpu - lastGameCpu) / double(now - lastSampleMs);
            QHash<quint32, const win::ProcessInfo*> byPid;
            for (const win::ProcessInfo& p : processes)
                byPid.insert(p.pid, &p);
            for (auto it = usage.cbegin(); it != usage.cend(); ++it) {
                const win::ProcessInfo* info = byPid.value(it.key());
                for (const mon::ProcessAdapterUsage& u : it.value()) {
                    if (u.utilization.state != mon::MetricValue::State::Ok || !names.contains(u.luid))
                        continue;
                    ProcessGpu g;
                    g.pid = it.key();
                    g.name = info ? info->name : QStringLiteral("process %1").arg(it.key());
                    g.path = info ? info->path : QString();
                    g.luid = u.luid;
                    g.adapter = names.value(u.luid);
                    g.meanPercent = u.utilization.value;
                    g.busiestEngine = u.busiestEngine;
                    s.gpu << g;
                }
            }
            samples << s;
            if (pass == 1 && notInspectable > 0)
                notes << QStringLiteral("%1 processes could not be inspected through D3DKMT (protected); their use of %2 is not included.")
                             .arg(notInspectable).arg(legacy.isEmpty() ? QStringLiteral("WDDM 1.x GPUs") : legacy.first().name);
        }
        lastGameCpu = gameCpu;
        lastSampleMs = now;
        if (progress)
            progress(QStringLiteral("Watching %1 and the programs around it…").arg(o.name), std::min(99, 100 * pass / std::max(1, o.seconds)));
        const qint64 next = qint64(pass + 1) * 1000;
        while (clock.elapsed() < next && !(cancel && cancel->load()))
            QThread::msleep(50);
    }
    if (game)
        CloseHandle(game);
    const double elapsed = double(clock.elapsed());

    GameCheck c = analyze(o.pid, o.name, o.path, samples, assessExe);
    c.id = QStringLiteral("game-") + QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss-zzz"));
    c.created = QDateTime::currentDateTime();
    c.label = o.label;
    c.seconds = o.seconds;
    c.notes << notes;
    c.checkCpuPercent = elapsed > 0 ? 100.0 * (processCpuMs(GetCurrentProcess()) - cpu0) / elapsed / std::max(1, QThread::idealThreadCount()) : -1;
    if (framesFuture.valid()) {
        c.frames = framesFuture.get();
        if (!c.frames->measured())
            c.notes << QStringLiteral("Frame times were not measured: %1").arg(c.frames->error.isEmpty() ? c.frames->capture.error : c.frames->error);
    }
    return c;
}

QList<ProcessGpu> sampleGpuUse(const QList<mon::MonitorTarget>& targets, int ms)
{
    CheckOptions o;
    o.seconds = std::max(1, (ms + 999) / 1000);
    GameCheck c = runCheck(targets, o, {});
    QHash<quint32, ProcessGpu> best;
    for (const ProcessGpu& g : c.processes)
        if (!best.contains(g.pid) || best.value(g.pid).meanPercent < g.meanPercent)
            best.insert(g.pid, g);
    QList<ProcessGpu> out = best.values();
    std::sort(out.begin(), out.end(), [](const ProcessGpu& a, const ProcessGpu& b) { return a.meanPercent > b.meanPercent; });
    return out;
}

// ---- comparison, report, storage -----------------------------------------------------------------------

QStringList compareChecks(const GameCheck& before, const GameCheck& after)
{
    QStringList out;
    if (before.frames && after.frames && before.frames->measured() && after.frames->measured()) {
        const frames::FrameComparison fc = frames::compareSessions(*before.frames, *after.frames);
        out << (fc.comparable ? fc.text : fc.reason);
    } else {
        out << QStringLiteral("Frame times: not compared (both checks must have measured them).");
    }
    out << QStringLiteral("Other programs on the game's GPU: %1 before, %2 after (estimated, sum of busiest engines).")
               .arg(pct(before.othersOnGameGpuMean), pct(after.othersOnGameGpuMean));
    out << QStringLiteral("The game's own GPU use: %1 before, %2 after (estimated).").arg(pct(before.gameGpuMean), pct(after.gameGpuMean));
    return out;
}

QStringList summaryLines(const GameCheck& c)
{
    QStringList out;
    if (!c.error.isEmpty())
        return { c.error };
    out << QStringLiteral("%1 (process %2), %3 s, %4 samples.").arg(c.name).arg(c.pid).arg(c.seconds).arg(c.samples);
    if (c.gameLuid)
        out << QStringLiteral("Estimated: it kept %1 busy at %2 on average (busiest engine, max %3).").arg(c.gameAdapter, pct(c.gameGpuMean), pct(c.gameGpuMax));
    if (c.frames && c.frames->measured())
        out << frames::summaryLines(*c.frames).value(0);
    out << QStringLiteral("CPU (estimated): %1 of all cores on average; busiest core %2 on average, %3 at most%4.")
               .arg(pct(c.cpu.meanTotalPercent), pct(c.cpu.meanBusiestCorePercent), pct(c.cpu.maxBusiestCorePercent),
                    c.cpu.gameCpuPercentOfOneCore >= 0 ? QStringLiteral("; the game used %1 of one core's time").arg(pct(c.cpu.gameCpuPercentOfOneCore)) : QString());
    for (const auto& [kind, text] : c.indications)
        out << QStringLiteral("%1: %2").arg(indicationText(kind), text);
    if (c.competitors.isEmpty() && c.gameLuid)
        out << QStringLiteral("No other program used %1 during the check.").arg(c.gameAdapter);
    for (const Competitor& k : c.competitors)
        out << QStringLiteral("Competitor: %1 used %2 of %3 on average (max %4, %5). Move it to the other GPU: %6 — %7")
                   .arg(k.use.name, pct(k.use.meanPercent), k.use.adapter, pct(k.use.maxPercent), k.use.busiestEngine.isEmpty() ? QStringLiteral("engine unknown") : k.use.busiestEngine,
                        compat::statusText(k.move.status), k.move.summary);
    for (const QString& n : c.notes)
        out << QStringLiteral("Note: %1").arg(n);
    if (c.checkCpuPercent >= 0)
        out << QStringLiteral("The check itself used %1 % of all CPU cores.").arg(c.checkCpuPercent, 0, 'f', 2);
    return out;
}

QString reportHtml(const GameCheck& c, const GameCheck* before)
{
    QString h = QStringLiteral("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\">"
                               "<title>Game check — %1</title><style>body{font:15px/1.5 system-ui,Segoe UI,sans-serif;max-width:960px;margin:2em auto;padding:0 16px;"
                               "color:#1b1f24;background:#fff}table{border-collapse:collapse}td,th{border:1px solid #ccd;padding:4px 8px;text-align:left;vertical-align:top}"
                               "th{background:#eef1f5}.m{color:#555}@media(prefers-color-scheme:dark){body{background:#16181c;color:#e6e8eb}th{background:#23272e}"
                               "td,th{border-color:#3a3f47}.m{color:#aab}}</style></head><body>")
                    .arg(c.name.toHtmlEscaped());
    h += QStringLiteral("<h1>Game check: %1</h1><p class=\"m\">%2 · process %3 · %4 s%5</p>")
             .arg(c.name.toHtmlEscaped(), c.created.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss"))).arg(c.pid).arg(c.seconds)
             .arg(c.label.isEmpty() ? QString() : QStringLiteral(" · “%1”").arg(c.label.toHtmlEscaped()));
    if (before) {
        h += QStringLiteral("<h2>Before / after</h2><ul>");
        for (const QString& l : compareChecks(*before, c))
            h += QStringLiteral("<li>%1</li>").arg(l.toHtmlEscaped());
        h += QStringLiteral("</ul>");
    }
    h += QStringLiteral("<h2>Summary</h2><ul>");
    for (const QString& l : summaryLines(c))
        if (!l.startsWith(QLatin1StringView("Competitor:")))
            h += QStringLiteral("<li>%1</li>").arg(l.toHtmlEscaped());
    h += QStringLiteral("</ul><h2>Programs using a GPU during the check</h2><table><tr><th>Program</th><th>GPU</th><th>Mean</th><th>Max</th><th>Engine</th>"
                        "<th>Can it move to the other GPU?</th></tr>");
    for (const ProcessGpu& g : c.processes) {
        QString move = g.pid == c.pid ? QStringLiteral("(the game)") : QString();
        for (const Competitor& k : c.competitors)
            if (k.use.pid == g.pid && k.use.luid == g.luid)
                move = QStringLiteral("%1 — %2%3").arg(compat::statusText(k.move.status), k.move.summary,
                                                       k.mechanism.isEmpty() ? QString() : QStringLiteral(" How: %1.").arg(k.mechanism));
        h += QStringLiteral("<tr><td>%1</td><td>%2</td><td>%3</td><td>%4</td><td>%5</td><td>%6</td></tr>")
                 .arg(g.name.toHtmlEscaped(), g.adapter.toHtmlEscaped(), pct(g.meanPercent), pct(g.maxPercent), g.busiestEngine.toHtmlEscaped(), move.toHtmlEscaped());
    }
    h += QStringLiteral("</table>");
    if (c.frames && c.frames->measured())
        h += QStringLiteral("<h2>Frame times (measured)</h2><ul>%1</ul>").arg([&] {
            QString x;
            for (const QString& l : frames::summaryLines(*c.frames))
                x += QStringLiteral("<li>%1</li>").arg(l.toHtmlEscaped());
            return x;
        }());
    h += QStringLiteral("<h2>What these numbers are</h2><ul><li><b>Measured</b>: frame times (only when the check included a frame-time capture).</li>"
                        "<li><b>Estimated</b>: GPU use per process (Windows GPU performance counters; D3DKMT statistics for WDDM 1.x GPUs; the busiest "
                        "engine, sampled once a second) and CPU load (performance counters).</li><li><b>Indications</b> are not proof: high utilization "
                        "shows where time goes, not what limits the frame rate. A before/after measurement of the game's frame times shows whether a "
                        "change helped.</li><li>Nothing was started, closed or changed by this check.</li></ul></body></html>");
    return h;
}

QJsonObject GameCheck::toJson() const
{
    QJsonArray procs, comps, inds;
    for (const ProcessGpu& g : processes)
        procs << gpuJson(g);
    for (const Competitor& k : competitors) {
        QJsonObject o = gpuJson(k.use);
        o.insert(QStringLiteral("moveStatus"), compat::statusKey(k.move.status));
        o.insert(QStringLiteral("moveSummary"), k.move.summary);
        o.insert(QStringLiteral("mechanism"), k.mechanism);
        comps << o;
    }
    for (const auto& [kind, text] : indications)
        inds << QJsonObject{ { QStringLiteral("kind"), int(kind) }, { QStringLiteral("text"), text } };
    QJsonObject o{ { QStringLiteral("format"), 1 }, { QStringLiteral("id"), id }, { QStringLiteral("created"), created.toString(Qt::ISODateWithMs) },
                   { QStringLiteral("label"), label }, { QStringLiteral("pid"), qint64(pid) }, { QStringLiteral("name"), name }, { QStringLiteral("path"), path },
                   { QStringLiteral("seconds"), seconds }, { QStringLiteral("samples"), samples }, { QStringLiteral("gameAdapter"), gameAdapter },
                   { QStringLiteral("gameLuid"), QString::number(gameLuid, 16) }, { QStringLiteral("gameGpuMean"), gameGpuMean }, { QStringLiteral("gameGpuMax"), gameGpuMax },
                   { QStringLiteral("othersOnGameGpuMean"), othersOnGameGpuMean }, { QStringLiteral("processes"), procs }, { QStringLiteral("competitors"), comps },
                   { QStringLiteral("cpu"), QJsonObject{ { QStringLiteral("cores"), cpu.cores }, { QStringLiteral("meanTotalPercent"), cpu.meanTotalPercent },
                                                          { QStringLiteral("meanBusiestCorePercent"), cpu.meanBusiestCorePercent },
                                                          { QStringLiteral("maxBusiestCorePercent"), cpu.maxBusiestCorePercent },
                                                          { QStringLiteral("gameCpuPercentOfOneCore"), cpu.gameCpuPercentOfOneCore } } },
                   { QStringLiteral("indications"), inds }, { QStringLiteral("notes"), QJsonArray::fromStringList(notes) },
                   { QStringLiteral("checkCpuPercent"), checkCpuPercent }, { QStringLiteral("error"), error } };
    if (frames)
        o.insert(QStringLiteral("frames"), frames->toJson());
    return o;
}

std::optional<GameCheck> GameCheck::fromJson(const QJsonObject& o)
{
    if (o.value(QLatin1StringView("format")).toInt() != 1 || o.value(QLatin1StringView("id")).toString().isEmpty())
        return std::nullopt;
    GameCheck c;
    c.id = o.value(QLatin1StringView("id")).toString();
    c.created = QDateTime::fromString(o.value(QLatin1StringView("created")).toString(), Qt::ISODateWithMs);
    c.label = o.value(QLatin1StringView("label")).toString();
    c.pid = quint32(o.value(QLatin1StringView("pid")).toDouble());
    c.name = o.value(QLatin1StringView("name")).toString();
    c.path = o.value(QLatin1StringView("path")).toString();
    c.seconds = o.value(QLatin1StringView("seconds")).toInt();
    c.samples = o.value(QLatin1StringView("samples")).toInt();
    c.gameAdapter = o.value(QLatin1StringView("gameAdapter")).toString();
    c.gameLuid = o.value(QLatin1StringView("gameLuid")).toString().toULongLong(nullptr, 16);
    c.gameGpuMean = o.value(QLatin1StringView("gameGpuMean")).toDouble();
    c.gameGpuMax = o.value(QLatin1StringView("gameGpuMax")).toDouble();
    c.othersOnGameGpuMean = o.value(QLatin1StringView("othersOnGameGpuMean")).toDouble();
    for (const QJsonValue& v : o.value(QLatin1StringView("processes")).toArray())
        c.processes << gpuFrom(v.toObject());
    for (const QJsonValue& v : o.value(QLatin1StringView("competitors")).toArray()) {
        Competitor k;
        k.use = gpuFrom(v.toObject());
        k.move.capability = compat::Capability::MoveOffGameGpu;
        k.move.status = compat::statusFromKey(v.toObject().value(QLatin1StringView("moveStatus")).toString()).value_or(compat::Status::NotTested);
        k.move.summary = v.toObject().value(QLatin1StringView("moveSummary")).toString();
        k.mechanism = v.toObject().value(QLatin1StringView("mechanism")).toString();
        c.competitors << k;
    }
    const QJsonObject cpu = o.value(QLatin1StringView("cpu")).toObject();
    c.cpu.cores = cpu.value(QLatin1StringView("cores")).toInt();
    c.cpu.meanTotalPercent = cpu.value(QLatin1StringView("meanTotalPercent")).toDouble();
    c.cpu.meanBusiestCorePercent = cpu.value(QLatin1StringView("meanBusiestCorePercent")).toDouble();
    c.cpu.maxBusiestCorePercent = cpu.value(QLatin1StringView("maxBusiestCorePercent")).toDouble();
    c.cpu.gameCpuPercentOfOneCore = cpu.value(QLatin1StringView("gameCpuPercentOfOneCore")).toDouble(-1);
    for (const QJsonValue& v : o.value(QLatin1StringView("indications")).toArray())
        c.indications << std::make_pair(Indication(v.toObject().value(QLatin1StringView("kind")).toInt()), v.toObject().value(QLatin1StringView("text")).toString());
    for (const QJsonValue& v : o.value(QLatin1StringView("notes")).toArray())
        c.notes << v.toString();
    c.checkCpuPercent = o.value(QLatin1StringView("checkCpuPercent")).toDouble(-1);
    c.error = o.value(QLatin1StringView("error")).toString();
    if (o.contains(QLatin1StringView("frames")))
        c.frames = frames::FrameSession::fromJson(o.value(QLatin1StringView("frames")).toObject());
    return c;
}

bool saveCheck(const QString& dir, const GameCheck& c, QString* error)
{
    QDir().mkpath(dir);
    QSaveFile f(QDir(dir).filePath(c.id + QStringLiteral(".json")));
    if (!f.open(QIODevice::WriteOnly) || f.write(QJsonDocument(c.toJson()).toJson(QJsonDocument::Compact)) < 0 || !f.commit()) {
        if (error)
            *error = f.errorString();
        return false;
    }
    return true;
}

QList<GameCheck> loadChecks(const QString& dir)
{
    QList<GameCheck> out;
    for (const QString& n : QDir(dir).entryList({ QStringLiteral("game-*.json") }, QDir::Files, QDir::Name | QDir::Reversed)) {
        QFile f(QDir(dir).filePath(n));
        if (!f.open(QIODevice::ReadOnly))
            continue;
        if (auto c = GameCheck::fromJson(QJsonDocument::fromJson(f.readAll()).object()))
            out << *c;
    }
    return out;
}

} // namespace gf::games
