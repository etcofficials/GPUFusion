#include "preferences/DualGpu.h"

namespace gf::pref {

QStringList GraphicsApis::names() const
{
    QStringList out;
    if (d3d9) out << QStringLiteral("Direct3D 9");
    if (d3d11) out << QStringLiteral("Direct3D 11");
    if (d3d12) out << QStringLiteral("Direct3D 12");
    if (dxgi && !d3d11 && !d3d12) out << QStringLiteral("DXGI");
    if (opengl) out << QStringLiteral("OpenGL");
    if (vulkan) out << QStringLiteral("Vulkan");
    if (cuda) out << QStringLiteral("CUDA");
    if (opencl) out << QStringLiteral("OpenCL");
    return out;
}

GraphicsApis apisFromImports(const QStringList& dlls)
{
    GraphicsApis a;
    for (const QString& raw : dlls) {
        const QString d = raw.toLower();
        a.d3d9 = a.d3d9 || d == QLatin1StringView("d3d9.dll");
        a.d3d11 = a.d3d11 || d == QLatin1StringView("d3d11.dll");
        a.d3d12 = a.d3d12 || d == QLatin1StringView("d3d12.dll");
        a.dxgi = a.dxgi || d == QLatin1StringView("dxgi.dll");
        a.opengl = a.opengl || d == QLatin1StringView("opengl32.dll");
        a.vulkan = a.vulkan || d == QLatin1StringView("vulkan-1.dll");
        a.cuda = a.cuda || d == QLatin1StringView("nvcuda.dll") || d.startsWith(QLatin1StringView("cudart"));
        a.opencl = a.opencl || d == QLatin1StringView("opencl.dll");
    }
    return a;
}

DualGpuAssessment assessDualGpu(const QString& exe, const std::optional<QStringList>& imports, const QString& importError,
                                const MultiGpuSystem& sys)
{
    DualGpuAssessment r;
    if (sys.physicalGpus < 2) {
        r.support = DualGpuSupport::NotPossible;
        r.verdict = QStringLiteral("Only one physical GPU is available, so no program can use two.");
        return r;
    }

    // A. One workload across both GPUs: explicit multi-adapter needs D3D12 on both; linked modes need one vendor.
    const bool explicitPossible = sys.d3d12Gpus >= 2;
    QString caseA = QStringLiteral("One rendering workload split across both GPUs: ");
    if (!explicitPossible)
        caseA += QStringLiteral("not possible on this PC — Direct3D 12 explicit multi-adapter needs Direct3D 12 on both GPUs, and %1 has none")
                     .arg(sys.gpusWithoutD3D12.isEmpty() ? QStringLiteral("one GPU") : sys.gpusWithoutD3D12.join(QStringLiteral(", ")));
    else
        caseA += QStringLiteral("possible only for programs that implement Direct3D 12 multi-adapter themselves");
    caseA += sys.sameVendor ? QStringLiteral("; SLI/CrossFire-style linking would need vendor support.")
                            : QStringLiteral("; SLI/CrossFire and Vulkan device groups need GPUs of the same vendor, which these are not.");
    r.cases << caseA;
    r.cases << QStringLiteral("One program using each GPU for a different task (e.g. rendering on one, video or compute on the other): "
                              "only where the program offers a device setting per task. GPU Fusion cannot add one.");
    r.cases << (sys.preferenceMovesApps
        ? QStringLiteral("Two programs on different GPUs: possible — Windows' preference places programs on either GPU here.")
        : QStringLiteral("Two programs on different GPUs: possible only for programs that choose their GPU themselves; Windows' preference "
                         "does not move programs between these GPUs on this PC (verified)."));
    r.cases << QStringLiteral("A game on one GPU while another program computes on the other: possible when the second program selects "
                              "the other GPU itself — GPU Fusion's Workload Lab does exactly this with its own workloads.");

    if (!imports) {
        r.support = explicitPossible ? DualGpuSupport::Unknown : DualGpuSupport::NotPossible;
        r.verdict = explicitPossible
            ? QStringLiteral("The graphics libraries %1 uses could not be read (%2).").arg(exe, importError)
            : QStringLiteral("%1 cannot have one workload spread over both GPUs on this PC, whatever it uses (the libraries it links "
                             "could not be read: %2).").arg(exe, importError);
        return r;
    }
    const GraphicsApis apis = apisFromImports(*imports);
    const QStringList names = apis.names();
    r.evidence << (names.isEmpty() ? QStringLiteral("%1 links no graphics library directly (it may load one at run time, or start "
                                                     "another program that renders).").arg(exe)
                                   : QStringLiteral("%1 links: %2.").arg(exe, names.join(QStringLiteral(", "))));
    if (!explicitPossible) {
        r.support = DualGpuSupport::NotPossible;
        r.verdict = QStringLiteral("%1 cannot be made to use both GPUs for one workload on this PC: no setting can do it, and the GPU "
                                   "requirements for it are not met.").arg(exe);
    } else if (apis.d3d12) {
        r.support = DualGpuSupport::OnlyIfApplicationImplementsIt;
        r.verdict = QStringLiteral("%1 uses Direct3D 12, so it *could* use both GPUs if it implements explicit multi-adapter — only the "
                                   "program can do that; GPU Fusion cannot switch it on.").arg(exe);
    } else {
        r.support = DualGpuSupport::NotPossible;
        r.verdict = QStringLiteral("%1 does not use Direct3D 12, so it has no Windows mechanism for spreading one workload over two GPUs.").arg(exe);
    }
    return r;
}

} // namespace gf::pref
