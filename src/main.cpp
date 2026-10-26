#include "core/AppPaths.h"
#include "core/Logging.h"
#include "core/Settings.h"
#include "core/Version.h"
#include "benchmarks/BenchmarkService.h"
#include "benchmarks/LabModel.h"
#include "benchmarks/LabRunner.h"
#include "hardware/HardwareService.h"
#include "monitoring/MonitorService.h"
#include "monitoring/UsageObserver.h"
#include "preferences/PreferenceService.h"
#include "profiles/KnownOptions.h"
#include "profiles/ProfileActions.h"
#include "profiles/ProfileStore.h"
#include "profiles/RunningInstances.h"
#include "profiles/Shortcuts.h"
#include "diagnostics/DiagnosticReport.h"
#include "dispatch/JobDispatcher.h"
#include "ui/DiagnosticsCollector.h"
#include "ui/AppContext.h"
#include "ui/MainWindow.h"
#include "ui/Theme.h"
#include "ui/UsageCheck.h"

#include <QApplication>
#include <QCommandLineParser>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QMessageBox>
#include <QSaveFile>
#include <QPushButton>
#include <QScrollArea>
#include <QStandardPaths>
#include <QSysInfo>
#include <QTimer>

namespace {

void applyLoggingSettings(const gf::Settings& s)
{
    gf::log::Level level = gf::log::Level::Info;
    gf::log::parseLevel(s.logLevel, &level);
    gf::log::setMinimumLevel(level);
    gf::log::setLimits(qint64(s.logMaxFileKB) * 1024, s.logMaxFiles);
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("GPUFusion"));
    QApplication::setApplicationDisplayName(QStringLiteral("GPU Fusion"));
    QApplication::setApplicationVersion(QString::fromLatin1(gf::version::kString));
    QApplication::setOrganizationName(QStringLiteral("GPUFusion"));

