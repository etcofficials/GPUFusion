#include "platform/windows/KmtProcessProvider.h"

#include <QSet>

#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>

namespace gf::mon::win {

namespace {

constexpr NTSTATUS kStatusInvalidParameter = NTSTATUS(0xC000000DL);

LUID toLuid(quint64 v)
{
    LUID l;
    l.HighPart = LONG(v >> 32);
    l.LowPart = DWORD(v & 0xFFFFFFFFu);
    return l;
}

qint64 now100ns()
{
    LARGE_INTEGER c, f;
    QueryPerformanceCounter(&c);
    QueryPerformanceFrequency(&f);
    return qint64(double(c.QuadPart) * 1e7 / double(f.QuadPart));
}

} // namespace

void KmtProcessProvider::setAdapters(const QList<Adapter>& adapters)
{
    QHash<quint64, AdapterState> next;
    for (const Adapter& a : adapters) {
        AdapterState s = m_adapters.value(a.luid);
        s.name = a.name;
        next.insert(a.luid, s);
    }
    m_adapters = std::move(next);
    m_baselines.clear();
    m_sampled = false;
}

bool KmtProcessProvider::prepare(quint64 luid, AdapterState& st)
{
    D3DKMT_QUERYSTATISTICS q{};
    q.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
    q.AdapterLuid = toLuid(luid);
    if (D3DKMTQueryStatistics(&q) != 0)
        return false;
    QStringList names;
    for (ULONG n = 0; n < q.QueryResult.AdapterInformation.NodeCount; ++n)
        names << QStringLiteral("Engine %1").arg(n); // WDDM 1.x drivers do not name engines
    st.engineNames = names;
    st.segmentAperture.clear();
    for (ULONG s = 0; s < q.QueryResult.AdapterInformation.NbSegments; ++s) {
        D3DKMT_QUERYSTATISTICS qs{};
        qs.Type = D3DKMT_QUERYSTATISTICS_SEGMENT;
        qs.AdapterLuid = toLuid(luid);
        qs.QuerySegment.SegmentId = s;
        st.segmentAperture.push_back(D3DKMTQueryStatistics(&qs) == 0 && qs.QueryResult.SegmentInformation.Aperture != 0);
    }
    st.ready = true;
    return true;
}

void KmtProcessProvider::sample(const QList<ProcessInfo>& processes, QHash<quint32, QList<ProcessAdapterUsage>>& out,
                                int* inspectable, int* notInspectable, QList<ProcessInfo>* skipped)
{
    *inspectable = *notInspectable = 0;
    if (m_adapters.isEmpty())
        return;
    for (auto it = m_adapters.begin(); it != m_adapters.end(); ++it) {
        if (!it->ready)
            prepare(it.key(), *it);
    }

    QSet<QString> alive;
    for (const ProcessInfo& p : processes) {
        HANDLE h = OpenProcess(PROCESS_QUERY_INFORMATION, FALSE, p.pid);
        if (!h) // sandboxed processes (e.g. a browser's GPU process) often grant only the limited right
            h = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, p.pid);
        if (!h) {
            ++*notInspectable;
            if (skipped)
                *skipped << p;
            continue;
        }
        ++*inspectable;
        const qint64 now = now100ns();
        for (auto it = m_adapters.cbegin(); it != m_adapters.cend(); ++it) {
            const AdapterState& st = *it;
            if (!st.ready)
                continue;
            const QString key = QStringLiteral("%1:%2:%3").arg(p.pid).arg(p.creationTime).arg(it.key());
            std::vector<quint64> running;
            bool hasContext = true;
            for (int node = 0; node < st.engineNames.size(); ++node) {
                D3DKMT_QUERYSTATISTICS q{};
                q.Type = D3DKMT_QUERYSTATISTICS_PROCESS_NODE;
                q.AdapterLuid = toLuid(it.key());
                q.hProcess = h;
                q.QueryProcessNode.NodeId = ULONG(node);
                const NTSTATUS s = D3DKMTQueryStatistics(&q);
                if (s == kStatusInvalidParameter && node == 0) { // the process has never used this adapter
                    hasContext = false;
                    break;
                }
                running.push_back(s == 0 ? quint64(q.QueryResult.ProcessNodeInformation.RunningTime.QuadPart) : 0);
            }
            if (!hasContext)
                continue;
            alive.insert(key);

            ProcessAdapterUsage u;
            u.luid = it.key();
            u.source = QStringLiteral("D3DKMT");
            const auto prev = m_baselines.constFind(key);
            QList<MetricValue> values;
            for (int node = 0; node < int(running.size()); ++node) {
                std::optional<quint64> before;
                if (prev != m_baselines.constEnd() && size_t(node) < prev->running.size())
                    before = prev->running[size_t(node)];
                const MetricValue v = utilizationFromRunningTime(before, running[size_t(node)],
                                                                 prev != m_baselines.constEnd() ? now - prev->time100ns : 0);
                u.engines << EngineSample{ node, st.engineNames[node], v };
                values << v;
            }
            u.utilization = busiestEngine(values);
            double best = kActiveThresholdPercent;
            for (const EngineSample& e : u.engines) {
                if (e.utilization.isOk() && e.utilization.value >= best) {
                    best = e.utilization.value;
                    u.busiestEngine = e.name;
                }
            }
            m_baselines.insert(key, { running, now });

            quint64 local = 0, aperture = 0;
            bool memOk = true;
            for (int s = 0; s < int(st.segmentAperture.size()); ++s) {
                D3DKMT_QUERYSTATISTICS q{};
                q.Type = D3DKMT_QUERYSTATISTICS_PROCESS_SEGMENT;
                q.AdapterLuid = toLuid(it.key());
                q.hProcess = h;
                q.QuerySegment.SegmentId = ULONG(s);
                if (D3DKMTQueryStatistics(&q) != 0) {
                    memOk = false;
                    continue;
                }
                (st.segmentAperture[size_t(s)] ? aperture : local) += q.QueryResult.ProcessSegmentInformation.BytesCommitted;
            }
            const QString memSource = QStringLiteral("D3DKMT committed bytes");
            u.dedicated = memOk ? MetricValue::ok(double(local), memSource) : MetricValue::unavailable(QStringLiteral("Segment query failed"));
            u.shared = memOk ? MetricValue::ok(double(aperture), memSource) : MetricValue::unavailable(QStringLiteral("Segment query failed"));
            out[p.pid] << u;
        }
        CloseHandle(h);
    }
    m_sampled = true;
    // Drop baselines of processes that exited or released the adapter.
    for (auto it = m_baselines.begin(); it != m_baselines.end();)
        it = alive.contains(it.key()) ? std::next(it) : m_baselines.erase(it);
}

} // namespace gf::mon::win
