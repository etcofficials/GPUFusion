#include "platform/windows/KmtStatsProvider.h"

#include "core/Logging.h"

#include <windows.h>
#include <winternl.h>
#include <d3dkmthk.h>

namespace gf::mon::win {

namespace {

LUID toLuid(quint64 v)
{
    LUID l;
    l.HighPart = LONG(v >> 32);
    l.LowPart = DWORD(v & 0xFFFFFFFFu);
    return l;
}

qint64 now100ns()
{
    LARGE_INTEGER counter, frequency;
    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return qint64(double(counter.QuadPart) * 1e7 / double(frequency.QuadPart));
}

QString ntstatus(NTSTATUS s) { return QStringLiteral("NTSTATUS 0x%1").arg(quint32(s), 8, 16, QLatin1Char('0')); }

// Engine names via KMTQAITYPE_NODEMETADATA (WDDM 2.0+). Empty list if the driver does not provide them.
QStringList queryEngineNames(LUID luid, int nodeCount)
{
    QStringList names;
    D3DKMT_OPENADAPTERFROMLUID open{};
    open.AdapterLuid = luid;
    if (D3DKMTOpenAdapterFromLuid(&open) != 0)
        return names;
    for (int node = 0; node < nodeCount; ++node) {
        D3DKMT_NODEMETADATA meta{};
        meta.NodeOrdinalAndAdapterIndex = UINT(node); // physical adapter index 0
        D3DKMT_QUERYADAPTERINFO q{};
        q.hAdapter = open.hAdapter;
        q.Type = KMTQAITYPE_NODEMETADATA;
        q.pPrivateDriverData = &meta;
        q.PrivateDriverDataSize = sizeof(meta);
        if (D3DKMTQueryAdapterInfo(&q) != 0) {
            names.clear();
            break;
        }
        QString name = QString::fromWCharArray(meta.NodeData.FriendlyName).trimmed();
        if (name.isEmpty())
            name = engineTypeName(int(meta.NodeData.EngineType), node);
        names << name;
    }
    D3DKMT_CLOSEADAPTER close{ open.hAdapter };
    D3DKMTCloseAdapter(&close);
    return names;
}

} // namespace

void KmtStatsProvider::setTargets(const QList<MonitorTarget>& targets)
{
    QHash<quint64, State> next;
    for (const MonitorTarget& t : targets)
        next.insert(t.luid, m_states.value(t.luid)); // keep baselines of adapters that are still present
    m_states = std::move(next);
}

bool KmtStatsProvider::initialise(quint64 luid, State& state)
{
    D3DKMT_QUERYSTATISTICS q{};
    q.Type = D3DKMT_QUERYSTATISTICS_ADAPTER;
    q.AdapterLuid = toLuid(luid);
    if (const NTSTATUS s = D3DKMTQueryStatistics(&q); s != 0) {
        m_lastError = QStringLiteral("adapter query failed (%1)").arg(ntstatus(s));
        return false;
    }
    const int nodes = int(q.QueryResult.AdapterInformation.NodeCount);
    state.segmentCount = int(q.QueryResult.AdapterInformation.NbSegments);
    QStringList names = queryEngineNames(q.AdapterLuid, nodes);
    if (names.size() != nodes) {
        names.clear();
        for (int n = 0; n < nodes; ++n)
            names << QStringLiteral("Engine %1").arg(n); // driver does not name its engines (WDDM 1.x)
    }
    state.engineNames = disambiguate(names);
    state.previousRunning.assign(size_t(nodes), std::nullopt);
    state.initialised = true;
    qCInfo(lcMonitor).noquote() << QStringLiteral("D3DKMT: LUID %1 has %2 engines (%3) and %4 memory segments")
                                       .arg(hw::formatLuid(luid)).arg(nodes).arg(state.engineNames.join(QStringLiteral(", "))).arg(state.segmentCount);
    return true;
}

QList<quint64> KmtStatsProvider::sample(QList<AdapterSample>& samples)
{
    QList<quint64> failed;
    for (AdapterSample& out : samples) {
        auto it = m_states.find(out.luid);
        if (it == m_states.end())
            continue;
        State& st = *it;
        const LUID luid = toLuid(out.luid);
        auto failAll = [&](const QString& why) {
            const MetricValue v = MetricValue::unavailable(why);
            out.utilization = out.dedicatedUsed = out.sharedUsed = v;
            failed << out.luid;
            st = State{}; // re-initialise next time
        };
        if (!st.initialised && !initialise(out.luid, st)) {
            failAll(QStringLiteral("D3DKMT: %1").arg(m_lastError));
            continue;
        }

        // Engines
        const qint64 now = now100ns();
        const qint64 elapsed = st.previousQpc100ns ? now - st.previousQpc100ns : 0;
        QList<MetricValue> engineValues;
        bool ok = true;
        for (int node = 0; node < st.engineNames.size(); ++node) {
            D3DKMT_QUERYSTATISTICS q{};
            q.Type = D3DKMT_QUERYSTATISTICS_NODE;
            q.AdapterLuid = luid;
            q.QueryNode.NodeId = ULONG(node);
            if (const NTSTATUS s = D3DKMTQueryStatistics(&q); s != 0) {
                failAll(QStringLiteral("D3DKMT engine query failed (%1); the adapter may have been removed or reset").arg(ntstatus(s)));
                ok = false;
                break;
            }
            const quint64 running = quint64(q.QueryResult.NodeInformation.GlobalInformation.RunningTime.QuadPart);
            MetricValue v = utilizationFromRunningTime(st.previousRunning[size_t(node)], running, elapsed);
            st.previousRunning[size_t(node)] = running;
            out.engines << EngineSample{ node, st.engineNames[node], v };
            engineValues << v;
        }
        if (!ok)
            continue;
        st.previousQpc100ns = now;
        out.utilization = busiestEngine(engineValues);

        // Memory segments
        QList<SegmentUsage> segments;
        for (int s = 0; s < st.segmentCount; ++s) {
            D3DKMT_QUERYSTATISTICS q{};
            q.Type = D3DKMT_QUERYSTATISTICS_SEGMENT;
            q.AdapterLuid = luid;
            q.QuerySegment.SegmentId = ULONG(s);
            if (D3DKMTQueryStatistics(&q) != 0) {
                segments.clear();
                break;
            }
            const auto& info = q.QueryResult.SegmentInformation;
            segments << SegmentUsage{ info.Aperture != 0, quint64(info.BytesResident) };
        }
        if (segments.isEmpty()) {
            out.dedicatedUsed = out.sharedUsed = MetricValue::unavailable(QStringLiteral("D3DKMT segment query failed"));
        } else {
            const MemoryUsage m = splitSegments(segments);
            out.dedicatedUsed = MetricValue::ok(double(m.dedicated), QStringLiteral("D3DKMT resident bytes in local segments"));
            out.sharedUsed = MetricValue::ok(double(m.shared), QStringLiteral("D3DKMT resident bytes in aperture segments"));
        }
        out.sources << QStringLiteral("D3DKMT");
    }
    return failed;
}

ProviderStatus KmtStatsProvider::status() const
{
    ProviderStatus s;
    s.name = QStringLiteral("D3DKMT statistics");
    int ready = 0;
    for (const State& st : m_states)
        ready += st.initialised ? 1 : 0;
    s.active = ready > 0;
    s.detail = QStringLiteral("Kernel graphics statistics (gdi32). Engine utilization and memory for %1 of %2 adapter(s); works with WDDM 1.x drivers.")
                   .arg(ready).arg(m_states.size());
    if (!m_lastError.isEmpty() && ready < m_states.size())
        s.detail += QStringLiteral(" Last error: %1").arg(m_lastError);
    return s;
}

} // namespace gf::mon::win