    QCommandLineParser cli;
    cli.setApplicationDescription(QStringLiteral("GPU Fusion — Windows GPU monitor and workload lab"));
    cli.addHelpOption();
    cli.addVersionOption();
    const QCommandLineOption dataDirOpt(QStringLiteral("data-dir"), QStringLiteral("Use <path> for settings, logs and benchmarks."), QStringLiteral("path"));
    const QCommandLineOption pageOpt(QStringLiteral("page"), QStringLiteral("Open on page <id>: %1.").arg(gf::ui::MainWindow::pageIds().join(QStringLiteral(", "))), QStringLiteral("id"));
    const QCommandLineOption smokeOpt(QStringLiteral("smoke-test"), QStringLiteral("Start, show the window, then exit with code 0 after <ms> (automated launch check)."), QStringLiteral("ms"), QStringLiteral("1500"));
    const QCommandLineOption shotOpt(QStringLiteral("screenshot"), QStringLiteral("Save a PNG of the main window to <file> and exit."), QStringLiteral("file"));
    const QCommandLineOption fullShotOpt(QStringLiteral("screenshot-full"), QStringLiteral("With --screenshot: capture the current page's entire scrollable content instead of the window."));
    const QCommandLineOption shotDelayOpt(QStringLiteral("screenshot-after"), QStringLiteral("With --screenshot: wait <ms> after adapter detection (default 400)."), QStringLiteral("ms"), QStringLiteral("400"));
    const QCommandLineOption verifyOpt(QStringLiteral("verify-gpu-preferences"),
        QStringLiteral("Run the GPU preference verification (sets each preference for GPUFusionProbe.exe only, launches it, restores the original), save the report and exit."));
    const QCommandLineOption exportOpt(QStringLiteral("export-diagnostics"),
        QStringLiteral("Write the diagnostic report to <file>.md and <file>.json (personal identifiers removed) and exit."), QStringLiteral("file"));
    const QCommandLineOption benchOpt(QStringLiteral("run-benchmarks"),
        QStringLiteral("Run every available benchmark at workload size <preset> (quick, standard, heavy) with the run counts from Settings, save the session and exit. Exit code 0 only if all completed with correct output."),
        QStringLiteral("preset"));
    const QCommandLineOption labOpt(QStringLiteral("run-lab"),
        QStringLiteral("Run Workload Lab experiment <name> (split, decode or all) with the run counts from Settings, save the session(s) and exit. Exit code 0 only if every mode completed with correct output."),
        QStringLiteral("name"));
    const QCommandLineOption labSizeOpt(QStringLiteral("lab-size"), QStringLiteral("With --run-lab: hash workload size (quick, standard, heavy; default standard)."),
                                        QStringLiteral("preset"), QStringLiteral("standard"));
    const QCommandLineOption verifyAppOpt(QStringLiteral("verify-app"),
        QStringLiteral("Start <exe> (with any arguments after --), watch which GPU its processes render on, log the staged result and exit. "
                       "Exit code 0 = the requested GPU was confirmed (or nothing was requested and GPU work was seen)."),
        QStringLiteral("exe"));
    const QCommandLineOption verifySecondsOpt(QStringLiteral("verify-seconds"), QStringLiteral("With --verify-app: how long to watch (default 20)."),
                                              QStringLiteral("seconds"), QStringLiteral("20"));
    const QCommandLineOption applyProfileOpt(QStringLiteral("apply-profile"),
        QStringLiteral("Create or update the GPU profile of <exe> with --mode, apply it and exit."), QStringLiteral("exe"));
    const QCommandLineOption modeOpt(QStringLiteral("mode"), QStringLiteral("With --apply-profile: automatic, integrated, discrete or both."),
                                     QStringLiteral("mode"));
    const QCommandLineOption profileArgsOpt(QStringLiteral("profile-args"), QStringLiteral("With --apply-profile: launch arguments stored in the profile."),
                                            QStringLiteral("arguments"));
    const QCommandLineOption knownOptionOpt(QStringLiteral("profile-known-option"),
        QStringLiteral("With --apply-profile: put the program's known GPU option (if GPU Fusion knows one) first in its launch arguments."));
    const QCommandLineOption closeRunningOpt(QStringLiteral("close-running"),
        QStringLiteral("With --launch-profile / --verify-profile: if the program is running, ask it to close first (like its close button; "
                       "never forced). Exit code 10 if it does not close."));
    const QCommandLineOption variantOpt(QStringLiteral("variant"),
        QStringLiteral("With --apply-profile / --launch-profile / --verify-profile / --create-shortcut <exe>: the profile variant <name> "
                       "(another profile for the same program; it never changes the program's Windows GPU preference)."), QStringLiteral("name"));
    const QCommandLineOption exportProfilesOpt(QStringLiteral("export-profiles"),
        QStringLiteral("Write every GPU profile to <file> (JSON, for backup or another PC) and exit."), QStringLiteral("file"));
    const QCommandLineOption importProfilesOpt(QStringLiteral("import-profiles"),
        QStringLiteral("Add the GPU profiles in <file> (from --export-profiles) and exit. Existing profiles are kept unless "
                       "--replace-existing; verification results are not imported and no Windows setting is changed."), QStringLiteral("file"));
    const QCommandLineOption replaceExistingOpt(QStringLiteral("replace-existing"), QStringLiteral("With --import-profiles: replace profiles that already exist."));
    const QCommandLineOption interactiveOpt(QStringLiteral("interactive"),
        QStringLiteral("With --launch-profile: report problems in dialogs and ask what to do if the program is running (used by GPU Fusion shortcuts)."));
    const QCommandLineOption createShortcutOpt(QStringLiteral("create-shortcut"),
        QStringLiteral("Write a shortcut that starts the program of GPU profile <exe|id> through GPU Fusion (to the Desktop, or --shortcut-dir), and exit."),
        QStringLiteral("profile"));
    const QCommandLineOption shortcutDirOpt(QStringLiteral("shortcut-dir"), QStringLiteral("With --create-shortcut: the folder to write it to."),
                                            QStringLiteral("folder"));
    const QCommandLineOption launchProfileOpt(QStringLiteral("launch-profile"),
        QStringLiteral("Start the program of a GPU profile (<exe> or profile id) with its arguments and GPU choice, and exit."), QStringLiteral("profile"));
    const QCommandLineOption verifyProfileOpt(QStringLiteral("verify-profile"),
        QStringLiteral("Start the program of a GPU profile, verify which GPU it renders on, store the result in the profile and exit "
                       "(0 = the profile's GPU mode was confirmed)."), QStringLiteral("profile"));
    const QCommandLineOption dispatchOpt(QStringLiteral("dispatch"),
        QStringLiteral("Run a batch of independent jobs, one per input file, spread over the GPUs: <command> is a command template with "
                       "placeholders {dxgi_index} {luid} {vendor_id} {vendor_hex} {gpu_name} {input} {output} {stem} {job}, or \"decode\" "
                       "(built-in H.264 decoding). Exit code 0 only if every job succeeded."), QStringLiteral("command"));
    const QCommandLineOption dispatchInputsOpt(QStringLiteral("dispatch-inputs"), QStringLiteral("With --dispatch: a folder, or files separated by ';'."),
                                               QStringLiteral("inputs"));
    const QCommandLineOption dispatchOutputOpt(QStringLiteral("dispatch-output"), QStringLiteral("With --dispatch: output folder for {output}."),
                                               QStringLiteral("folder"));
    const QCommandLineOption dispatchPatternOpt(QStringLiteral("dispatch-pattern"),
        QStringLiteral("With --dispatch: output file name, e.g. {stem}.mkv ({stem} {name} {ext} {job}); none = jobs write no file."), QStringLiteral("pattern"));
    const QCommandLineOption dispatchGpusOpt(QStringLiteral("dispatch-gpus"),
        QStringLiteral("With --dispatch: all (default), integrated, discrete, or DXGI indexes such as 0,1."), QStringLiteral("gpus"), QStringLiteral("all"));
    const QCommandLineOption dispatchCompareOpt(QStringLiteral("dispatch-compare"),
        QStringLiteral("With --dispatch: measure the batch <n> times on each GPU alone and on all chosen GPUs, check outputs and report "
                       "whether all GPUs together are significantly faster."), QStringLiteral("n"));
    cli.addOptions({ dataDirOpt, pageOpt, smokeOpt, shotOpt, fullShotOpt, shotDelayOpt, verifyOpt, exportOpt, benchOpt, labOpt, labSizeOpt,
                     verifyAppOpt, verifySecondsOpt, applyProfileOpt, modeOpt, profileArgsOpt, knownOptionOpt, closeRunningOpt, variantOpt, exportProfilesOpt, importProfilesOpt, replaceExistingOpt, interactiveOpt, createShortcutOpt, shortcutDirOpt, launchProfileOpt, verifyProfileOpt,
                     dispatchOpt, dispatchInputsOpt, dispatchOutputOpt, dispatchPatternOpt, dispatchGpusOpt, dispatchCompareOpt });
    cli.process(app);

