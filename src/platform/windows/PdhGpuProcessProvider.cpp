#include "platform/windows/PdhGpuProcessProvider.h"

#include <QHash>

#include <windows.h>
#include <pdh.h>
#include <pdhmsg.h>

#include <vector>

namespace gf::mon::win {

namespace {

QString pdhError(PDH_STATUS s) { return QStringLiteral("PDH status 0x%1").arg(quint32(s), 8, 16, QLatin1Char('0')); }

// Formatted values of a wildcard counter: (instance name, value or state).
QList<std::pair<QString, MetricValue>> readArray(PDH_HCOUNTER counter, DWORD format)
{
    QList<std::pair<QString, MetricValue>> out;
    DWORD bytes = 0, count = 0;
    if (PdhGetFormattedCounterArrayW(counter, format, &bytes, &count, nullptr) != PDH_MORE_DATA)
        return out;
    std::vector<BYTE> buffer(bytes);
    auto* items = reinterpret_cast<PDH_FMT_COUNTERVALUE_ITEM_W*>(buffer.data());
    if (PdhGetFormattedCounterArrayW(counter, format, &bytes, &count, items) != ERROR_SUCCESS)
        return out;
    out.reserve(qsizetype(count));
    for (DWORD i = 0; i < count; ++i) {
        const auto& v = items[i].FmtValue;
        MetricValue m;
        if (v.CStatus == PDH_CSTATUS_VALID_DATA || v.CStatus == PDH_CSTATUS_NEW_DATA)
            m = MetricValue::ok(format == PDH_FMT_LARGE ? double(v.largeValue) : v.doubleValue, QStringLiteral("Windows performance counters"));
        else
            m = MetricValue::unknown(QStringLiteral("Counter not ready (new process or first sample)"));
        out.append({ QString::fromWCharArray(items[i].szName), m });
    }
    return out;
}

} // namespace

PdhGpuProcessProvider::PdhGpuProcessProvider()
{
    PDH_HQUERY query = nullptr;
    if (const PDH_STATUS s = PdhOpenQueryW(nullptr, 0, &query); s != ERROR_SUCCESS) {
        m_error = QStringLiteral("Could not open a performance-counter query (%1)").arg(pdhError(s));
        return;
    }
    PDH_HCOUNTER engine = nullptr, dedicated = nullptr, shared = nullptr;
    const PDH_STATUS a = PdhAddEnglishCounterW(query, L"\\GPU Engine(*)\\Utilization Percentage", 0, &engine);
    const PDH_STATUS b = PdhAddEnglishCounterW(query, L"\\GPU Process Memory(*)\\Dedicated Usage", 0, &dedicated);
    const PDH_STATUS c = PdhAddEnglishCounterW(query, L"\\GPU Process Memory(*)\\Shared Usage", 0, &shared);
    if (a != ERROR_SUCCESS || b != ERROR_SUCCESS || c != ERROR_SUCCESS) {
        m_error = QStringLiteral("Windows GPU performance counters are not available on this system (%1)")
                      .arg(pdhError(a != ERROR_SUCCESS ? a : b != ERROR_SUCCESS ? b : c));
        PdhCloseQuery(query);
        return;
    }
    m_query = query;
    m_engine = engine;
    m_dedicated = dedicated;
    m_shared = shared;
}

PdhGpuProcessProvider::~PdhGpuProcessProvider()
{
    if (m_query)
        PdhCloseQuery(static_cast<PDH_HQUERY>(m_query));
}

bool PdhGpuProcessProvider::collect(QList<PdhEngineReading>* engines, QList<PdhMemoryReading>* memory)
{
    engines->clear();
    memory->clear();
    if (!m_query)
        return false;
    if (const PDH_STATUS s = PdhCollectQueryData(static_cast<PDH_HQUERY>(m_query)); s != ERROR_SUCCESS) {
        m_error = QStringLiteral("Collecting GPU performance counters failed (%1)").arg(pdhError(s));
        return false;
    }
    m_error.clear();
    const bool primed = m_primed;
    m_primed = true;

    for (const auto& [name, value] : readArray(static_cast<PDH_HCOUNTER>(m_engine), PDH_FMT_DOUBLE)) {
        if (const auto inst = parsePdhEngineInstance(name)) {
            MetricValue v = primed ? value : MetricValue::unknown(QStringLiteral("Needs two samples"));
            if (v.isOk())
                v.value = std::clamp(v.value, 0.0, 100.0);
            engines->append({ *inst, v });
        }
    }
    QHash<QString, MetricValue> shared;
    for (const auto& [name, value] : readArray(static_cast<PDH_HCOUNTER>(m_shared), PDH_FMT_LARGE))
        shared.insert(name, value);
    for (const auto& [name, value] : readArray(static_cast<PDH_HCOUNTER>(m_dedicated), PDH_FMT_LARGE)) {
        if (const auto inst = parsePdhProcessInstance(name))
            memory->append({ *inst, value, shared.value(name, MetricValue::unavailable(QStringLiteral("No shared-memory counter"))) });
    }
    return true;
}

} // namespace gf::mon::win
