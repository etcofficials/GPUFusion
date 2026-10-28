#include "compat/Compatibility.h"

#include "hardware/AdapterModel.h"
#include "platform/windows/PeImports.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSet>

namespace gf::compat {

QList<Capability> allCapabilities()
{
    return { Capability::SplitFrames, Capability::SplitWorkloads, Capability::MoveOffGameGpu, Capability::SelectGpu, Capability::ConcurrentJobs,
             Capability::Scheduling };
}

QString capabilityKey(Capability c)
{
    switch (c) {
    case Capability::SplitFrames: return QStringLiteral("split-frames");
    case Capability::SplitWorkloads: return QStringLiteral("split-workloads");
    case Capability::MoveOffGameGpu: return QStringLiteral("move-off-game-gpu");
    case Capability::SelectGpu: return QStringLiteral("select-gpu");
    case Capability::ConcurrentJobs: return QStringLiteral("concurrent-jobs");
    case Capability::Scheduling: return QStringLiteral("scheduling");
    }
    return {};
}

QString capabilityText(Capability c)
{
    switch (c) {
    case Capability::SplitFrames: return QStringLiteral("Render its frames on both GPUs");
    case Capability::SplitWorkloads: return QStringLiteral("Run its own separate workloads on both GPUs");
    case Capability::MoveOffGameGpu: return QStringLiteral("Move its GPU work off the game's GPU");
    case Capability::SelectGpu: return QStringLiteral("Choose its GPU");
    case Capability::ConcurrentJobs: return QStringLiteral("Run its jobs on both GPUs at once");
    case Capability::Scheduling: return QStringLiteral("Faster through scheduling and resource management");
    }
    return {};
}

QString statusKey(Status s)
{
    switch (s) {
    case Status::Supported: return QStringLiteral("supported");
    case Status::Experimental: return QStringLiteral("experimental");
    case Status::Partial: return QStringLiteral("partial");
    case Status::Unsupported: return QStringLiteral("unsupported");
    case Status::NotTested: return QStringLiteral("not-tested");
    }
    return {};
}

QString statusText(Status s)
{
    switch (s) {
    case Status::Supported: return QStringLiteral("Supported");
    case Status::Experimental: return QStringLiteral("Experimental");
    case Status::Partial: return QStringLiteral("Partially supported");
    case Status::Unsupported: return QStringLiteral("Unsupported");
    case Status::NotTested: return QStringLiteral("Not tested");
    }
    return {};
}

std::optional<Capability> capabilityFromKey(const QString& key)
{
    for (Capability c : allCapabilities())
        if (capabilityKey(c) == key)
            return c;
    return std::nullopt;
}

std::optional<Status> statusFromKey(const QString& key)
{
    for (Status s : { Status::Supported, Status::Experimental, Status::Partial, Status::Unsupported, Status::NotTested })
        if (statusKey(s) == key)
            return s;
    return std::nullopt;
}

QStringList ProgramFacts::apis() const
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

ProgramFacts factsFrom(const QString& exePath, const std::optional<QStringList>& imports, const QStringList& filesBeside)
{
    ProgramFacts f;
    f.exePath = exePath;
    f.exeName = QFileInfo(exePath).fileName().toLower();
    f.importsRead = imports.has_value();
    if (imports)
        for (const QString& i : *imports)
            f.imports << i.toLower();
    auto imp = [&](const char* dll) { return f.imports.contains(QLatin1StringView(dll)); };
    f.d3d9 = imp("d3d9.dll");
    f.d3d11 = imp("d3d11.dll") || imp("d3d10.dll") || imp("d3d10_1.dll");
    f.d3d12 = imp("d3d12.dll");
    f.dxgi = imp("dxgi.dll");
    f.opengl = imp("opengl32.dll");
    f.vulkan = imp("vulkan-1.dll");
    f.opencl = imp("opencl.dll");
    for (const QString& i : f.imports)
        f.cuda = f.cuda || i.startsWith(QLatin1StringView("cudart")) || i == QLatin1StringView("nvcuda.dll");
    QStringList beside;
    for (const QString& b : filesBeside)
        beside << b.toLower();
    auto has = [&](const char* name) { return beside.contains(QLatin1StringView(name)); };
    // Runtimes and engines, from the files that ship with them.
    const bool chromium = has("icudtl.dat") && (has("v8_context_snapshot.bin") || has("snapshot_blob.bin"));
    if (chromium)
        f.runtimes << (has("resources.pak") && (has("resources") || has("app.asar")) && !f.exeName.contains(QLatin1StringView("chrome")) && f.exeName != QLatin1StringView("msedge.exe")
                           && f.exeName != QLatin1StringView("brave.exe")
                           ? QStringLiteral("Electron")
                           : QStringLiteral("Chromium"));
    if (f.exeName == QLatin1StringView("javaw.exe") || f.exeName == QLatin1StringView("java.exe") || imp("jvm.dll") || has("jvm.dll"))
        f.runtimes << QStringLiteral("Java");
    if (has("unityplayer.dll") || imp("unityplayer.dll"))
        f.runtimes << QStringLiteral("Unity");
    if (f.exeName.endsWith(QLatin1StringView("-win64-shipping.exe")) || f.exeName.endsWith(QLatin1StringView("-win64-test.exe")))
        f.runtimes << QStringLiteral("Unreal Engine");
    if (f.exeName.startsWith(QLatin1StringView("godot")) || has(qPrintable(QFileInfo(exePath).completeBaseName().toLower() + QStringLiteral(".pck"))))
        f.runtimes << QStringLiteral("Godot");
    if (imp("sdl2.dll") || imp("sdl3.dll"))
        f.runtimes << QStringLiteral("SDL");
    if (imp("mscoree.dll"))
        f.runtimes << QStringLiteral(".NET");
    if (imp("gpufusionsdk.dll") || f.exeName.startsWith(QLatin1StringView("gpufusion")))
        f.usesGpuFusionSdk = true;
    if (f.usesGpuFusionSdk)
        f.runtimes << QStringLiteral("GPU Fusion SDK");
    const QString lower = QDir::fromNativeSeparators(exePath).toLower();
    f.storePackage = lower.contains(QLatin1StringView("/windowsapps/")) || lower.contains(QLatin1StringView("/xboxgames/"))
                     || lower.contains(QLatin1StringView("/wpsystem/")) || has("appxmanifest.xml") || has("microsoftgame.config");
    return f;
}

ProgramFacts examine(const QString& exePath)
{
    QString error;
    const std::optional<QStringList> imports = pref::win::readImports(exePath, &error);
    const QFileInfo fi(exePath);
    QStringList beside = QDir(fi.absolutePath()).entryList(QDir::Files | QDir::Dirs | QDir::NoDotAndDotDot);
    // Chromium keeps its runtime files in a version folder beside the executable.
    for (const QString& d : QDir(fi.absolutePath()).entryList(QDir::Dirs | QDir::NoDotAndDotDot))
        if (!d.isEmpty() && d.at(0).isDigit())
            beside << QDir(fi.absolutePath() + u'/' + d).entryList(QDir::Files);
    ProgramFacts f = factsFrom(exePath, imports, beside);
    f.importError = error;
    return f;
}

SystemFacts systemFacts(const hw::DetectionResult& detection, std::optional<bool> preferenceReachesIntegrated)
{
    SystemFacts s;
    s.preferenceReachesIntegrated = preferenceReachesIntegrated;
    QList<const hw::Adapter*> physical;
    for (const hw::Adapter& a : detection.adapters)
        if (a.dxgi && !a.isSoftware())
            physical << &a;
    std::stable_sort(physical.begin(), physical.end(), [](const hw::Adapter* x, const hw::Adapter* y) {
        return x->classification.value == hw::AdapterClass::Discrete && y->classification.value != hw::AdapterClass::Discrete;
    });
    for (const hw::Adapter* a : physical) {
        s.gpus << a->name();
        if (a->dxgi->d3d12.device.ok())
            s.d3d12Gpus << a->name();
        if (a->classification.value == hw::AdapterClass::Integrated && s.integratedGpu.isEmpty()) {
            s.integratedGpu = a->name();
            s.integratedHasD3D11Fl11 = a->dxgi->d3d11.device.ok() && a->dxgi->d3d11.device.featureLevel >= QLatin1StringView("11_0");
        }
        if (a->classification.value == hw::AdapterClass::Discrete && s.discreteGpu.isEmpty())
            s.discreteGpu = a->name();
    }
    s.physicalGpus = int(physical.size());
    return s;
}

// ---- built-in entries ------------------------------------------------------------------------------

namespace {
Entry make(const QString& id, const QString& program, const QStringList& exes, const QStringList& runtimes, const QString& mechanism,
           const QString& evidence, const QList<std::tuple<Capability, Status, QString>>& rows)
{
    Entry e;
    e.id = id;
    e.program = program;
    e.executables = exes;
    e.runtimes = runtimes;
    e.mechanism = mechanism;
    e.evidence = evidence;
    for (const auto& [c, s, r] : rows) {
        e.status.insert(c, s);
        e.reason.insert(c, r);
    }
    return e;
}
} // namespace

QList<Entry> builtInEntries()
{
    using C = Capability;
    using S = Status;
    const QString noSplit = QStringLiteral("Rendering one program's frames on two GPUs needs the program's own explicit multi-adapter design and "
                                           "Direct3D 12 or Vulkan on both GPUs.");
    return {
        make(QStringLiteral("gpufusion-sdk-tools"), QStringLiteral("GPU Fusion's own SDK programs"),
             { QStringLiteral("gpufusionsplitdemo.exe"), QStringLiteral("gpufusionimage.exe") }, {},
             QStringLiteral("GPUFUSION_ADAPTERS from a GPU Fusion profile (SDK)"),
             QStringLiteral("Measured on the reference PC (2026-10-28): hash split 1.14×, photo filtering 1.14× (split) and 1.15× (batch) "
                            "faster than the GT 730 alone, every output bit-exact."),
             { { C::SplitWorkloads, S::Supported, QStringLiteral("The SDK's scheduler divides its work between both GPUs; measured and verified.") },
               { C::ConcurrentJobs, S::Supported, QStringLiteral("GPUFusionImage's batch mode runs whole images on both GPUs at once; measured and verified.") },
               { C::SelectGpu, S::Supported, QStringLiteral("It uses the GPUs a GPU Fusion profile offers (SDK); verified with the probe and the split demo.") },
               { C::Scheduling, S::Supported, QStringLiteral("Adaptive scheduling: the Intel GPU helps only as far as it measurably pays off.") } }),
        make(QStringLiteral("chromium-verified"), QStringLiteral("Microsoft Edge, Brave"), { QStringLiteral("msedge.exe"), QStringLiteral("brave.exe") },
             {}, QStringLiteral("--use-adapter-luid={luid_pair} in a GPU Fusion profile (the program must be started with it)"),
             QStringLiteral("Verified on the reference PC (2026-10-26): WebGL rendered on the HD 2000 with the switch, on the GT 730 without. "
                            "Measured 2026-10-28: a GPU-bound game ran at 27.7 fps with Edge's WebGL on the GT 730 and at 42.9 fps after Edge "
                            "was started on the Intel GPU (game alone: 49.9 fps)."),
             { { C::SelectGpu, S::Supported, QStringLiteral("Chromium's --use-adapter-luid, verified for Edge and Brave on this PC.") },
               { C::MoveOffGameGpu, S::Supported, QStringLiteral("Started with the Intel GPU's LUID, its rendering (and video playback) runs on the Intel GPU; a "
                                                                 "running instance must be restarted for that, which GPU Fusion only does when you ask.") },
               { C::ConcurrentJobs, S::Unsupported, QStringLiteral("An interactive program, not a batch tool.") } }),
        make(QStringLiteral("chromium-other"), QStringLiteral("Other Chromium browsers"),
             { QStringLiteral("chrome.exe"), QStringLiteral("vivaldi.exe"), QStringLiteral("opera.exe"), QStringLiteral("chromium.exe") }, {},
             QStringLiteral("--use-adapter-luid={luid_pair} in a GPU Fusion profile"),
             QStringLiteral("The same Chromium switch as Edge and Brave; not verified with these browsers here."),
             { { C::SelectGpu, S::Experimental, QStringLiteral("Chromium's switch; verified for Edge and Brave, not for this browser.") },
               { C::MoveOffGameGpu, S::Experimental, QStringLiteral("Through Chromium's switch, after a restart you approve; not verified for this browser.") } }),
        make(QStringLiteral("electron"), QStringLiteral("Electron apps (Discord, VS Code, ...)"), {}, { QStringLiteral("Electron") },
             QStringLiteral("--use-adapter-luid={luid_pair} in a GPU Fusion profile"),
             QStringLiteral("Electron embeds Chromium, which accepts the switch; GPU Fusion has not verified it with an Electron app (the Discord "
                            "check was not done)."),
             { { C::SelectGpu, S::Experimental, QStringLiteral("Chromium's switch, as Electron passes command-line switches to Chromium; not verified.") },
               { C::MoveOffGameGpu, S::Experimental, QStringLiteral("Through Chromium's switch after a restart you approve; not verified.") } }),
        make(QStringLiteral("ffmpeg"), QStringLiteral("FFmpeg"), { QStringLiteral("ffmpeg.exe") }, {},
             QStringLiteral("-init_hw_device d3d11va=gf:{dxgi_index} in a profile or a job batch"),
             QStringLiteral("Verified on the reference PC (2026-10-26): Direct3D 11 hardware decoding ran on the chosen adapter."),
             { { C::SelectGpu, S::Supported, QStringLiteral("Its own -init_hw_device option, verified on both GPUs.") },
               { C::MoveOffGameGpu, S::Supported, QStringLiteral("Its hardware decoding can be put on the Intel GPU with its option.") },
               { C::ConcurrentJobs, S::Supported, QStringLiteral("GPU Fusion's job dispatcher runs FFmpeg jobs on both GPUs at once, with identical outputs "
                                                                 "(measured 2026-10-28: 8 H.264 decodes). For decoding it was not faster than the Intel "
                                                                 "GPU alone with 2 jobs at a time (9.0 s vs 8.8 s, no significant difference): the HD 2000's "
                                                                 "decoder is the faster one here.") } }),
        make(QStringLiteral("java"), QStringLiteral("Java programs (javaw.exe), e.g. Minecraft Java Edition"), { QStringLiteral("javaw.exe") }, {},
             QString(),
             QStringLiteral("Minecraft Java 26.3's own log on the reference PC: it tries Vulkan (no driver here), then uses OpenGL 3.3 on the GT 730."),
             { { C::SelectGpu, S::Partial, QStringLiteral("OpenGL programs use the GPU that drives their window's display (the GT 730 here); OpenGL "
                                                         "on Windows offers no adapter choice to a normal program.") },
               { C::MoveOffGameGpu, S::Unsupported, QStringLiteral("Its OpenGL rendering cannot be moved to the Intel GPU while the display is on the GT 730.") },
               { C::SplitWorkloads, S::NotTested, QStringLiteral("A Java program could call the GPU Fusion SDK's C library through the Foreign Function & "
                                                                 "Memory API (JDK 22+); that needs the program's (or a mod's) own code.") } }),
        make(QStringLiteral("roblox"), QStringLiteral("Roblox"), { QStringLiteral("robloxplayerbeta.exe") }, {}, QString(),
             QStringLiteral("Feasibility report 2026-10-28 (docs/GAME_MULTI_GPU_FEASIBILITY.md): Direct3D 11 on the GT 730; Store/GDK package; "
                            "anti-tamper protection; no native extension interface."),
             { { C::SplitWorkloads, S::Unsupported, QStringLiteral("No interface lets outside code run inside Roblox; GPU Fusion never injects code.") },
               { C::Scheduling, S::Partial, QStringLiteral("GPU Fusion cannot schedule its work, but can measure its frame times (from outside) and find "
                                                           "other programs competing for its GPU.") },
               { C::SelectGpu, S::Partial, QStringLiteral("It runs on the GT 730 (Windows' default here); no documented option puts it on the Intel GPU.") },
               { C::MoveOffGameGpu, S::Unsupported, QStringLiteral("No documented way to put Roblox on the Intel GPU on this PC.") },
               { C::ConcurrentJobs, S::Unsupported, QStringLiteral("A game, not a batch tool.") } }),
        make(QStringLiteral("windows-compositor"), QStringLiteral("Windows desktop (compositor)"),
             { QStringLiteral("dwm.exe"), QStringLiteral("csrss.exe") }, {}, QString(),
             QStringLiteral("Windows draws the desktop and every window on the GPU that drives the display (seen on the reference PC: dwm.exe "
                            "and csrss.exe on the GT 730)."),
             { { C::MoveOffGameGpu, S::Unsupported, QStringLiteral("Windows composes the desktop on the GPU that drives the display; it cannot be moved. "
                                                                   "Its load grows with what changes on screen (other windows, animations).") },
               { C::SelectGpu, S::Unsupported, QStringLiteral("Part of Windows; it follows the display.") },
               { C::SplitWorkloads, S::Unsupported, QStringLiteral("Part of Windows.") },
               { C::ConcurrentJobs, S::Unsupported, QStringLiteral("Part of Windows.") },
               { C::Scheduling, S::Unsupported, QStringLiteral("Part of Windows.") } }),
        make(QStringLiteral("obs"), QStringLiteral("OBS Studio"), { QStringLiteral("obs64.exe") }, {},
             QStringLiteral("OBS's own setting: Settings → Advanced → Video → Adapter (made in OBS)"),
             QStringLiteral("Documented OBS setting; not verified by GPU Fusion. Hardware encoders: none usable here (no NVENC on Fermi, no Quick "
                            "Sync encoder in the installed Intel driver)."),
             { { C::SelectGpu, S::Experimental, QStringLiteral("OBS chooses its rendering adapter in its own settings; GPU Fusion can verify the result.") },
               { C::MoveOffGameGpu, S::Experimental, QStringLiteral("Possible through OBS's adapter setting; capturing a game on the other GPU then copies "
                                                                    "frames between GPUs.") },
               { C::SplitWorkloads, S::Unsupported, QStringLiteral("Encoding on the other GPU needs a hardware encoder; neither GPU has a usable one here.") } }),
        make(QStringLiteral("unreal"), QStringLiteral("Unreal Engine games"), {}, { QStringLiteral("Unreal Engine") },
             QStringLiteral("-graphicsadapter={dxgi_index} in a GPU Fusion profile"),
             QStringLiteral("Unreal Engine's documented -graphicsadapter command-line option; not verified on this PC."),
             { { C::SelectGpu, S::Experimental, QStringLiteral("Unreal Engine's -graphicsadapter option; the game's renderer must run on the chosen GPU "
                                                               "(most need feature level 11_0, which the HD 2000 lacks).") } }),
        make(QStringLiteral("godot"), QStringLiteral("Godot games and editor"), {}, { QStringLiteral("Godot") },
             QStringLiteral("--gpu-index {dxgi_index} in a GPU Fusion profile (Vulkan and Direct3D 12 renderers)"),
             QStringLiteral("Godot's documented --gpu-index option applies to its Vulkan and Direct3D 12 renderers. The GPU Fusion GDExtension "
                            "(sdk/godot) lets a Godot project run work on both GPUs."),
             { { C::SelectGpu, S::NotTested, QStringLiteral("--gpu-index needs Vulkan or Direct3D 12 on the chosen GPU; the HD 2000 has neither.") },
               { C::SplitWorkloads, S::Partial, QStringLiteral("Image work of a project that uses the GPU Fusion GDExtension runs on both GPUs (validated "
                                                               "with Godot 4.7.2: 1.09–1.12× faster, bit-exact); the game's own rendering is not split.") } }),
    };
}

QJsonObject entryToJson(const Entry& e)
{
    QJsonObject status, reasons;
    for (auto it = e.status.cbegin(); it != e.status.cend(); ++it) {
        status.insert(capabilityKey(it.key()), statusKey(it.value()));
        if (e.reason.contains(it.key()))
            reasons.insert(capabilityKey(it.key()), e.reason.value(it.key()));
    }
    return { { QStringLiteral("id"), e.id }, { QStringLiteral("program"), e.program }, { QStringLiteral("executables"), QJsonArray::fromStringList(e.executables) },
             { QStringLiteral("runtimes"), QJsonArray::fromStringList(e.runtimes) }, { QStringLiteral("status"), status }, { QStringLiteral("reasons"), reasons },
             { QStringLiteral("mechanism"), e.mechanism }, { QStringLiteral("evidence"), e.evidence } };
}

std::optional<Entry> entryFromJson(const QJsonObject& o, QString* problem)
{
    Entry e;
    e.id = o.value(QLatin1StringView("id")).toString().trimmed();
    e.program = o.value(QLatin1StringView("program")).toString(e.id);
    for (const QJsonValue& v : o.value(QLatin1StringView("executables")).toArray())
        if (!v.toString().trimmed().isEmpty())
            e.executables << v.toString().trimmed().toLower();
    for (const QJsonValue& v : o.value(QLatin1StringView("runtimes")).toArray())
        e.runtimes << v.toString().trimmed();
    const QJsonObject status = o.value(QLatin1StringView("status")).toObject();
    const QJsonObject reasons = o.value(QLatin1StringView("reasons")).toObject();
    for (auto it = status.constBegin(); it != status.constEnd(); ++it) {
        const auto c = capabilityFromKey(it.key());
        const auto s = statusFromKey(it.value().toString());
        if (!c || !s) {
            if (problem)
                *problem = QStringLiteral("%1: unknown capability or status \"%2: %3\"").arg(e.id, it.key(), it.value().toString());
            return std::nullopt;
        }
        e.status.insert(*c, *s);
        e.reason.insert(*c, reasons.value(it.key()).toString(QStringLiteral("Your own entry.")));
    }
    e.mechanism = o.value(QLatin1StringView("mechanism")).toString();
    e.evidence = o.value(QLatin1StringView("evidence")).toString(QStringLiteral("Added by you; not checked by GPU Fusion."));
    if (e.id.isEmpty() || (e.executables.isEmpty() && e.runtimes.isEmpty()) || e.status.isEmpty()) {
        if (problem)
            *problem = QStringLiteral("an entry needs an id, executables or runtimes, and at least one status");
        return std::nullopt;
    }
    return e;
}

QList<Entry> loadEntries(const QString& userFile, QStringList* problems)
{
    QList<Entry> out;
    QFile f(userFile);
    if (!userFile.isEmpty() && f.exists()) {
        QJsonParseError pe{};
        const QJsonDocument d = f.open(QIODevice::ReadOnly) ? QJsonDocument::fromJson(f.readAll(), &pe) : QJsonDocument();
        if (!d.isArray()) {
            if (problems)
                *problems << QStringLiteral("%1: not a JSON array of entries (%2)").arg(QDir::toNativeSeparators(userFile), pe.errorString());
        } else {
            for (const QJsonValue& v : d.array()) {
                QString problem;
                if (auto e = entryFromJson(v.toObject(), &problem)) {
                    e->userDefined = true;
                    out << *e;
                } else if (problems) {
                    *problems << QStringLiteral("%1: %2").arg(QDir::toNativeSeparators(userFile), problem);
                }
            }
        }
    }
    out << builtInEntries();
    return out;
}

// ---- the classifier ----------------------------------------------------------------------------------

const Answer& Assessment::answer(Capability c) const
{
    for (const Answer& a : answers)
        if (a.capability == c)
            return a;
    static const Answer none;
    return none;
}

namespace {

Answer generic(Capability c, const ProgramFacts& p, const SystemFacts& s)
{
    Answer a;
    a.capability = c;
    a.source = QStringLiteral("classifier");
    // Runtimes and engines that load their graphics API at run time count as graphics programs too.
    static const QStringList graphicsRuntimes{ QStringLiteral("Chromium"), QStringLiteral("Electron"), QStringLiteral("Java"), QStringLiteral("Unity"),
                                               QStringLiteral("Unreal Engine"), QStringLiteral("Godot"), QStringLiteral("SDL") };
    bool runtimeGraphics = false;
    for (const QString& r : p.runtimes)
        runtimeGraphics = runtimeGraphics || graphicsRuntimes.contains(r);
    const bool graphics = p.d3d9 || p.d3d11 || p.d3d12 || p.dxgi || p.opengl || p.vulkan || runtimeGraphics;
    const QString apis = p.apis().isEmpty() ? QStringLiteral("no graphics API in its imports (it may load one at run time)") : p.apis().join(QStringLiteral(", "));
    const bool chromium = p.runtimes.contains(QStringLiteral("Chromium")) || p.runtimes.contains(QStringLiteral("Electron"));
    // Can its rendering run on the integrated GPU at all?
    const bool onlyD3D12 = p.d3d12 && !p.d3d11 && !p.d3d9 && !p.opengl;
    const bool onlyVulkan = p.vulkan && !p.d3d11 && !p.d3d9 && !p.d3d12 && !p.opengl;
    const bool integratedLacksD3D12 = !s.integratedGpu.isEmpty() && !s.d3d12Gpus.contains(s.integratedGpu);
    const bool integratedLacksVulkan = !s.integratedGpu.isEmpty() && !s.vulkanGpus.contains(s.integratedGpu);
    switch (c) {
    case Capability::SplitFrames: {
        const bool d3d12Everywhere = s.physicalGpus >= 2 && s.d3d12Gpus.size() >= s.physicalGpus;
        if (d3d12Everywhere && (p.d3d12 || p.vulkan)) {
            a.status = Status::NotTested;
            a.summary = QStringLiteral("Only if the program itself implements explicit multi-adapter rendering; GPU Fusion cannot add it.");
        } else {
            a.status = Status::Unsupported;
            a.summary = QStringLiteral("Not possible on this PC: splitting one program's frames needs Direct3D 12 or Vulkan multi-adapter on every GPU.");
            QStringList missing;
            for (const QString& g : s.gpus)
                if (!s.d3d12Gpus.contains(g))
                    missing << g;
            if (!missing.isEmpty())
                a.reasons << QStringLiteral("No Direct3D 12 on: %1.").arg(missing.join(QStringLiteral(", ")));
            if (s.vulkanGpus.size() < s.physicalGpus)
                a.reasons << QStringLiteral("Vulkan is not available on every GPU.");
            a.reasons << QStringLiteral("Both GPUs being busy at the same time does not mean a program's frames are split.");
        }
        break;
    }
    case Capability::SplitWorkloads:
        if (p.usesGpuFusionSdk) {
            a.status = Status::Experimental;
            a.summary = QStringLiteral("It integrates the GPU Fusion SDK: it can divide its own work between the GPUs.");
        } else {
            a.status = Status::NotTested;
            a.summary = QStringLiteral("Only if the program offers a separate GPU choice for some of its work, or integrates the GPU Fusion SDK.");
            a.reasons << QStringLiteral("GPU Fusion cannot divide another program's work without the program's cooperation.");
        }
        break;
    case Capability::SelectGpu:
    case Capability::MoveOffGameGpu: {
        QStringList blockers;
        if (onlyD3D12 && integratedLacksD3D12)
            blockers << QStringLiteral("it uses Direct3D 12 only, which %1 does not have").arg(s.integratedGpu);
        if (onlyVulkan && integratedLacksVulkan)
            blockers << QStringLiteral("it uses Vulkan only, which %1 does not have").arg(s.integratedGpu);
        if (p.opengl && !p.d3d9 && !p.d3d11 && !p.d3d12 && !chromium)
            blockers << QStringLiteral("it renders with OpenGL, which uses the GPU driving its window's display on a desktop PC");
        if (!blockers.isEmpty()) {
            a.status = c == Capability::SelectGpu ? Status::Partial : Status::Unsupported;
            a.summary = c == Capability::SelectGpu ? QStringLiteral("It runs on the default GPU; it cannot be moved to %1.").arg(s.integratedGpu)
                                                   : QStringLiteral("Its GPU work cannot be moved to %1.").arg(s.integratedGpu);
            a.reasons << QStringLiteral("Why: %1.").arg(blockers.join(QStringLiteral("; ")));
        } else if (p.usesGpuFusionSdk) {
            a.status = Status::Experimental;
            a.summary = QStringLiteral("It follows the GPUs a GPU Fusion profile offers (SDK).");
        } else if (chromium) {
            a.status = Status::Experimental;
            a.summary = QStringLiteral("Chromium-based: Chromium's --use-adapter-luid switch should place it (verified for Edge and Brave, not for this program).");
        } else if (s.preferenceReachesIntegrated.value_or(false)) {
            a.status = Status::Experimental;
            a.summary = QStringLiteral("Windows' per-app preference can place programs on %1 on this PC; GPU Fusion verifies the result.").arg(s.integratedGpu);
        } else if (graphics) {
            a.status = c == Capability::SelectGpu ? Status::Partial : Status::NotTested;
            a.summary = c == Capability::SelectGpu
                ? QStringLiteral("It runs on the default GPU. No mechanism GPU Fusion knows can choose another GPU for it; if it has its own GPU setting, "
                                 "add a registry entry.")
                : QStringLiteral("No known way to move it to the Intel GPU; if it has its own GPU setting, GPU Fusion can use it.");
            if (s.preferenceReachesIntegrated.has_value() && !*s.preferenceReachesIntegrated)
                a.reasons << QStringLiteral("Windows' per-app preference cannot select the integrated GPU on this PC (verified with GPU Fusion's probe).");
        } else {
            a.status = Status::NotTested;
            a.summary = QStringLiteral("No graphics API in its imports; it may not use a GPU, or load one at run time.");
        }
        if (p.storePackage)
            a.reasons << QStringLiteral("It is a Store/Xbox package: Windows may start it through its package, so launch options may not reach it.");
        break;
    }
    case Capability::ConcurrentJobs:
        a.status = Status::NotTested;
        a.summary = QStringLiteral("Only for programs that run separate jobs with a GPU option; GPU Fusion's job dispatcher can then run them on both GPUs.");
        break;
    case Capability::Scheduling:
        if (p.usesGpuFusionSdk) {
            a.status = Status::Experimental;
            a.summary = QStringLiteral("The SDK's adaptive scheduler divides its work by measured speed.");
        } else if (graphics) {
            a.status = Status::Partial;
            a.summary = QStringLiteral("GPU Fusion cannot schedule its work, but can measure its frame times and find other programs competing for its GPU.");
        } else {
            a.status = Status::NotTested;
            a.summary = QStringLiteral("Nothing to schedule that GPU Fusion can see.");
        }
        break;
    }
    if (c != Capability::SplitFrames)
        a.reasons << QStringLiteral("Graphics APIs: %1%2.").arg(apis, p.importsRead ? QString() : QStringLiteral(" (imports not readable)"));
    return a;
}

bool matches(const Entry& e, const ProgramFacts& p)
{
    if (e.executables.contains(p.exeName))
        return true;
    for (const QString& r : e.runtimes)
        if (p.runtimes.contains(r))
            return true;
    return false;
}

} // namespace

Assessment assess(const ProgramFacts& program, const SystemFacts& system, const QList<Entry>& entries, const ProgramEvidence& evidence)
{
    Assessment out;
    out.program = program;
    for (Capability c : allCapabilities())
        out.answers << generic(c, program, system);
    // Entries: earlier ones (the user's) take precedence; each capability is set by the first entry
    // that has it.
    QSet<Capability> set;
    for (const Entry& e : entries) {
        if (!matches(e, program))
            continue;
        out.matchedEntries << e.id;
        for (Answer& a : out.answers) {
            if (set.contains(a.capability) || !e.status.contains(a.capability))
                continue;
            set.insert(a.capability);
            a.status = e.status.value(a.capability);
            a.summary = e.reason.value(a.capability);
            a.source = QStringLiteral("registry: %1%2").arg(e.id, e.userDefined ? QStringLiteral(" (yours)") : QString());
            a.reasons.prepend(QStringLiteral("Evidence: %1").arg(e.evidence));
            if (!e.mechanism.isEmpty() && (a.capability == Capability::SelectGpu || a.capability == Capability::MoveOffGameGpu))
                a.reasons << QStringLiteral("How: %1.").arg(e.mechanism);
        }
    }
    // What GPU Fusion verified for this very program outranks expectations.
    if (evidence.ranOnIntegrated.value_or(false)) {
        for (Answer& a : out.answers) {
            if (a.capability != Capability::SelectGpu && a.capability != Capability::MoveOffGameGpu)
                continue;
            a.status = Status::Supported;
            a.summary = QStringLiteral("Verified on this PC: it rendered on the integrated GPU when GPU Fusion started it that way.");
            a.source = QStringLiteral("verified on this PC");
            if (!evidence.note.isEmpty())
                a.reasons.prepend(evidence.note);
        }
    }
    return out;
}

QStringList assessmentLines(const Assessment& a)
{
    QStringList out;
    out << QStringLiteral("%1 — %2%3").arg(a.program.exeName, a.program.apis().isEmpty() ? QStringLiteral("no graphics API in its imports") : a.program.apis().join(QStringLiteral(", ")),
                                          a.program.runtimes.isEmpty() ? QString() : QStringLiteral("; %1").arg(a.program.runtimes.join(QStringLiteral(", "))));
    for (const Answer& x : a.answers)
        out << QStringLiteral("  %1: %2 — %3 [%4]").arg(capabilityText(x.capability), statusText(x.status), x.summary, x.source);
    return out;
}

QString assessmentsHtml(const QList<Assessment>& list, const SystemFacts& system)
{
    QString h = QStringLiteral("<!doctype html><html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\">"
                               "<title>GPU Fusion compatibility</title><style>body{font:14px/1.5 system-ui,Segoe UI,sans-serif;margin:2em auto;max-width:1100px;"
                               "padding:0 16px;color:#1b1f24;background:#fff}table{border-collapse:collapse;width:100%}td,th{border:1px solid #ccd;padding:4px 6px;"
                               "vertical-align:top;text-align:left}th{background:#eef1f5}.supported{color:#0a7d32}.experimental{color:#8a5a00}.partial{color:#8a5a00}"
                               ".unsupported{color:#b3261e}.not-tested{color:#555}small{color:#555}"
                               "@media(prefers-color-scheme:dark){body{background:#16181c;color:#e6e8eb}th{background:#23272e}td,th{border-color:#3a3f47}"
                               ".supported{color:#5bd187}.unsupported{color:#ff8a80}.experimental,.partial{color:#f0c060}small,.not-tested{color:#aab}}</style></head><body>");
    h += QStringLiteral("<h1>Compatibility</h1><p>This PC: %1. Direct3D 12 on: %2. Vulkan on: %3.</p>")
             .arg(system.gpus.join(QStringLiteral(", ")).toHtmlEscaped(), system.d3d12Gpus.isEmpty() ? QStringLiteral("none") : system.d3d12Gpus.join(QStringLiteral(", ")).toHtmlEscaped(),
                  system.vulkanGpus.isEmpty() ? QStringLiteral("none") : system.vulkanGpus.join(QStringLiteral(", ")).toHtmlEscaped());
    h += QStringLiteral("<table><tr><th>Program</th>");
    for (Capability c : allCapabilities())
        h += QStringLiteral("<th>%1</th>").arg(capabilityText(c).toHtmlEscaped());
    h += QStringLiteral("</tr>");
    for (const Assessment& a : list) {
        h += QStringLiteral("<tr><td><b>%1</b><br><small>%2</small></td>")
                 .arg(a.program.exeName.toHtmlEscaped(), (a.program.apis() + a.program.runtimes).join(QStringLiteral(", ")).toHtmlEscaped());
        for (const Answer& x : a.answers)
            h += QStringLiteral("<td class=\"%1\" title=\"%2\"><b>%3</b><br><small>%4</small></td>")
                     .arg(statusKey(x.status), x.reasons.join(u' ').toHtmlEscaped(), statusText(x.status), x.summary.toHtmlEscaped());
        h += QStringLiteral("</tr>");
    }
    h += QStringLiteral("</table><p><small>Supported = tested and verified. Experimental = implemented or documented, not yet validated. Partially "
                        "supported = only some features. Unsupported = not possible through documented interfaces on this hardware. Not tested = not "
                        "established. Nothing was started to produce this page: it comes from the programs' files and GPU Fusion's records.</small></p></body></html>");
    return h;
}

} // namespace gf::compat