    // ---- data location + logging ----------------------------------------------
    const QString fallbackRoot = QStandardPaths::writableLocation(QStandardPaths::AppLocalDataLocation);
    const gf::DataLocation data = gf::resolveDataLocation(QApplication::applicationDirPath(), cli.value(dataDirOpt), fallbackRoot);
    const QString layoutError = gf::ensureDataLayout(data);

    QString logError;
    const bool logging = layoutError.isEmpty() &&
        gf::log::initialize({ data.logsDir() }, gf::log::Level::Info, &logError);

    qCInfo(lcApp).noquote() << QStringLiteral("GPU Fusion %1 starting; Qt %2; %3 (kernel %4)")
                                   .arg(QLatin1StringView(gf::version::kString), QLatin1StringView(qVersion()),
                                        QSysInfo::prettyProductName(), QSysInfo::kernelVersion());
    qCInfo(lcApp).noquote() << QStringLiteral("Data folder: %1 (%2)").arg(QDir::toNativeSeparators(data.root), gf::describe(data.source));
    if (!data.problem.isEmpty())
        qCWarning(lcApp).noquote() << data.problem;

    // Graphics drivers write shader caches under %TEMP% when Direct3D devices are created (the NVIDIA
    // driver did so on the reference machine). Point this process's TEMP/TMP at the data folder so
    // that stays off the system drive. Process-local only; the original is kept for child processes.
    if (layoutError.isEmpty()) {
        const QByteArray original = qgetenv("TEMP");
        if (!qEnvironmentVariableIsSet("GPUFUSION_ORIGINAL_TEMP"))
            qputenv("GPUFUSION_ORIGINAL_TEMP", original);
        const QByteArray temp = QDir::toNativeSeparators(data.tempDir()).toLocal8Bit();
        qputenv("TEMP", temp);
        qputenv("TMP", temp);
        qCInfo(lcApp).noquote() << QStringLiteral("Process TEMP: %1 (was %2)").arg(QString::fromLocal8Bit(temp), QString::fromLocal8Bit(original));
    }

    if (!layoutError.isEmpty() || !logging) {
        QMessageBox::warning(nullptr, QStringLiteral("GPU Fusion"),
                             QStringLiteral("GPU Fusion could not prepare its data folder, so settings and logs may not be saved.\n\n%1")
                                 .arg(layoutError.isEmpty() ? logError : layoutError));
    }

    // ---- settings ----------------------------------------------------------------
    gf::SettingsStore store(data.settingsFile());
    gf::SettingsStore::LoadResult loaded = store.load();
    qCInfo(lcSettings).noquote() << QStringLiteral("Settings %1: %2").arg(gf::describe(loaded.status), QDir::toNativeSeparators(store.filePath()));
    for (const QString& w : loaded.warnings)
        qCWarning(lcSettings).noquote() << w;
    if (loaded.status == gf::SettingsStore::LoadStatus::Missing) {
        QString error;
        if (!store.save(loaded.settings, &error))
            qCWarning(lcSettings).noquote() << "Could not write default settings:" << error;
    }
    applyLoggingSettings(loaded.settings);

    gf::ui::applyTheme(app, loaded.settings.theme);

    // Adapter detection runs on a worker thread; the window shows "Detecting…" until it finishes.
    gf::hw::HardwareService hardware;
    // Monitoring starts on its own thread once the first detection has listed the adapters.
    gf::mon::MonitorService monitor(hardware, loaded.settings);
    hardware.refresh(true);

    // GPU preferences: restores a probe value left by an interrupted verification, if any.
    gf::pref::PreferenceService preferences(data.configDir(), data.reportsDir(),
                                            QDir(QApplication::applicationDirPath()).filePath(QStringLiteral("GPUFusionProbe.exe")));

    if (cli.isSet(verifyOpt)) {
        // Explicit command-line request: no window, same safeguards as the button (probe only, restored).
        if (!preferences.probeAvailable()) {
            qCCritical(lcApp).noquote() << "GPUFusionProbe.exe is missing:" << preferences.probePath();
            return 5;
        }
        QObject::connect(&preferences, &gf::pref::PreferenceService::verificationFinished, &app, [&preferences] {
            const auto& v = preferences.lastVerification();
            qCInfo(lcApp).noquote() << "Verification finished; restored:" << v->restored
                                    << "; preference changes the GPU:" << !v->preferenceHasNoEffect();
            QApplication::exit(v->restored ? 0 : 4);
        });
        preferences.startVerification();
        const int code = app.exec();
        gf::log::shutdown();
        return code;
    }

