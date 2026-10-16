#include "diagnostics/DiagnosticReport.h"

#include <QDir>
#include <QJsonArray>
#include <QRegularExpression>
#include <QSet>
#include <QTextStream>

namespace gf::diag {

using hw::Adapter;
using hw::AdapterClass;
using hw::ApiProbe;
using hw::Finding;
using mon::MetricValue;

QString severityName(Issue::Severity s)
{
    switch (s) {
    case Issue::Severity::Error:   return QStringLiteral("Error");
    case Issue::Severity::Warning: return QStringLiteral("Warning");
    case Issue::Severity::Note:    return QStringLiteral("Note");
    }
    return {};
}

QString verdictName(Technique::Verdict v)
{
    switch (v) {
    case Technique::Verdict::Feasible:    return QStringLiteral("Feasible");
    case Technique::Verdict::NotFeasible: return QStringLiteral("Not feasible");
    case Technique::Verdict::Unknown:     return QStringLiteral("Unknown");
    }
    return {};
}

int Report::count(Issue::Severity s) const
{
    return int(std::count_if(issues.begin(), issues.end(), [s](const Issue& i) { return i.severity == s; }));
}

namespace {

QList<const Adapter*> physical(const hw::DetectionResult& d)
{
    QList<const Adapter*> out;
    for (const Adapter& a : d.adapters) {
        if (a.dxgi && !a.isSoftware())
            out << &a;
    }
    return out;
}

QStringList names(const QList<const Adapter*>& adapters)
{
    QStringList n;
    for (const Adapter* a : adapters)
        n << a->name();
    return n;
}

Issue::Severity fromFinding(Finding::Severity s)
{
    switch (s) {
    case Finding::Severity::Error:   return Issue::Severity::Error;
    case Finding::Severity::Warning: return Issue::Severity::Warning;
    case Finding::Severity::Info:    return Issue::Severity::Note;
    }
    return Issue::Severity::Note;
}

QStringList actionsForFinding(const Finding& f)
{
    if (f.title.startsWith(QLatin1StringView("Legacy WDDM")))
        return { QStringLiteral("Use GPU Fusion's Dashboard and Applications pages to watch this GPU; Task Manager cannot show it.") };
    if (f.title.startsWith(QLatin1StringView("Driver from")))
        return { QStringLiteral("Check the GPU vendor's website for a newer driver. If the GPU is no longer supported, this is the final driver; keep it.") };
    if (f.title.startsWith(QLatin1StringView("No vendor driver")))
        return { QStringLiteral("Install the driver from the GPU vendor's website.") };
    if (f.title.startsWith(QLatin1StringView("Direct3D 11 device creation failed")))
        return { QStringLiteral("Restart Windows; if it persists, reinstall the graphics driver.") };
    if (f.title.startsWith(QLatin1StringView("Not currently detected")))
        return { QStringLiteral("Check the BIOS/UEFI setup for an integrated-graphics or primary-display option.") };
    if (f.title.startsWith(QLatin1StringView("Device Manager reports a problem")))
        return { QStringLiteral("Open Device Manager, check the device's status, and reinstall or enable it as the message describes.") };
    return {};
}

const mon::ProviderStatus* provider(const QList<mon::ProviderStatus>& list, const QString& name)
{
    for (const mon::ProviderStatus& p : list) {
        if (p.name == name)
            return &p;
    }
    return nullptr;
}

QString cell(QString s)
{
    s.replace(u'|', QStringLiteral("\\|"));
    s.replace(u'\n', u' ');
    return s;
}

QString stateWithSource(const MetricValue& v)
{
    if (v.isOk())
        return QStringLiteral("Available");
    return mon::stateName(v.state);
}

} // namespace

// ---- multi-GPU --------------------------------------------------------------------------------------

QList<Technique> assessMultiGpu(const hw::DetectionResult& detection, const OpenClInfo& openCl)
{
    using V = Technique::Verdict;
    const QList<const Adapter*> gpus = physical(detection);
    QList<Technique> out;
    if (gpus.size() < 2) {
        out << Technique{ QStringLiteral("Any multi-GPU technique"), V::NotFeasible,
                          QStringLiteral("Only %1 physical GPU is available to DirectX.").arg(gpus.size()) };
        return out;
    }

    auto without = [&](auto&& has) {
        QList<const Adapter*> missing;
        for (const Adapter* a : gpus) {
            if (!has(*a))
                missing << a;
        }
        return missing;
    };

    // Independent D3D11 work.
    const auto noD3d11 = without([](const Adapter& a) { return a.dxgi->d3d11.device.ok(); });
    QStringList levels;
    for (const Adapter* a : gpus)
        levels << QStringLiteral("%1: FL %2").arg(a->name(), a->dxgi->d3d11.device.ok() ? a->dxgi->d3d11.device.featureLevel : QStringLiteral("none"));
    out << Technique{ QStringLiteral("Separate Direct3D 11 workloads on each GPU, results copied through system memory"),
                      noD3d11.isEmpty() ? V::Feasible : V::NotFeasible,
                      noD3d11.isEmpty() ? QStringLiteral("Every GPU creates a Direct3D 11 device (%1). Work must be split so each part fits the weakest feature level; data moves via the CPU.").arg(levels.join(QStringLiteral(", ")))
                                        : QStringLiteral("No Direct3D 11 device on: %1.").arg(names(noD3d11).join(QStringLiteral(", "))) };

    const auto noCompute = without([](const Adapter& a) { return a.dxgi->d3d11.computeShaders; });
    out << Technique{ QStringLiteral("Direct3D compute shaders on every GPU"), noCompute.isEmpty() ? V::Feasible : V::NotFeasible,
                      noCompute.isEmpty() ? QStringLiteral("Every GPU supports compute shaders.")
                                          : QStringLiteral("No compute-shader support on: %1. Use pixel shaders for work shared with it.").arg(names(noCompute).join(QStringLiteral(", "))) };

    const auto noD3d12 = without([](const Adapter& a) { return a.dxgi->d3d12.device.ok(); });
    out << Technique{ QStringLiteral("Direct3D 12 explicit multi-adapter (cross-adapter heaps)"), noD3d12.isEmpty() ? V::Feasible : V::NotFeasible,
                      noD3d12.isEmpty() ? QStringLiteral("Every GPU creates a Direct3D 12 device; cross-adapter heap support still depends on the drivers and is not tested here.")
                                        : QStringLiteral("No Direct3D 12 on: %1.").arg(names(noD3d12).join(QStringLiteral(", "))) };

    out << Technique{ QStringLiteral("Sharing GPU memory between the GPUs without a CPU copy"), noD3d12.isEmpty() ? V::Unknown : V::NotFeasible,
                      noD3d12.isEmpty() ? QStringLiteral("Would need Direct3D 12 cross-adapter heaps on every GPU; not verified.")
                                        : QStringLiteral("Needs Direct3D 12 cross-adapter heaps, which %1 cannot use. Every transfer goes through system memory.")
                                              .arg(names(noD3d12).join(QStringLiteral(", "))) };

    QSet<QString> common;
    bool first = true;
    for (const Adapter* a : gpus) {
        const QStringList codecs = hw::decoderCodecs(a->dxgi->d3d11.decoderProfiles);
        const QSet<QString> set(codecs.begin(), codecs.end());
        common = first ? set : common.intersect(set);
        first = false;
    }
    QStringList commonList(common.begin(), common.end());
    commonList.sort();
    out << Technique{ QStringLiteral("Hardware video decoding on every GPU at the same time"), commonList.isEmpty() ? V::NotFeasible : V::Feasible,
                      commonList.isEmpty() ? QStringLiteral("The GPUs have no hardware decoder for a common codec.")
                                           : QStringLiteral("Codecs every GPU can decode: %1. Independent streams can be split between GPUs.").arg(commonList.join(QStringLiteral(", "))) };

    QString clReason;
    V clVerdict = V::NotFeasible;
    if (!openCl.loaderPresent) {
        clReason = QStringLiteral("The OpenCL loader (OpenCL.dll) is not installed.");
        if (!openCl.icds.isEmpty())
            clReason += QStringLiteral(" Drivers registered anyway: %1.").arg(openCl.icds.join(QStringLiteral(", ")));
    } else if (openCl.icds.size() < gpus.size()) {
        clReason = QStringLiteral("OpenCL drivers registered: %1 — fewer than the number of GPUs.")
                       .arg(openCl.icds.isEmpty() ? QStringLiteral("none") : openCl.icds.join(QStringLiteral(", ")));
    } else {
        clVerdict = V::Unknown;
        clReason = QStringLiteral("A loader and %1 drivers are registered; device support is not tested.").arg(openCl.icds.size());
    }
    out << Technique{ QStringLiteral("OpenCL on every GPU"), clVerdict, clReason };

    QSet<quint32> vendors;
    for (const Adapter* a : gpus)
        vendors.insert(a->dxgi->vendorId);
    out << Technique{ QStringLiteral("Linked-GPU mode (SLI / CrossFire)"), V::NotFeasible,
                      vendors.size() > 1 ? QStringLiteral("Requires matching GPUs from one vendor; these are from different vendors.")
                                         : QStringLiteral("Requires matching GPUs, a bridge and driver support; not something GPU Fusion can enable, and not a general way to add GPUs together.") };
    return out;
}

// ---- metric availability -------------------------------------------------------------------------

QList<MetricAvailability> metricAvailability(const mon::Snapshot& snapshot)
{
    QList<MetricAvailability> rows;
    auto add = [&](const QString& name, auto getter) {
        MetricAvailability row{ name, {} };
        for (const mon::AdapterSample& a : snapshot.adapters)
            row.perAdapter << getter(a);
        rows << row;
    };
    add(QStringLiteral("GPU utilization"), [](const mon::AdapterSample& a) { return a.utilization; });
    add(QStringLiteral("Per-engine utilization"), [](const mon::AdapterSample& a) {
        for (const mon::EngineSample& e : a.engines) {
            if (e.utilization.isOk())
                return e.utilization;
        }
        return a.engines.isEmpty() ? MetricValue::unavailable(QStringLiteral("No engines")) : a.engines.first().utilization;
    });
    add(QStringLiteral("Dedicated memory in use"), [](const mon::AdapterSample& a) { return a.dedicatedUsed; });
    add(QStringLiteral("Shared memory in use"), [](const mon::AdapterSample& a) { return a.sharedUsed; });
    add(QStringLiteral("Temperature"), [](const mon::AdapterSample& a) { return a.temperature; });
    add(QStringLiteral("Fan speed"), [](const mon::AdapterSample& a) { return a.fanPercent; });
    add(QStringLiteral("Power draw"), [](const mon::AdapterSample& a) { return a.powerWatts; });
    add(QStringLiteral("Core clock"), [](const mon::AdapterSample& a) { return a.coreClockMHz; });
    add(QStringLiteral("Memory clock"), [](const mon::AdapterSample& a) { return a.memoryClockMHz; });
    add(QStringLiteral("Performance state"), [](const mon::AdapterSample& a) { return a.pstate; });
    return rows;
}

// ---- issues -------------------------------------------------------------------------------------------

Report buildReport(const ReportInputs& in)
{
    using S = Issue::Severity;
    Report r;
    r.inputs = in;
    QList<Issue>& issues = r.issues;

    if (!in.detection) {
        issues << Issue{ S::Error, QStringLiteral("Hardware"), QStringLiteral("Hardware detection has not completed"),
                         QStringLiteral("The report was created before GPU Fusion finished listing the graphics adapters."),
                         { QStringLiteral("Wait a moment and create the report again.") } };
    } else {
        const hw::DetectionResult& d = *in.detection;
        for (const QString& e : d.errors)
            issues << Issue{ S::Error, QStringLiteral("Hardware"), QStringLiteral("Detection was incomplete"), e, {} };
        if (d.hardwareCount() == 0)
            issues << Issue{ S::Error, QStringLiteral("Hardware"), QStringLiteral("No physical GPU is available to DirectX"),
                             QStringLiteral("Windows may be using only its software renderer."),
                             { QStringLiteral("Check Device Manager for display adapters with problems."), QStringLiteral("Install the GPU vendor's driver.") } };
        for (const Adapter& a : d.adapters) {
            if (a.isSoftware())
                continue;
            for (const Finding& f : a.findings)
                issues << Issue{ fromFinding(f.severity), a.name(), f.title, f.detail, actionsForFinding(f) };
        }
        r.multiGpu = assessMultiGpu(d, in.openCl);
    }

    // Monitoring
    if (!in.snapshot) {
        issues << Issue{ S::Note, QStringLiteral("Monitoring"), QStringLiteral("No live readings in this report"),
                         in.monitoringPaused ? QStringLiteral("Monitoring is paused.") : QStringLiteral("No monitoring sample had been taken yet."),
                         { QStringLiteral("Resume monitoring on the Dashboard and create the report again.") } };
    } else {
        bool hasNvidia = false;
        if (in.detection) {
            for (const Adapter* a : physical(*in.detection))
                hasNvidia = hasNvidia || a->dxgi->vendorId == 0x10DE;
        }
        const mon::ProviderStatus* nvml = provider(in.providers, QStringLiteral("NVIDIA NVML"));
        if (hasNvidia && nvml && !nvml->active) {
            const bool disabled = nvml->detail.contains(QStringLiteral("Disabled"));
            issues << Issue{ S::Warning, QStringLiteral("Monitoring"), QStringLiteral("NVIDIA sensors are not available"), nvml->detail,
                             { disabled ? QStringLiteral("Enable \"Read NVIDIA sensors through NVML\" in Settings.")
                                        : QStringLiteral("Reinstall the NVIDIA driver; NVML is part of it.") } };
        }
        if (nvml && nvml->active && !nvml->unsupported.isEmpty())
            issues << Issue{ S::Note, QStringLiteral("Monitoring"), QStringLiteral("Some NVIDIA sensors are not exposed by the driver"),
                             QStringLiteral("NVML reports these as not supported for this GPU/driver: %1. They are shown as Unsupported, never as zero.")
                                 .arg(nvml->unsupported.join(QStringLiteral(", "))), {} };
        for (const mon::AdapterSample& a : in.snapshot->adapters) {
            const QList<const MetricValue*> sensors{ &a.temperature, &a.fanPercent, &a.powerWatts, &a.coreClockMHz, &a.memoryClockMHz, &a.pstate };
            if (std::all_of(sensors.begin(), sensors.end(), [](const MetricValue* v) { return v->state == MetricValue::State::Unsupported; }))
                issues << Issue{ S::Note, a.name, QStringLiteral("No temperature, fan, power or clock sensors"),
                                 QStringLiteral("No sensor interface is available for this GPU, so these readings are Unsupported. Utilization and memory are still measured."), {} };
            if (!a.utilization.isOk() && a.utilization.state != MetricValue::State::Unknown)
                issues << Issue{ S::Warning, a.name, QStringLiteral("Utilization could not be read"), a.utilization.note,
                                 { QStringLiteral("If the GPU was just reset or removed, GPU Fusion re-detects it automatically.") } };
        }
    }

    // Per-process coverage
    if (in.processes) {
        if (!in.processes->pdhError.isEmpty())
            issues << Issue{ S::Warning, QStringLiteral("Applications"), QStringLiteral("Windows GPU performance counters could not be read"),
                             in.processes->pdhError, { QStringLiteral("Restart GPU Fusion; if it persists, the performance-counter service may be disabled.") } };
        if (!in.processes->legacyAdapterNames.isEmpty() && in.processes->legacyNotInspectable > 0)
            issues << Issue{ S::Note, QStringLiteral("Applications"), QStringLiteral("Per-application data is partial for older drivers"),
                             QStringLiteral("For %1, %2 of %3 processes could be inspected without administrator rights. Protected and other accounts' processes are not shown for this GPU.")
                                 .arg(in.processes->legacyAdapterNames.join(QStringLiteral(", ")))
                                 .arg(in.processes->legacyInspectable).arg(in.processes->legacyInspectable + in.processes->legacyNotInspectable),
                             { QStringLiteral("Run GPU Fusion as administrator only if you need those processes; it never asks for elevation.") } };
    }

    // GPU preference
    if (!in.preferenceVerification) {
        issues << Issue{ S::Note, QStringLiteral("GPU preference"), QStringLiteral("Not verified on this PC"),
                         QStringLiteral("It is unknown whether the Windows \"Power saving\" / \"High performance\" setting changes which GPU programs use here."),
                         { QStringLiteral("Run the verification on the GPU Preferences page.") } };
    } else if (!in.preferenceVerification->restored) {
        issues << Issue{ S::Warning, QStringLiteral("GPU preference"), QStringLiteral("The probe's preference was not restored"),
                         in.preferenceVerification->restoreError, { QStringLiteral("Restart GPU Fusion; it retries automatically. Only GPUFusionProbe.exe is affected.") } };
    } else if (in.preferenceVerification->preferenceHasNoEffect()) {
        QString gpu;
        for (const pref::VerificationStep& s : in.preferenceVerification->steps) {
            if (s.ok && s.report.d3d11Default) {
                gpu = s.report.d3d11Default->name;
                break;
            }
        }
        issues << Issue{ S::Note, QStringLiteral("GPU preference"), QStringLiteral("The Windows GPU preference makes no difference on this PC"),
                         QStringLiteral("Verified %1 with a probe program: every setting used %2.").arg(in.preferenceVerification->time.toString(QStringLiteral("yyyy-MM-dd HH:mm")), gpu),
                         { QStringLiteral("To use another GPU, choose it inside applications that offer an adapter setting.") } };
    }

    // Storage and settings
    if (in.dataFallback)
        issues << Issue{ S::Warning, QStringLiteral("Storage"), QStringLiteral("Data is stored in the fallback folder"), in.dataProblem,
                         { QStringLiteral("Make the preferred data folder writable, or start GPU Fusion with --data-dir.") } };
    if (!in.settingsWarnings.isEmpty())
        issues << Issue{ S::Warning, QStringLiteral("Settings"), QStringLiteral("The settings file needed corrections"),
                         in.settingsWarnings.join(QStringLiteral(" ")), { QStringLiteral("Review the Settings page and save.") } };
    if (!in.recentProblems.isEmpty())
        issues << Issue{ S::Note, QStringLiteral("Log"), QStringLiteral("%1 recent warning(s) or error(s) in the log").arg(in.recentProblems.size()),
                         QStringLiteral("They are listed at the end of this report."), {} };

    std::stable_sort(issues.begin(), issues.end(), [](const Issue& a, const Issue& b) { return int(a.severity) < int(b.severity); });
    return r;
}

// ---- tables ----------------------------------------------------------------------------------------

Table Report::adapterTable() const
{
    Table t;
    if (!inputs.detection)
        return t;
    QList<const Adapter*> shown;
    for (const Adapter& a : inputs.detection->adapters) {
        if (!a.isSoftware())
            shown << &a;
    }
    t.header << QString();
    for (const Adapter* a : shown)
        t.header << a->name();
    auto row = [&](const QString& label, auto value) {
        QStringList r{ label };
        for (const Adapter* a : shown)
            r << value(*a);
        t.rows << r;
    };
    row(QStringLiteral("Available to DirectX"), [](const Adapter& a) { return a.dxgi ? QStringLiteral("Yes") : QStringLiteral("No"); });
    row(QStringLiteral("Type"), [](const Adapter& a) {
        switch (a.classification.value) {
        case AdapterClass::Discrete:   return QStringLiteral("Discrete");
        case AdapterClass::Integrated: return QStringLiteral("Integrated");
        case AdapterClass::Software:   return QStringLiteral("Software");
        case AdapterClass::Unknown:    return QStringLiteral("Unknown");
        }
        return QString();
    });
    row(QStringLiteral("PCI vendor:device"), [](const Adapter& a) {
        const hw::PciIds ids = a.pciIds();
        return ids.valid ? QStringLiteral("%1:%2").arg(ids.vendor, 4, 16, QLatin1Char('0')).arg(ids.device, 4, 16, QLatin1Char('0')).toUpper() : QStringLiteral("—");
    });
    row(QStringLiteral("Device Manager"), [](const Adapter& a) {
        if (!a.pnp) return QStringLiteral("No entry matched");
        if (!a.pnp->present) return QStringLiteral("Not present");
        return a.pnp->hasProblem ? QStringLiteral("Problem code %1").arg(a.pnp->problemCode) : QStringLiteral("OK");
    });
    row(QStringLiteral("Driver"), [](const Adapter& a) {
        if (!a.pnp) return a.dxgi ? a.dxgi->umdVersion : QString();
        return QStringLiteral("%1 (%2)").arg(a.pnp->driverVersion, a.pnp->driverDate.toString(Qt::ISODate));
    });
    row(QStringLiteral("Driver model"), [](const Adapter& a) { return a.dxgi ? QStringLiteral("WDDM %1").arg(hw::formatWddm(a.dxgi->wddmVersion)) : QStringLiteral("—"); });
    row(QStringLiteral("Direct3D 11"), [](const Adapter& a) {
        if (!a.dxgi) return QStringLiteral("—");
        const ApiProbe& p = a.dxgi->d3d11.device;
        return p.ok() ? QStringLiteral("Feature level %1").arg(p.featureLevel) : QStringLiteral("Failed: %1").arg(p.error);
    });
    row(QStringLiteral("Compute shaders"), [](const Adapter& a) { return a.dxgi ? (a.dxgi->d3d11.computeShaders ? QStringLiteral("Yes") : QStringLiteral("No")) : QStringLiteral("—"); });
    row(QStringLiteral("Direct3D 12"), [](const Adapter& a) {
        if (!a.dxgi) return QStringLiteral("—");
        const ApiProbe& p = a.dxgi->d3d12.device;
        return p.ok() ? QStringLiteral("Feature level %1").arg(a.dxgi->d3d12.maxFeatureLevel) : QStringLiteral("Not available: %1").arg(p.error);
    });
    row(QStringLiteral("Hardware video decode"), [](const Adapter& a) {
        if (!a.dxgi) return QStringLiteral("—");
        const QStringList c = hw::decoderCodecs(a.dxgi->d3d11.decoderProfiles);
        return c.isEmpty() ? QStringLiteral("None reported") : c.join(QStringLiteral(", "));
    });
    row(QStringLiteral("Dedicated memory"), [](const Adapter& a) { return a.dxgi ? hw::formatBytes(a.dxgi->dedicatedVideoMemory) : QStringLiteral("—"); });
    row(QStringLiteral("Displays connected"), [](const Adapter& a) { return a.dxgi ? QString::number(a.dxgi->outputs.size()) : QStringLiteral("—"); });
    return t;
}

Table Report::metricTable() const
{
    Table t;
    if (!inputs.snapshot)
        return t;
    t.header << QStringLiteral("Metric");
    for (const mon::AdapterSample& a : inputs.snapshot->adapters)
        t.header << a.name;
    for (const MetricAvailability& m : metricAvailability(*inputs.snapshot)) {
        QStringList r{ m.metric };
        for (const MetricValue& v : m.perAdapter)
            r << stateWithSource(v);
        t.rows << r;
    }
    return t;
}

Table Report::multiGpuTable() const
{
    Table t{ { QStringLiteral("Technique"), QStringLiteral("Verdict"), QStringLiteral("Why") }, {} };
    for (const Technique& tech : multiGpu)
        t.rows << QStringList{ tech.name, verdictName(tech.verdict), tech.reason };
    return t;
}

Table Report::preferenceTable() const
{
    Table t{ { QStringLiteral("Preference set for the probe"), QStringLiteral("GPU used by the probe") }, {} };
    if (inputs.preferenceVerification) {
        for (const pref::VerificationStep& st : inputs.preferenceVerification->steps)
            t.rows << QStringList{ pref::gpuPreferenceText(st.preference), st.ok ? st.report.d3d11Default->name : QStringLiteral("failed: %1").arg(st.error) };
    }
    return t;
}

// ---- rendering -------------------------------------------------------------------------------------

namespace {

void writeTable(QTextStream& s, const Table& t)
{
    if (t.header.isEmpty())
        return;
    s << "|";
    for (const QString& h : t.header)
        s << " " << cell(h) << " |";
    s << "\n|";
    for (int i = 0; i < t.header.size(); ++i)
        s << "---|";
    s << "\n";
    for (const QStringList& r : t.rows) {
        s << "|";
        for (const QString& c : r)
            s << " " << cell(c) << " |";
        s << "\n";
    }
    s << "\n";
}

} // namespace

QString Report::toMarkdown() const
{
    const ReportInputs& in = inputs;
    QString md;
    QTextStream s(&md);
    s << "# GPU Fusion diagnostic report\n\n";
    s << "Generated " << in.generated.toString(QStringLiteral("yyyy-MM-dd HH:mm")) << " · GPU Fusion " << in.appVersion
      << " · Qt " << in.qtVersion << " · " << in.osDescription << "\n\n";
    s << "> Privacy: this report contains hardware, driver and configuration details only. It excludes your user name, "
         "computer name, the list of running programs and the paths of applications you configured.\n\n";

    s << "## Summary\n\n";
    if (in.detection) {
        int dmOnly = 0;
        for (const Adapter& a : in.detection->adapters)
            dmOnly += a.dxgi ? 0 : 1;
        s << "- " << in.detection->hardwareCount() << " physical GPU(s) available to DirectX, " << in.detection->softwareCount()
          << " software renderer(s), " << dmOnly << " Device Manager entr" << (dmOnly == 1 ? "y" : "ies") << " not available to DirectX\n";
    }
    s << "- " << count(Issue::Severity::Error) << " error(s), " << count(Issue::Severity::Warning) << " warning(s), "
      << count(Issue::Severity::Note) << " note(s)\n\n";

    s << "## Issues and suggestions\n\n";
    if (issues.isEmpty())
        s << "None.\n\n";
    for (const Issue& i : issues) {
        s << "### [" << severityName(i.severity) << "] " << i.subject << ": " << i.title << "\n\n" << i.detail << "\n\n";
        for (const QString& a : i.actions)
            s << "- Suggested: " << a << "\n";
        if (!i.actions.isEmpty())
            s << "\n";
    }

    if (in.detection) {
        s << "## Graphics adapters\n\n";
        writeTable(s, adapterTable());
    }

    s << "## Monitoring\n\n";
    for (const mon::ProviderStatus& p : in.providers) {
        s << "- **" << p.name << "**: " << (p.active ? "active" : "inactive") << " — " << p.detail;
        if (!p.unsupported.isEmpty())
            s << " Not supported: " << p.unsupported.join(QStringLiteral(", ")) << ".";
        s << "\n";
    }
    s << "- **Windows GPU performance counters**: used for per-application data on WDDM 2.0+ GPUs only.\n\n";
    if (in.snapshot) {
        writeTable(s, metricTable());
        s << "Monitoring cost at the time of the report: collection " << QString::number(in.snapshot->collectionMs, 'f', 2) << " ms per sample.\n\n";
    }

    if (in.processes) {
        s << "## Per-application coverage\n\n";
        if (!in.processes->pdhAdapterNames.isEmpty())
            s << "- " << in.processes->pdhAdapterNames.join(QStringLiteral(", ")) << ": Windows performance counters, all processes.\n";
        if (!in.processes->legacyAdapterNames.isEmpty())
            s << "- " << in.processes->legacyAdapterNames.join(QStringLiteral(", ")) << ": D3DKMT, " << in.processes->legacyInspectable << " of "
              << (in.processes->legacyInspectable + in.processes->legacyNotInspectable) << " processes inspectable without administrator rights.\n";
        s << "\n";
    }

    s << "## GPU preference\n\n";
    s << "- Applications with a Windows GPU preference: " << in.configuredPreferences << " (names not included).\n";
    if (in.preferenceVerification) {
        s << "- Verified " << in.preferenceVerification->time.toString(QStringLiteral("yyyy-MM-dd HH:mm")) << " with GPUFusionProbe.exe; original value restored: "
          << (in.preferenceVerification->restored ? "yes" : "no") << "\n\n";
        writeTable(s, preferenceTable());
    } else {
        s << "- Not verified on this PC.\n\n";
    }

    if (!multiGpu.isEmpty()) {
        s << "## Multi-GPU compatibility\n\nFeasible means the technique can run on these GPUs; whether it is faster than one GPU is only known after measuring it.\n\n";
        writeTable(s, multiGpuTable());
    }

    s << "## Application and storage\n\n- Build: " << in.buildDescription << "\n- Data folder: " << QDir::toNativeSeparators(in.dataRoot)
      << (in.dataFallback ? " (fallback)" : "") << "\n- OpenCL loader: " << (in.openCl.loaderPresent ? "present" : "not installed")
      << "; registered OpenCL drivers: " << (in.openCl.icds.isEmpty() ? QStringLiteral("none") : in.openCl.icds.join(QStringLiteral(", "))) << "\n\n";

    if (!in.recentProblems.isEmpty()) {
        s << "## Recent warnings and errors (log)\n\n```\n";
        for (const LogLine& l : in.recentProblems)
            s << l.time.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")) << " " << l.severity << " [" << l.subsystem << "] " << l.message << "\n";
        s << "```\n";
    }
    return md;
}

QJsonObject Report::toJson() const
{
    const ReportInputs& in = inputs;
    QJsonArray issuesJson;
    for (const Issue& i : issues)
        issuesJson << QJsonObject{ { "severity", severityName(i.severity) }, { "subject", i.subject }, { "title", i.title },
                                   { "detail", i.detail }, { "actions", QJsonArray::fromStringList(i.actions) } };
    QJsonArray adapters;
    if (in.detection) {
        for (const Adapter& a : in.detection->adapters) {
            QJsonObject o{ { "name", a.name() }, { "software", a.isSoftware() }, { "availableToDirectX", a.availableToDirectX() } };
            const hw::PciIds ids = a.pciIds();
            if (ids.valid) {
                o.insert(QStringLiteral("vendorId"), qint64(ids.vendor));
                o.insert(QStringLiteral("deviceId"), qint64(ids.device));
            }
            if (a.dxgi) {
                o.insert(QStringLiteral("wddm"), hw::formatWddm(a.dxgi->wddmVersion));
                o.insert(QStringLiteral("d3d11"), a.dxgi->d3d11.device.ok() ? a.dxgi->d3d11.device.featureLevel : QString());
                o.insert(QStringLiteral("computeShaders"), a.dxgi->d3d11.computeShaders);
                o.insert(QStringLiteral("d3d12"), a.dxgi->d3d12.device.ok() ? a.dxgi->d3d12.maxFeatureLevel : QString());
                o.insert(QStringLiteral("videoDecode"), QJsonArray::fromStringList(hw::decoderCodecs(a.dxgi->d3d11.decoderProfiles)));
                o.insert(QStringLiteral("dedicatedMemoryBytes"), qint64(a.dxgi->dedicatedVideoMemory));
            }
            if (a.pnp) {
                o.insert(QStringLiteral("driverVersion"), a.pnp->driverVersion);
                o.insert(QStringLiteral("driverDate"), a.pnp->driverDate.toString(Qt::ISODate));
                o.insert(QStringLiteral("deviceManagerProblemCode"), qint64(a.pnp->problemCode));
            }
            adapters << o;
        }
    }
    QJsonArray metrics;
    if (in.snapshot) {
        for (const MetricAvailability& m : metricAvailability(*in.snapshot)) {
            QJsonArray values;
            for (int i = 0; i < m.perAdapter.size(); ++i)
                values << QJsonObject{ { "adapter", in.snapshot->adapters[i].name }, { "state", mon::stateName(m.perAdapter[i].state) },
                                       { "note", m.perAdapter[i].note } };
            metrics << QJsonObject{ { "metric", m.metric }, { "values", values } };
        }
    }
    QJsonArray techniques;
    for (const Technique& t : multiGpu)
        techniques << QJsonObject{ { "technique", t.name }, { "verdict", verdictName(t.verdict) }, { "reason", t.reason } };
    QJsonArray log;
    for (const LogLine& l : in.recentProblems)
        log << QJsonObject{ { "time", l.time.toString(Qt::ISODate) }, { "severity", l.severity }, { "subsystem", l.subsystem }, { "message", l.message } };

    return QJsonObject{
        { "schemaVersion", 1 },
        { "generated", in.generated.toString(Qt::ISODate) },
        { "application", QJsonObject{ { "version", in.appVersion }, { "qt", in.qtVersion }, { "os", in.osDescription }, { "build", in.buildDescription } } },
        { "summary", QJsonObject{ { "errors", count(Issue::Severity::Error) }, { "warnings", count(Issue::Severity::Warning) },
                                  { "notes", count(Issue::Severity::Note) } } },
        { "issues", issuesJson },
        { "adapters", adapters },
        { "monitoring", QJsonObject{ { "paused", in.monitoringPaused }, { "metrics", metrics } } },
        { "multiGpu", techniques },
        { "gpuPreference", QJsonObject{ { "configuredApplications", in.configuredPreferences },
                                        { "verification", in.preferenceVerification ? in.preferenceVerification->toJson() : QJsonValue(QJsonValue::Null) } } },
        { "storage", QJsonObject{ { "dataFolder", QDir::toNativeSeparators(in.dataRoot) }, { "fallback", in.dataFallback } } },
        { "openCl", QJsonObject{ { "loaderPresent", in.openCl.loaderPresent }, { "drivers", QJsonArray::fromStringList(in.openCl.icds) } } },
        { "recentLogProblems", log },
    };
}

// ---- log -------------------------------------------------------------------------------------------

QList<LogLine> parseLogProblems(const QString& text, int maxLines)
{
    static const QRegularExpression re(QStringLiteral(R"(^(\S+) (DEBUG|INFO |WARN |ERROR) \[([^\]]*)\] \(t\d+\) (.*)$)"));
    QList<LogLine> out;
    bool lastWasProblem = false;
    for (const QString& line : text.split(u'\n')) {
        const QString l = QString(line).remove(u'\r');
        if (l.startsWith(QLatin1StringView("    "))) { // continuation of the previous entry
            if (lastWasProblem && !out.isEmpty())
                out.last().message += u' ' + l.trimmed();
            continue;
        }
        const auto m = re.match(l);
        lastWasProblem = false;
        if (!m.hasMatch())
            continue;
        const QString level = m.captured(2).trimmed();
        if (level != QLatin1StringView("WARN") && level != QLatin1StringView("ERROR"))
            continue;
        out << LogLine{ QDateTime::fromString(m.captured(1), Qt::ISODateWithMs), level, m.captured(3), m.captured(4) };
        lastWasProblem = true;
    }
    while (out.size() > maxLines)
        out.removeFirst();
    return out;
}

// ---- privacy -----------------------------------------------------------------------------------------

QString Redactor::apply(const QString& text) const
{
    QString out = text;
    auto replaceAll = [&](const QString& needle, const QString& with) {
        if (needle.size() >= 3)
            out.replace(needle, with, Qt::CaseInsensitive);
    };
    if (!userProfile.isEmpty()) {
        replaceAll(QDir::toNativeSeparators(userProfile), QStringLiteral("%USERPROFILE%"));
        replaceAll(QDir::fromNativeSeparators(userProfile), QStringLiteral("%USERPROFILE%"));
        replaceAll(QString(QDir::toNativeSeparators(userProfile)).replace(u'\\', QStringLiteral("\\\\")), QStringLiteral("%USERPROFILE%")); // JSON-escaped
    }
    if (!computerName.isEmpty() && !userName.isEmpty()) {
        replaceAll(computerName + u'\\' + userName, QStringLiteral("<computer>\\<user>"));
        replaceAll(computerName + QStringLiteral("\\\\") + userName, QStringLiteral("<computer>\\\\<user>"));
    }
    if (!computerName.isEmpty()) {
        const QRegularExpression word(QStringLiteral("\\b%1\\b").arg(QRegularExpression::escape(computerName)), QRegularExpression::CaseInsensitiveOption);
        out.replace(word, QStringLiteral("<computer>"));
    }
    return out;
}

Redactor currentRedactor()
{
    return { qEnvironmentVariable("USERPROFILE"), qEnvironmentVariable("USERNAME"), qEnvironmentVariable("COMPUTERNAME") };
}

} // namespace gf::diag
