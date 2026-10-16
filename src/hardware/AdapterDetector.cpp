#include "hardware/AdapterDetector.h"

#include "core/Logging.h"
#include "platform/windows/DxgiProbe.h"
#include "platform/windows/PnpDevices.h"

#include <QElapsedTimer>

namespace gf::hw {

QString summarize(const Adapter& a)
{
    QStringList parts;
    parts << QStringLiteral("\"%1\"").arg(a.name());
    switch (a.classification.value) {
    case AdapterClass::Discrete:   parts << QStringLiteral("discrete"); break;
    case AdapterClass::Integrated: parts << QStringLiteral("integrated"); break;
    case AdapterClass::Software:   parts << QStringLiteral("software"); break;
    case AdapterClass::Unknown:    parts << QStringLiteral("class unknown"); break;
    }
    if (a.dxgi) {
        const DxgiAdapter& d = *a.dxgi;
        parts << QStringLiteral("DXGI #%1 LUID %2").arg(d.index).arg(formatLuid(d.luid));
        if (!a.isSoftware()) {
            parts << QStringLiteral("%1:%2").arg(d.vendorId, 4, 16, QLatin1Char('0')).arg(d.deviceId, 4, 16, QLatin1Char('0'));
            parts << QStringLiteral("WDDM %1").arg(formatWddm(d.wddmVersion));
            parts << (d.d3d11.device.ok() ? QStringLiteral("D3D11 FL %1").arg(d.d3d11.device.featureLevel) : QStringLiteral("D3D11 failed"));
            parts << (d.d3d12.device.ok() ? QStringLiteral("D3D12 FL %1").arg(d.d3d12.maxFeatureLevel) : QStringLiteral("no D3D12"));
            parts << QStringLiteral("%1 dedicated").arg(formatBytes(d.dedicatedVideoMemory));
            if (d.capsReused)
                parts << QStringLiteral("probes reused");
        }
    } else {
        parts << QStringLiteral("not available to DirectX");
    }
    if (a.pnp) {
        parts << (a.pnp->present ? (a.pnp->hasProblem ? QStringLiteral("problem code %1").arg(a.pnp->problemCode) : QStringLiteral("device OK"))
                                 : QStringLiteral("not present"));
        if (!a.pnp->driverVersion.isEmpty())
            parts << QStringLiteral("driver %1").arg(a.pnp->driverVersion);
    }
    return parts.join(QStringLiteral(", "));
}

DetectionResult detectAdapters(const DetectionResult* previous)
{
    QElapsedTimer timer;
    timer.start();

    QList<DxgiAdapter> previousDxgi;
    if (previous) {
        for (const Adapter& a : previous->adapters) {
            if (a.dxgi)
                previousDxgi.append(*a.dxgi);
        }
    }

    const win::DxgiEnumeration dxgi = win::enumerateDxgi(previous ? &previousDxgi : nullptr);
    const win::PnpEnumeration pnp = win::enumerateDisplayDevices();

    DetectionResult result = mergeAdapters(dxgi.adapters, pnp.devices);
    result.errors << dxgi.errors << pnp.errors;
    const QDate today = QDate::currentDate();
    for (Adapter& a : result.adapters)
        a.findings = findingsFor(a, today);
    result.durationMs = timer.elapsed();
    result.finishedAt = QDateTime::currentDateTime();

    qCInfo(lcHardware).noquote() << QStringLiteral("Detection finished in %1 ms: %2 hardware adapter(s), %3 software, %4 Device-Manager-only, %5 stale entr%6 hidden")
                                        .arg(result.durationMs).arg(result.hardwareCount()).arg(result.softwareCount())
                                        .arg(std::count_if(result.adapters.begin(), result.adapters.end(), [](const Adapter& a) { return !a.dxgi; }))
                                        .arg(result.staleDeviceEntries).arg(result.staleDeviceEntries == 1 ? QStringLiteral("y") : QStringLiteral("ies"));
    for (const Adapter& a : result.adapters) {
        qCInfo(lcHardware).noquote() << "  " << summarize(a);
        for (const Finding& f : a.findings) {
            if (f.severity == Finding::Severity::Error)
                qCWarning(lcHardware).noquote() << "    error:" << f.title << "-" << f.detail;
        }
    }
    for (const QString& e : result.errors)
        qCWarning(lcHardware).noquote() << e;
    return result;
}

} // namespace gf::hw