    gf::bench::BenchmarkService benchmarks(data.benchmarksDir());
    gf::bench::BenchmarkService lab(gf::bench::labDir(data.benchmarksDir()));
    gf::profiles::ProfileStore profileStore(QDir(data.configDir()).filePath(QStringLiteral("profiles")));
    gf::ui::AppContext context(data, &store, std::move(loaded), &hardware, &monitor, &preferences, &benchmarks, &lab, &profileStore);
    // Automatic profile backup (Settings > GPU profiles): after every change, when a folder is set.
    profileStore.setChangeCallback([&context, &profileStore] {
        const gf::Settings& s = context.settings();
        if (s.profileBackupFolder.trimmed().isEmpty())
            return;
        const gf::profiles::BackupResult r = gf::profiles::backupStore(profileStore, s.profileBackupFolder, s.profileBackupsKept);
        if (!r.error.isEmpty())
            qCWarning(lcPrefs).noquote() << "Profile backup failed:" << r.error;
        else if (r.written)
            qCInfo(lcPrefs).noquote() << "Profiles backed up to" << r.path << (r.removed ? QStringLiteral("(%1 old backup(s) removed)").arg(r.removed) : QString());
    });
    QObject::connect(&context, &gf::ui::AppContext::settingsChanged, &app, [&app, &monitor](const gf::Settings& s) {
        applyLoggingSettings(s);
        gf::ui::applyTheme(app, s.theme);
        monitor.applySettings(s);
    });

    if (cli.isSet(benchOpt)) {
        // Headless benchmark session: same runner, storage and rules as the Benchmarks page.
        const QString preset = cli.value(benchOpt).toLower();
        if (preset != QLatin1StringView("quick") && preset != QLatin1StringView("standard") && preset != QLatin1StringView("heavy")) {
            qCCritical(lcApp).noquote() << "Unknown benchmark preset" << preset << "(use quick, standard or heavy)";
            gf::log::shutdown();
            return 2;
        }
        monitor.setPaused(true); // our own sampling would load the GPUs being measured
        auto run = [&] {
            const gf::Settings& s = context.settings();
            gf::bench::RunOptions options;
            options.warmupRuns = s.benchmarkWarmupRuns;
            options.measuredRuns = s.benchmarkMeasuredRuns;
            options.preset = preset;
            options.hash = gf::bench::hashPreset(preset);
            options.probePath = preferences.probeAvailable() ? preferences.probePath() : QString();
            const auto specs = gf::bench::availableBenchmarks(*hardware.latest(), options.probePath);
            qCInfo(lcApp).noquote() << QStringLiteral("Running %1 benchmark(s), preset %2").arg(specs.size()).arg(preset);
            benchmarks.start(specs, options, *hardware.latest(), s.benchmarkRetentionDays);
        };
        QObject::connect(&benchmarks, &gf::bench::BenchmarkService::finished, &app, [&](const gf::bench::Session& session) {
            bool allGood = benchmarks.lastSaveError().isEmpty();
            for (const gf::bench::BenchmarkResult& r : session.results)
                allGood = allGood && r.status == gf::bench::Status::Completed && (!r.correct.has_value() || *r.correct);
            for (const QString& line : gf::bench::sessionSummary(session))
                qCInfo(lcApp).noquote() << line;
            QApplication::exit(allGood ? 0 : 7);
        });
        if (hardware.isRunning() || !hardware.latest())
            QObject::connect(&hardware, &gf::hw::HardwareService::detectionFinished, &app, run, Qt::SingleShotConnection);
        else
            run();
        const int code = app.exec();
        gf::log::shutdown();
        return code;
    }

    if (cli.isSet(exportProfilesOpt) || cli.isSet(importProfilesOpt)) {
        // Backup / transfer of GPU profiles (no hardware access needed).
        int code = 0;
        if (cli.isSet(exportProfilesOpt)) {
            QSaveFile out(cli.value(exportProfilesOpt));
            const QList<gf::profiles::AppProfile> all = profileStore.list();
            if (out.open(QIODevice::WriteOnly) && out.write(gf::profiles::exportProfiles(all)) >= 0 && out.commit()) {
                qCInfo(lcApp).noquote() << QStringLiteral("Exported %1 GPU profile(s) to %2").arg(all.size()).arg(QDir::toNativeSeparators(out.fileName()));
            } else {
                qCCritical(lcApp).noquote() << "Export failed:" << out.errorString();
                code = 9;
            }
        } else {
            QFile in(cli.value(importProfilesOpt));
            const gf::profiles::ImportResult r = in.open(QIODevice::ReadOnly)
                ? gf::profiles::importProfiles(profileStore, in.readAll(), cli.isSet(replaceExistingOpt))
                : gf::profiles::ImportResult{ false, in.errorString() };
            if (r.ok)
                qCInfo(lcApp).noquote() << "Import:" << r.summary();
            else
                qCCritical(lcApp).noquote() << "Import failed:" << r.summary();
            code = r.ok ? 0 : 9;
        }
        gf::log::shutdown();
        return code;
    }

    if (cli.isSet(applyProfileOpt) || cli.isSet(launchProfileOpt) || cli.isSet(verifyProfileOpt) || cli.isSet(createShortcutOpt)) {
        // Headless profile operations: same store, apply/launch/verify code as the GPU Manager page.
        int exitCode = 2;
        auto run = [&] {
            const gf::hw::DetectionResult detection = *hardware.latest();
            const QList<gf::pref::SelectableGpu> gpus = gf::profiles::selectableGpus(detection);
            QString error;
            if (cli.isSet(applyProfileOpt)) {
                const QString exe = QDir::toNativeSeparators(QFileInfo(cli.value(applyProfileOpt)).absoluteFilePath());
                const auto mode = gf::profiles::modeFromKey(cli.value(modeOpt).toLower());
                if (!QFileInfo::exists(exe) || !mode) {
                    qCCritical(lcApp).noquote() << "Use --apply-profile <existing exe> --mode automatic|integrated|discrete|both";
                    QApplication::exit(2);
                    return;
                }
                const QString variant = cli.value(variantOpt).trimmed();
                gf::profiles::AppProfile p = profileStore.find(gf::profiles::profileIdFor(exe, variant))
                                                 .value_or(variant.isEmpty() ? gf::profiles::newProfile(exe)
                                                                             : *gf::profiles::newVariant(profileStore.find(exe).value_or(
                                                                                   gf::profiles::newProfile(exe)), variant));
                p.mode = *mode;
                if (cli.isSet(profileArgsOpt))
                    p.arguments = cli.value(profileArgsOpt);
                if (cli.isSet(knownOptionOpt)) {
                    const auto known = gf::profiles::suggestedOptionFor(
                        exe, gf::profiles::loadKnownOptions(QDir(data.configDir()).filePath(QStringLiteral("gpu-options.json"))));
                    if (known)
                        p.arguments = gf::profiles::withKnownOption(p.arguments, *known);
                    qCInfo(lcApp).noquote() << (known ? QStringLiteral("Known GPU option: %1 (%2)").arg(known->arguments, known->evidence)
                                                      : QStringLiteral("No known GPU option for %1").arg(QFileInfo(exe).fileName()));
                }
                const gf::profiles::CapabilityInputs in = gf::profiles::capabilityInputs(exe, detection, preferences.lastVerification());
                const gf::profiles::ApplyResult r = gf::profiles::applyProfile(p, preferences.manager(), in);
                if (r.ok && profileStore.save(p, &error)) {
                    qCInfo(lcApp).noquote() << "Profile" << p.id << "(" << gf::profiles::displayName(p) << ")" << gf::profiles::modeKey(p.mode) << ":" << r.message;
                    exitCode = 0;
                } else {
                    qCCritical(lcApp).noquote() << "Profile not applied:" << (r.ok ? error : r.message);
                    exitCode = 9;
                }
            } else {
                const bool interactive = cli.isSet(interactiveOpt); // started from a GPU Fusion shortcut: tell the user in dialogs
                auto tell = [&](QMessageBox::Icon icon, const QString& text) {
                    if (interactive)
                        QMessageBox(icon, QStringLiteral("GPU Fusion"), text, QMessageBox::Ok).exec();
                };
                const QString key = cli.isSet(createShortcutOpt) ? cli.value(createShortcutOpt)
                                  : cli.isSet(launchProfileOpt)  ? cli.value(launchProfileOpt) : cli.value(verifyProfileOpt);
                const bool isPath = key.contains(u'\\') || key.contains(u'/');
                auto p = profileStore.find(isPath && cli.isSet(variantOpt)
                                               ? gf::profiles::profileIdFor(QFileInfo(key).absoluteFilePath(), cli.value(variantOpt)) : key);
                if (!p) {
                    qCCritical(lcApp).noquote() << "No GPU profile for" << key;
                    tell(QMessageBox::Warning, QStringLiteral("There is no GPU profile for %1 (any more). Open GPU Fusion's GPU Manager to "
                                                              "create one.").arg(key));
                    QApplication::exit(2);
                    return;
                }
                if (cli.isSet(createShortcutOpt)) {
                    const QString folder = cli.isSet(shortcutDirOpt) ? cli.value(shortcutDirOpt) : gf::profiles::desktopFolder();
                    const gf::profiles::ShortcutSpec s = gf::profiles::profileShortcut(
                        *p, QApplication::applicationFilePath(), folder,
                        data.source == gf::DataLocation::Source::CommandLine ? data.root : QString());
                    const bool ok = gf::profiles::writeShortcut(s, &error);
                    qCInfo(lcApp).noquote() << (ok ? QStringLiteral("Shortcut written: %1 -> %2 %3").arg(s.path, s.target, gf::profiles::joinArguments(s.arguments))
                                                   : QStringLiteral("Shortcut not written: %1").arg(error));
                    QApplication::exit(ok ? 0 : 9);
                    return;
                }
                const QString exe = gf::profiles::resolveCurrentExecutable(p->exePath);
                if (const auto running = gf::profiles::runningInstances(exe); !running.isEmpty()) {
                    bool close = cli.isSet(closeRunningOpt);
                    if (interactive && !close && cli.isSet(launchProfileOpt)) {
                        QMessageBox box(QMessageBox::Question, QStringLiteral("GPU Fusion"),
                                        QStringLiteral("%1 is already running. It keeps the GPU it started on, and it may simply take over this "
                                                       "launch.\n\nAsk it to close (like clicking its close button) and start it with GPU mode "
                                                       "\"%2\"?").arg(p->name, gf::profiles::modeText(p->mode)), QMessageBox::Cancel);
                        QPushButton* closeButton = box.addButton(QStringLiteral("Close it and start"), QMessageBox::AcceptRole);
                        QPushButton* anyway = box.addButton(QStringLiteral("Start anyway"), QMessageBox::ActionRole);
                        box.setDefaultButton(closeButton);
                        box.exec();
                        if (box.clickedButton() == closeButton)
                            close = true;
                        else if (box.clickedButton() != anyway) {
                            QApplication::exit(0);
                            return;
                        }
                    }
                    if (close) {
                        const gf::profiles::CloseOutcome closed = gf::profiles::closeGracefully(exe);
                        qCInfo(lcApp).noquote() << closed.message;
                        if (!closed.ok()) {
                            tell(QMessageBox::Information, closed.message);
                            QApplication::exit(10);
                            return;
                        }
                    } else if (!interactive) {
                        qCWarning(lcApp).noquote() << QStringLiteral("%1 is already running (%2 process(es)): it keeps its current GPU, and it may "
                                                                     "take over the new launch. Use --close-running to ask it to close first.")
                                                          .arg(QFileInfo(exe).fileName()).arg(running.size());
                    }
                }
                if (cli.isSet(launchProfileOpt)) {
                    const auto pid = gf::profiles::launchProfile(*p, gpus, &error);
                    qCInfo(lcApp).noquote() << "Launch profile" << gf::profiles::displayName(*p) << (pid ? QStringLiteral("pid %1").arg(*pid) : error);
                    if (!pid)
                        tell(QMessageBox::Warning, QStringLiteral("%1 could not be started: %2").arg(p->name, error));
                    exitCode = pid ? 0 : 9;
                } else {
                    const int seconds = std::clamp(cli.value(verifySecondsOpt).toInt(), 3, 600);
                    const gf::mon::UsageVerdict v = gf::profiles::verifyProfile(*p, detection, preferences.manager().rawValue(
                                                                                    gf::profiles::resolveCurrentExecutable(p->exePath)), seconds);
                    profileStore.save(*p, &error);
                    qCInfo(lcApp).noquote() << "Verify profile" << gf::profiles::displayName(*p) << gf::profiles::modeKey(p->mode) << ":" << v.headline;
                    for (const QString& s : p->lastVerification->stages)
                        qCInfo(lcApp).noquote() << "    " << s;
                    for (const QString& d : v.details)
                        qCInfo(lcApp).noquote() << "    " << d;
                    exitCode = p->lastVerification->confirmed || (p->mode == gf::profiles::GpuMode::Automatic && v.outcome == gf::mon::UsageOutcome::Observed) ? 0 : 8;
                }
            }
            QApplication::exit(exitCode);
        };
        if (hardware.isRunning() || !hardware.latest())
            QObject::connect(&hardware, &gf::hw::HardwareService::detectionFinished, &app, run, Qt::SingleShotConnection);
        else
            QTimer::singleShot(0, &app, run);
        exitCode = app.exec(); // the code passed to QApplication::exit, including early exits
        gf::log::shutdown();
        return exitCode;
    }

    if (cli.isSet(dispatchOpt)) {
        // Headless multi-GPU batch (class C): same dispatcher as the Workload Lab's job panel.
        int exitCode = 2;
        auto run = [&] {
            const QList<gf::dispatch::Worker> workers = gf::dispatch::workersFrom(*hardware.latest());
            QString error;
            const auto use = gf::dispatch::selectWorkers(workers, cli.value(dispatchGpusOpt), &error);
            const QStringList inputs = gf::dispatch::inputFiles(cli.value(dispatchInputsOpt));
            const QString pattern = cli.value(dispatchPatternOpt);
            const QString outDir = QDir::toNativeSeparators(QFileInfo(cli.value(dispatchOutputOpt).isEmpty() ? data.tempDir() + QStringLiteral("/dispatch")
                                                                                                              : cli.value(dispatchOutputOpt)).absoluteFilePath());
            if (!use || inputs.isEmpty()) {
                qCCritical(lcApp).noquote() << (use ? QStringLiteral("No input files.") : error);
                QApplication::exit(2);
                return;
            }
            const QString command = cli.value(dispatchOpt);
            const gf::dispatch::Executor executor = command.compare(QLatin1StringView("decode"), Qt::CaseInsensitive) == 0
                ? gf::dispatch::decodeExecutor() : gf::dispatch::commandExecutor(command);
            QStringList names;
            for (int w : *use)
                names << QStringLiteral("%1 (DXGI %2)").arg(workers[w].name).arg(workers[w].dxgiIndex);
            qCInfo(lcApp).noquote() << "Dispatch:" << inputs.size() << "job(s) on" << names.join(QStringLiteral(", "));
            const int reps = cli.value(dispatchCompareOpt).toInt();
            if (cli.isSet(dispatchCompareOpt)) {
                if (use->size() < 2 || reps < 1) {
                    qCCritical(lcApp).noquote() << "--dispatch-compare needs at least two GPUs and n >= 1";
                    QApplication::exit(2);
                    return;
                }
                const gf::dispatch::Experiment e = gf::dispatch::compareConfigurations(
                    workers, *use, inputs, outDir, pattern, executor, std::min(reps, 50), nullptr,
                    [](const QString& stage) { qCInfo(lcApp).noquote() << "  " << stage; });
                for (const gf::dispatch::ConfigResult& c : e.configs)
                    qCInfo(lcApp).noquote() << QStringLiteral("  %1: %2 runs, mean %3 ms (sd %4)%5").arg(c.label).arg(c.wall.n)
                                                   .arg(c.wall.mean, 0, 'f', 0).arg(c.wall.stddev, 0, 'f', 0).arg(c.allOk ? QString() : QStringLiteral(", FAILED jobs"));
                qCInfo(lcApp).noquote() << "Result:" << e.verdict;
                for (const QString& n : e.notes)
                    qCInfo(lcApp).noquote() << "  " << n;
                exitCode = std::all_of(e.configs.begin(), e.configs.end(), [](const gf::dispatch::ConfigResult& c) { return c.allOk; }) ? 0 : 8;
            } else {
                const QList<gf::dispatch::Job> jobs = gf::dispatch::jobsFor(inputs, outDir, pattern);
                const gf::dispatch::RunResult r = gf::dispatch::runJobs(workers, *use, jobs, executor);
                for (const gf::dispatch::JobResult& j : r.jobs)
                    qCInfo(lcApp).noquote() << QStringLiteral("  job %1 on %2: %3 in %4 ms%5").arg(j.job).arg(workers.value(j.worker).name,
                                                   j.ok ? QStringLiteral("ok") : QStringLiteral("FAILED")).arg(j.ms, 0, 'f', 0)
                                                   .arg(j.ok ? (j.checksum ? QStringLiteral(" (%1)").arg(*j.checksum) : QString()) : QStringLiteral(" — ") + j.error);
                qCInfo(lcApp).noquote() << QStringLiteral("Dispatch finished in %1 ms, %2 failed").arg(r.wallMs, 0, 'f', 0).arg(r.failed());
                exitCode = r.failed() == 0 ? 0 : 8;
            }
            QApplication::exit(exitCode);
        };
        if (hardware.isRunning() || !hardware.latest())
            QObject::connect(&hardware, &gf::hw::HardwareService::detectionFinished, &app, run, Qt::SingleShotConnection);
        else
            QTimer::singleShot(0, &app, run);
        exitCode = app.exec(); // the code passed to QApplication::exit, including early exits
        gf::log::shutdown();
        return exitCode;
    }

    if (cli.isSet(verifyAppOpt)) {
        // Headless per-application verification: same observer and evaluation as the GPU Preferences page.
        const QString exe = QFileInfo(cli.value(verifyAppOpt)).absoluteFilePath();
        if (!QFileInfo::exists(exe)) {
            qCCritical(lcApp).noquote() << "No such program:" << QDir::toNativeSeparators(exe);
            gf::log::shutdown();
            return 2;
        }
        int exitCode = 8;
        auto run = [&] {
            gf::mon::ObserveOptions o;
            o.exe = QDir::toNativeSeparators(exe);
            o.arguments = cli.positionalArguments();
            o.seconds = std::clamp(cli.value(verifySecondsOpt).toInt(), 3, 600);
            const gf::hw::DetectionResult detection = *hardware.latest();
            const gf::mon::UsageEvidence e = gf::mon::observeUsage(gf::mon::targetsFrom(detection), o);
            const gf::mon::UsageVerdict v = gf::ui::evaluateForApp(e, preferences.manager(), detection);
            qCInfo(lcApp).noquote() << "Verify app:" << o.exe << (o.arguments.isEmpty() ? QString() : o.arguments.join(u' '));
            qCInfo(lcApp).noquote() << "  " << v.headline;
            for (const gf::mon::UsageStage& s : v.stages)
                qCInfo(lcApp).noquote() << "    " << s.name << ":" << gf::mon::stageStateText(s.state) << "-" << s.detail;
            for (const QString& d : v.details)
                qCInfo(lcApp).noquote() << "    " << d;
            for (const QString& x : v.renderingExecutables)
                qCInfo(lcApp).noquote() << "    rendering executable:" << x;
            const bool good = v.outcome == gf::mon::UsageOutcome::Confirmed || v.outcome == gf::mon::UsageOutcome::ConfirmedUnnamed ||
                              v.outcome == gf::mon::UsageOutcome::Observed;
            exitCode = good ? 0 : 8;
            QApplication::exit(exitCode);
        };
        if (hardware.isRunning() || !hardware.latest())
            QObject::connect(&hardware, &gf::hw::HardwareService::detectionFinished, &app, run, Qt::SingleShotConnection);
        else
            QTimer::singleShot(0, &app, run);
        exitCode = app.exec(); // the code passed to QApplication::exit, including early exits
        gf::log::shutdown();
        return exitCode;
    }

    if (cli.isSet(labOpt)) {
        // Headless Workload Lab: same runner, storage and analysis as the Workload Lab page.
        const QString which = cli.value(labOpt).toLower();
        const QString size = cli.value(labSizeOpt).toLower();
        QStringList queue;
        if (which == QLatin1StringView("split") || which == QLatin1StringView("all"))
            queue << QStringLiteral("split");
        if (which == QLatin1StringView("decode") || which == QLatin1StringView("all"))
            queue << QStringLiteral("decode");
        if (queue.isEmpty() || !QStringList{ QStringLiteral("quick"), QStringLiteral("standard"), QStringLiteral("heavy") }.contains(size)) {
            qCCritical(lcApp).noquote() << "Use --run-lab split|decode|all and --lab-size quick|standard|heavy";
            gf::log::shutdown();
            return 2;
        }
        monitor.setPaused(true);
        bool allGood = true;
        auto next = [&] {
            const gf::Settings& s = context.settings();
            const gf::bench::LabOptions options = gf::bench::labOptions(s.benchmarkWarmupRuns, s.benchmarkMeasuredRuns, size, lab.benchmarksDir());
            const QString experiment = queue.takeFirst();
            const gf::hw::DetectionResult detection = *hardware.latest();
            qCInfo(lcApp).noquote() << "Workload Lab:" << experiment << "experiment, size" << size;
            lab.startJob([experiment, options, detection](const gf::bench::Progress& p, const std::atomic<bool>* c) {
                return experiment == QLatin1StringView("split") ? gf::bench::runSplitExperiment(detection, options, p, c)
                                                                : gf::bench::runDecodeExperiment(detection, options, p, c);
            }, s.benchmarkRetentionDays);
        };
        QObject::connect(&lab, &gf::bench::BenchmarkService::finished, &app, [&](const gf::bench::Session& session) {
            allGood = allGood && lab.lastSaveError().isEmpty();
            bool decode = false;
            for (const gf::bench::BenchmarkResult& r : session.results) {
                allGood = allGood && r.status == gf::bench::Status::Completed && (!r.correct.has_value() || *r.correct);
                decode = decode || r.testId.startsWith(QLatin1StringView("lab.decode"));
            }
            const gf::bench::LabAnalysis analysis = decode ? gf::bench::analyzeDecode(session) : gf::bench::analyzeSplit(session);
            qCInfo(lcApp).noquote() << analysis.headline;
            for (const QString& f : analysis.findings)
                qCInfo(lcApp).noquote() << "  " << f;
            if (queue.isEmpty())
                QApplication::exit(allGood ? 0 : 7);
            else
                next();
        });
        if (hardware.isRunning() || !hardware.latest())
            QObject::connect(&hardware, &gf::hw::HardwareService::detectionFinished, &app, next, Qt::SingleShotConnection);
        else
            next();
        const int code = app.exec();
        gf::log::shutdown();
        return code;
    }

    if (cli.isSet(exportOpt)) {
        // Headless export: wait until detection, two monitoring samples and a per-process sample with
        // rates exist (bounded), then write the report and exit.
        QString base = cli.value(exportOpt);
        if (base.endsWith(QLatin1StringView(".md"), Qt::CaseInsensitive) || base.endsWith(QLatin1StringView(".json"), Qt::CaseInsensitive))
            base = base.section(u'.', 0, -2);
        base = QFileInfo(base).absoluteFilePath();
        monitor.setProcessTracking(true);
        int samples = 0;
        auto tryExport = [&] {
            const bool ready = hardware.latest() && samples >= 2 && monitor.latestProcesses() && monitor.latestProcesses()->ratesReady;
            if (!ready)
                return false;
            QStringList written;
            const QString error = gf::ui::exportReport(gf::diag::buildReport(gf::ui::collectReportInputs(context)), base, &written);
            if (!error.isEmpty())
                qCCritical(lcApp).noquote() << error;
            QApplication::exit(error.isEmpty() ? 0 : 6);
            return true;
        };
        QObject::connect(&monitor, &gf::mon::MonitorService::updated, &app, [&] { ++samples; tryExport(); });
        QObject::connect(&monitor, &gf::mon::MonitorService::processesUpdated, &app, [&] { tryExport(); });
        QTimer::singleShot(8000, &app, [&] { // never hang: export what exists after 8 s
            samples = std::max(samples, 2);
            if (!tryExport()) {
                const QString error = gf::ui::exportReport(gf::diag::buildReport(gf::ui::collectReportInputs(context)), base, nullptr);
                QApplication::exit(error.isEmpty() ? 0 : 6);
            }
        });
        const int code = app.exec();
        monitor.setProcessTracking(false);
        gf::log::shutdown();
        return code;
    }

    // ---- window ------------------------------------------------------------------
    gf::ui::MainWindow window(context);
    if (cli.isSet(pageOpt) && !window.showPage(cli.value(pageOpt)))
        qCWarning(lcApp).noquote() << "Unknown page id" << cli.value(pageOpt);

    const bool automated = cli.isSet(smokeOpt) || cli.isSet(shotOpt);
    if (context.settings().startMinimized && !automated)
        window.showMinimized();
    else
        window.show();

    if (cli.isSet(shotOpt)) {
        const QString file = cli.value(shotOpt);
        const bool full = cli.isSet(fullShotOpt);
        const int delay = std::max(0, cli.value(shotDelayOpt).toInt());
        auto capture = [&window, file, full, delay] {
            QTimer::singleShot(delay, &window, [&window, file, full] {
                QWidget* target = &window;
                if (full) {
                    for (QScrollArea* area : window.findChildren<QScrollArea*>()) {
                        if (area->isVisible() && area->widget()) {
                            target = area->widget();
                            break;
                        }
                    }
                }
                const bool ok = target->grab().save(file, "PNG");
                qCInfo(lcApp).noquote() << "Screenshot" << (ok ? "saved to" : "FAILED for") << QDir::toNativeSeparators(file);
                QApplication::exit(ok ? 0 : 2);
            });
        };
        // Capture the finished state rather than "Detecting…".
        if (hardware.isRunning())
            QObject::connect(&hardware, &gf::hw::HardwareService::detectionFinished, &window, capture, Qt::SingleShotConnection);
        else
            capture();
    } else if (cli.isSet(smokeOpt)) {
        bool ok = false;
        const int delay = cli.value(smokeOpt).toInt(&ok);
        QTimer::singleShot(ok && delay > 0 ? delay : 1500, &window, [&window] {
            const bool visible = window.isVisible();
            qCInfo(lcApp) << "Smoke test" << (visible ? "passed: main window visible" : "FAILED: main window not visible");
            QApplication::exit(visible ? 0 : 3);
        });
    }

    const int rc = app.exec();
    qCInfo(lcApp) << "Exiting with code" << rc;
    gf::log::shutdown();
    return rc;
}
