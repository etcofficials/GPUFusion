// Pure tests for generic per-application profiles: JSON, ids, the per-file store, version-folder
// resolution, the per-mode capability assessment, the SDK launch environment, the per-mode verdict and
// applying a profile (against an in-memory registry).
#include "preferences/PreferenceModel.h"
#include "profiles/AppDiscovery.h"
#include "profiles/AppProfile.h"
#include "profiles/KnownOptions.h"
#include "profiles/ProfileActions.h"
#include "profiles/ProfileStore.h"
#include "profiles/Shortcuts.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

#include <map>

using namespace gf;
using namespace gf::profiles;
using namespace gf::pref;
using namespace gf::mon;

namespace {

class FakeStore : public IPreferenceStore {
public:
    std::map<QString, QString> values;
    QStringList names() const override
    {
        QStringList n;
        for (const auto& [k, v] : values)
            n << k;
        return n;
    }
    std::optional<QString> read(const QString& name) const override
    {
        for (const auto& [k, v] : values) {
            if (k.compare(name, Qt::CaseInsensitive) == 0)
                return v;
        }
        return std::nullopt;
    }
    bool write(const QString& name, const QString& data, QString*) override
    {
        values[name] = data;
        return true;
    }
    bool remove(const QString& name, QString*) override
    {
        values.erase(name);
        return true;
    }
};

const SelectableGpu kNvidia{ QStringLiteral("NVIDIA GeForce GT 730"), 0x10DE, QStringLiteral("00000000_00007DEC"), false, 0 };
const SelectableGpu kIntel{ QStringLiteral("Intel(R) HD Graphics"), 0x8086, QStringLiteral("00000000_0000870A"), true, 1 };
const QList<SelectableGpu> kGpus{ kNvidia, kIntel };
constexpr quint64 kNvLuid = 0x7DEC, kIntelLuid = 0x870A;

VerificationStep step(GpuPreference p, const SelectableGpu& got)
{
    const ProbeAdapter a{ got.name, got.vendorId, got.luid };
    const ProbeAdapter nv{ kNvidia.name, kNvidia.vendorId, kNvidia.luid };
    VerificationStep s;
    s.preference = p;
    s.ok = true;
    s.report.d3d11Default = a;
    s.report.minimumPower = nv;
    s.report.highPerformance = nv;
    return s;
}

// The reference PC: every preference renders on the GT 730.
VerificationResult referencePc()
{
    VerificationResult v;
    v.restored = true;
    for (auto p : { GpuPreference::NotSet, GpuPreference::LetWindowsDecide, GpuPreference::PowerSaving, GpuPreference::HighPerformance })
        v.steps << step(p, kNvidia);
    return v;
}

CapabilityInputs referenceInputs()
{
    CapabilityInputs in;
    in.gpus = kGpus;
    in.probeVerification = referencePc();
    in.dualGpu.verdict = QStringLiteral("No: one rendering workload cannot be split across these GPUs.");
    in.dualGpu.cases << QStringLiteral("not possible here (Direct3D 12 is missing on Intel)");
    return in;
}

UsageEvidence evidence()
{
    UsageEvidence e;
    e.targetExe = QStringLiteral("C:\\Tools\\app.exe");
    e.launched = true;
    e.passes = 6;
    e.adapters = { { kNvLuid, kNvidia.name }, { kIntelLuid, kIntel.name } };
    return e;
}

void addWork(UsageEvidence& e, quint64 luid, const QString& engine, double percent)
{
    for (int pass = 1; pass <= 5; ++pass)
        e.observations << UsageObservation{ pass, 100, e.targetExe, luid, { EngineSample{ 0, engine, MetricValue::ok(percent) } } };
}

void touch(const QString& path)
{
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile f(path);
    QVERIFY(f.open(QIODevice::WriteOnly));
}

} // namespace

class TestProfiles : public QObject {
    Q_OBJECT
    QTemporaryDir m_dir;

private Q_SLOTS:
    void initTestCase() { QVERIFY(m_dir.isValid()); }

    // ---- identity + JSON ------------------------------------------------------------------------
    void idsAreStableAndCaseInsensitive()
    {
        const QString a = profileIdFor(QStringLiteral("C:\\Games\\Some Game\\Game.exe"));
        QCOMPARE(a, profileIdFor(QStringLiteral("c:/games/some game/GAME.EXE")));
        QVERIFY(a.startsWith(QStringLiteral("game-")));
        QVERIFY(a != profileIdFor(QStringLiteral("C:\\Games\\Other\\Game.exe"))); // same name, other program
    }

    void jsonRoundTrip()
    {
        AppProfile p = newProfile(QStringLiteral("C:\\Tools\\app.exe"), QStringLiteral("My App"));
        p.arguments = QStringLiteral("--render 7 \"a b\"");
        p.mode = GpuMode::Both;
        p.shareGpuChoiceWithApp = false;
        p.appliedValue = QStringLiteral("GpuPreference=2;");
        p.created = QDateTime(QDate(2026, 10, 1), QTime(12, 0));
        p.modified = p.created.addSecs(60);
        ProfileVerification v;
        v.time = p.modified;
        v.mode = GpuMode::Both;
        v.outcome = QStringLiteral("partly");
        v.headline = QStringLiteral("Both GPUs used");
        v.stages << QStringLiteral("x: Passed — y");
        v.renderingExecutables << p.exePath;
        v.confirmed = true;
        p.lastVerification = v;

        const auto back = AppProfile::fromJson(QJsonDocument::fromJson(QJsonDocument(p.toJson()).toJson()).object());
        QVERIFY(back.has_value());
        QCOMPARE(back->id, p.id);
        QCOMPARE(back->name, p.name);
        QCOMPARE(back->arguments, p.arguments);
        QCOMPARE(back->mode, GpuMode::Both);
        QCOMPARE(back->shareGpuChoiceWithApp, false);
        QCOMPARE(back->appliedValue, p.appliedValue);
        QCOMPARE(back->created, p.created);
        QVERIFY(back->lastVerification.has_value());
        QCOMPARE(back->lastVerification->outcome, v.outcome);
        QCOMPARE(back->lastVerification->stages, v.stages);
        QVERIFY(back->lastVerification->confirmed);

        QJsonObject edited = p.toJson();
        edited.insert(QStringLiteral("id"), QStringLiteral("somebody-else"));   // ids are derived, never trusted
        QCOMPARE(AppProfile::fromJson(edited)->id, p.id);
        edited.insert(QStringLiteral("schemaVersion"), 2);                     // a newer format is not misread
        QVERIFY(!AppProfile::fromJson(edited).has_value());
        QCOMPARE(modeFromKey(QStringLiteral("discrete")), GpuMode::Discrete);
        QVERIFY(!modeFromKey(QStringLiteral("turbo")).has_value());
    }

    // ---- store ------------------------------------------------------------------------------------
    void storeKeepsOneFilePerProgram()
    {
        ProfileStore store(m_dir.filePath(QStringLiteral("profiles")));
        AppProfile a = newProfile(m_dir.filePath(QStringLiteral("a/zeta.exe")));
        AppProfile b = newProfile(m_dir.filePath(QStringLiteral("b/alpha.exe")));
        QString error;
        QVERIFY2(store.save(a, &error), qPrintable(error));
        QVERIFY2(store.save(b, &error), qPrintable(error));
        const QString aFile = QDir(store.directory()).filePath(a.id + QStringLiteral(".json"));
        QFile fa(aFile);
        QVERIFY(fa.open(QIODevice::ReadOnly));
        const QByteArray aBefore = fa.readAll();
        fa.close();

        b.mode = GpuMode::Discrete;
        QVERIFY(store.save(b, &error));
        QFile fa2(aFile);
        QVERIFY(fa2.open(QIODevice::ReadOnly));
        QCOMPARE(fa2.readAll(), aBefore); // saving b never rewrites a

        const QList<AppProfile> all = store.list();
        QCOMPARE(all.size(), 2);
        QCOMPARE(all[0].name, QStringLiteral("alpha")); // sorted by name
        QCOMPARE(store.find(b.exePath)->mode, GpuMode::Discrete);
        QCOMPARE(store.find(b.id)->exePath, b.exePath);
        QVERIFY(store.find(b.id)->created.isValid());
        QVERIFY(store.remove(b.id, &error));
        QVERIFY(!store.find(b.id).has_value());
        QCOMPARE(store.list().size(), 1);
    }

    // A damaged profile file (e.g. a failing disk) is skipped, left untouched, and does not hide the others.
    void damagedProfileFilesAreSkippedNotLost()
    {
        ProfileStore store(m_dir.filePath(QStringLiteral("profiles-damaged")));
        AppProfile good = newProfile(m_dir.filePath(QStringLiteral("d/good.exe")));
        QString error;
        QVERIFY(store.save(good, &error));
        const QString bad = QDir(store.directory()).filePath(QStringLiteral("broken-000000000000.json"));
        QFile f(bad);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write("{\"schemaVersion\":1,\"exePath\":\"C:\\\\x.e");   // truncated
        f.close();
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("GPU profile ignored")));
        const QList<AppProfile> all = store.list();
        QCOMPARE(all.size(), 1);
        QCOMPARE(all[0].id, good.id);
        QVERIFY(QFile::exists(bad));                                     // left for the user to inspect
    }

    // Backup / transfer: everything round-trips except what describes the PC it was made on.
    void exportAndImport()
    {
        ProfileStore source(m_dir.filePath(QStringLiteral("export-src")));
        AppProfile here = newProfile(m_dir.filePath(QStringLiteral("x/here.exe")));
        touch(here.exePath);
        here.mode = GpuMode::Integrated;
        here.arguments = QStringLiteral("--use-adapter-luid={luid_pair}");
        here.appliedValue = QStringLiteral("GpuPreference=1;");
        ProfileVerification v;
        v.time = QDateTime::currentDateTime();
        v.mode = GpuMode::Integrated;
        v.outcome = QStringLiteral("confirmed");
        v.confirmed = true;
        here.lastVerification = v;
        AppProfile gone = newProfile(QStringLiteral("Z:\\Not\\Installed\\gone.exe"), QStringLiteral("Gone"));
        QString error;
        QVERIFY(source.save(here, &error));
        QVERIFY(source.save(gone, &error));
        const QByteArray doc = exportProfiles(source.list());

        ProfileStore target(m_dir.filePath(QStringLiteral("export-dst")));
        ImportResult r = importProfiles(target, doc, false);
        QVERIFY2(r.ok, qPrintable(r.error));
        QCOMPARE(r.imported, 2);
        QCOMPARE(r.missingPrograms, QStringList{ QStringLiteral("Gone") });
        QVERIFY(r.summary().contains(QStringLiteral("No Windows setting was changed")));
        const AppProfile in = *target.find(here.exePath);
        QCOMPARE(in.mode, GpuMode::Integrated);
        QCOMPARE(in.arguments, here.arguments);                 // the placeholder, valid on any PC
        QVERIFY(!in.lastVerification.has_value());              // results describe the other PC
        QVERIFY(in.appliedValue.isEmpty());

        AppProfile local = in;                                   // a profile changed here is kept ...
        local.mode = GpuMode::Discrete;
        QVERIFY(target.save(local, &error));
        r = importProfiles(target, doc, false);
        QCOMPARE(r.skipped, 2);
        QCOMPARE(target.find(here.exePath)->mode, GpuMode::Discrete);
        r = importProfiles(target, doc, true);                   // ... unless replacing was chosen
        QCOMPARE(r.replaced, 2);
        QCOMPARE(target.find(here.exePath)->mode, GpuMode::Integrated);

        QVERIFY(!importProfiles(target, "{}", false).ok);
        QVERIFY(importProfiles(target, "not json", false).error.contains(QStringLiteral("not a GPU Fusion profile export")));
        QVERIFY(importProfiles(target, R"({"gpuFusionProfiles":2,"profiles":[]})", false).error.contains(QStringLiteral("newer")));
    }

    // Automatic backup: written after a change, skipped when nothing changed, rotated to `keep`, other
    // files untouched; an import triggers one backup, not one per profile.
    void automaticBackup()
    {
        const QString folder = m_dir.filePath(QStringLiteral("backups"));
        QDir().mkpath(folder);
        touch(folder + QStringLiteral("/my-notes.txt"));                    // someone else's file in the folder
        ProfileStore store(m_dir.filePath(QStringLiteral("backup-src")));
        int notifications = 0;
        QList<BackupResult> results;
        store.setChangeCallback([&] {
            ++notifications;
            results << backupProfiles(store.list(), folder, 3);
        });
        QString error;
        AppProfile a = newProfile(m_dir.filePath(QStringLiteral("b/a.exe")));
        QVERIFY(store.save(a, &error));
        QCOMPARE(notifications, 1);
        QVERIFY(results.last().written);
        QVERIFY(backupProfiles(store.list(), folder, 3).unchanged);       // same profiles: nothing written

        for (int i = 0; i < 4; ++i) {                                       // 4 more changes, keep 3
            a.mode = i % 2 ? GpuMode::Integrated : GpuMode::Discrete;
            QTest::qWait(5);                                                // distinct time stamps
            QVERIFY(store.save(a, &error));
        }
        const QStringList backups = QDir(folder).entryList({ QStringLiteral("gpufusion-profiles-*.json") }, QDir::Files);
        QCOMPARE(backups.size(), 3);
        QVERIFY(QFile::exists(folder + QStringLiteral("/my-notes.txt")));    // never touched
        // The newest backup restores the current state.
        QFile newest(QDir(folder).filePath(backups.last()));
        QVERIFY(newest.open(QIODevice::ReadOnly));
        ProfileStore restored(m_dir.filePath(QStringLiteral("backup-restored")));
        QVERIFY(importProfiles(restored, newest.readAll(), false).ok);
        QCOMPARE(restored.find(a.exePath)->mode, a.mode);

        // Import of several profiles: one notification for the whole batch.
        ProfileStore other(m_dir.filePath(QStringLiteral("backup-other")));
        QList<AppProfile> many;
        for (int i = 0; i < 5; ++i)
            many << newProfile(m_dir.filePath(QStringLiteral("many/p%1.exe").arg(i)));
        notifications = 0;
        QVERIFY(importProfiles(store, exportProfiles(many), false).ok);
        QCOMPARE(notifications, 1);
        QCOMPARE(store.list().size(), 6);

        QVERIFY(onSameVolume(m_dir.path(), folder));
        QVERIFY(!backupProfiles(store.list(), QString(), 3).error.isEmpty());

        // A damaged profile file: no backup is written, so the backups that still hold it are not rotated away.
        const QStringList before = QDir(folder).entryList({ QStringLiteral("gpufusion-profiles-*.json") }, QDir::Files);
        QFile damaged(QDir(store.directory()).filePath(QStringLiteral("damaged-000000000000.json")));
        QVERIFY(damaged.open(QIODevice::WriteOnly));
        damaged.write("{\"schemaVersion\":1,\"exePath\":\"C:\\\\x");
        damaged.close();
        QTest::ignoreMessage(QtWarningMsg, QRegularExpression(QStringLiteral("GPU profile ignored")));
        const BackupResult refused = backupStore(store, folder, 3);
        QVERIFY(!refused.written);
        QVERIFY2(refused.error.contains(QStringLiteral("could not be read")), qPrintable(refused.error));
        QCOMPARE(QDir(folder).entryList({ QStringLiteral("gpufusion-profiles-*.json") }, QDir::Files), before);
    }

    void versionFoldersResolveToTheNewestCopy()
    {
        const QString root = m_dir.filePath(QStringLiteral("Discordish"));
        touch(root + QStringLiteral("/app-1.0.9188/Discordish.exe"));
        touch(root + QStringLiteral("/app-1.0.9265/Discordish.exe"));
        touch(root + QStringLiteral("/app-1.0.9300/readme.txt"));  // newer, but without the executable
        touch(root + QStringLiteral("/Discordish.exe"));           // launcher in the root
        const QString old = QDir::toNativeSeparators(root + QStringLiteral("/app-1.0.9188/Discordish.exe"));
        QCOMPARE(resolveCurrentExecutable(old), QDir::toNativeSeparators(root + QStringLiteral("/app-1.0.9265/Discordish.exe")));
        const QString launcher = QDir::toNativeSeparators(root + QStringLiteral("/Discordish.exe"));
        QCOMPARE(resolveCurrentExecutable(launcher), launcher);    // not in a version folder: unchanged
        const QString plain = QDir::toNativeSeparators(m_dir.filePath(QStringLiteral("x/1.2/tool.exe")));
        touch(plain);
        QCOMPARE(resolveCurrentExecutable(plain), plain);          // no newer sibling
    }

    // ---- capabilities -----------------------------------------------------------------------------
    void referencePcCapabilities()
    {
        const AppProfile p = newProfile(QStringLiteral("C:\\Tools\\app.exe"));
        const CapabilityInputs in = referenceInputs();

        const ModeCapability automatic = assessMode(p, GpuMode::Automatic, in);
        QCOMPARE(automatic.status, ModeStatus::Verified);
        QCOMPARE(automatic.write, GpuPreference::NotSet);

        const ModeCapability intel = assessMode(p, GpuMode::Integrated, in);
        QCOMPARE(intel.status, ModeStatus::NeedsAppSupport);       // Windows cannot place programs on Intel here
        QCOMPARE(intel.capabilityClass, QStringLiteral("A"));
        QVERIFY(!intel.write.has_value());
        QVERIFY(intel.summary.contains(QStringLiteral("SDK")));
        QVERIFY(intel.reasons.join(u' ').contains(QStringLiteral("Hypothesis")));

        const ModeCapability nv = assessMode(p, GpuMode::Discrete, in);
        QCOMPARE(nv.status, ModeStatus::Verified);
        QCOMPARE(nv.write, GpuPreference::HighPerformance);

        const ModeCapability both = assessMode(p, GpuMode::Both, in);
        QCOMPARE(both.status, ModeStatus::NeedsAppSupport);
        QCOMPARE(both.capabilityClass, QStringLiteral("D"));
        QVERIFY(both.reasons.join(u' ').contains(QStringLiteral("class E")));
        QVERIFY(!both.write.has_value());
        QCOMPARE(assessAllModes(p, in).size(), 4);
    }

    void verificationForThisProgramOverridesTheGenericStatus()
    {
        AppProfile p = newProfile(QStringLiteral("C:\\Tools\\app.exe"));
        ProfileVerification v;
        v.time = QDateTime::currentDateTime();
        v.mode = GpuMode::Integrated;
        v.outcome = QStringLiteral("confirmed-unnamed");
        v.headline = QStringLiteral("Confirmed on Intel");
        v.confirmed = true;
        p.lastVerification = v;
        const CapabilityInputs in = referenceInputs();
        QCOMPARE(assessMode(p, GpuMode::Integrated, in).status, ModeStatus::Verified);
        QVERIFY(!assessMode(p, GpuMode::Integrated, in).write.has_value()); // still no Windows value
        QCOMPARE(assessMode(p, GpuMode::Both, in).status, ModeStatus::NeedsAppSupport); // other modes unaffected

        v.mode = GpuMode::Both;
        v.outcome = QStringLiteral("partly");
        p.lastVerification = v;
        QCOMPARE(assessMode(p, GpuMode::Both, in).status, ModeStatus::Verified);

        CapabilityInputs one = in;
        one.gpus = { kNvidia };
        QCOMPARE(assessMode(newProfile(p.exePath), GpuMode::Both, one).status, ModeStatus::Unsupported);
    }

    // ---- launch environment -----------------------------------------------------------------------
    void launchEnvironmentForSdkPrograms()
    {
        AppProfile p = newProfile(QStringLiteral("C:\\Tools\\app.exe"));
        QVERIFY(requestedAdapterLuids(GpuMode::Automatic, kGpus).isEmpty());
        QCOMPARE(requestedAdapterLuids(GpuMode::Integrated, kGpus), QStringList{ kIntel.luid });
        QCOMPARE(requestedAdapterLuids(GpuMode::Discrete, kGpus), QStringList{ kNvidia.luid });
        QCOMPARE(requestedAdapterLuids(GpuMode::Both, kGpus), (QStringList{ kNvidia.luid, kIntel.luid }));

        p.mode = GpuMode::Both;
        const QHash<QString, QString> env = launchEnvironment(p, kGpus);
        QCOMPARE(env.value(QStringLiteral("GPUFUSION_MODE")), QStringLiteral("both"));
        QCOMPARE(env.value(QStringLiteral("GPUFUSION_ADAPTERS")), kNvidia.luid + u',' + kIntel.luid);
        QCOMPARE(env.value(QStringLiteral("GPUFUSION_PROFILE")), p.id);
        QCOMPARE(env.value(QStringLiteral("GPUFUSION_ENV_VERSION")), QStringLiteral("1"));
        p.shareGpuChoiceWithApp = false;
        QVERIFY(launchEnvironment(p, kGpus).isEmpty());

        p.arguments = QStringLiteral("--render 7 \"C:\\My Files\\in.mp4\"");
        QCOMPARE(profileArguments(p, kGpus), (QStringList{ QStringLiteral("--render"), QStringLiteral("7"), QStringLiteral("C:\\My Files\\in.mp4") }));
    }

    // A launch message says what was passed and that a started process proves nothing about its GPU;
    // Both never reads as "running on both GPUs". Store/Xbox packages get a caveat.
    void launchMessagesAreHonest()
    {
        AppProfile p = newProfile(QStringLiteral("C:\\Tools\\app.exe"));
        p.mode = GpuMode::Both;
        const QString both = launchMessage(p, 42);
        QVERIFY2(both.contains(QStringLiteral("process 42")) && both.contains(QStringLiteral("only programs that integrate the GPU Fusion SDK use them")),
                 qPrintable(both));
        QVERIFY(both.contains(QStringLiteral("does not show which GPU it uses")));
        QVERIFY(!both.contains(QStringLiteral("in mode")));
        p.mode = GpuMode::Integrated;
        QVERIFY(launchMessage(p, 7).contains(QStringLiteral("asks for")));
        QVERIFY(launchCaveats(p).isEmpty());

        QVERIFY(isPackagedInstall(QStringLiteral("G:\\XboxGames\\Roblox\\Content\\RobloxPlayerBeta.exe")));
        QVERIFY(isPackagedInstall(QStringLiteral("C:\\Program Files\\WindowsApps\\Vendor.App_1.0_x64__abc\\app.exe")));
        QVERIFY(isPackagedInstall(QStringLiteral("G:\\WpSystem\\S-1-5-21\\AppData\\Local\\Packages\\X\\LocalCache\\java\\bin\\javaw.exe")));
        QVERIFY(!isPackagedInstall(QStringLiteral("C:\\Tools\\app.exe")));
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString exe = dir.filePath(QStringLiteral("game.exe"));
        QVERIFY(!isPackagedInstall(exe));
        QFile manifest(dir.filePath(QStringLiteral("MicrosoftGame.config")));
        QVERIFY(manifest.open(QIODevice::WriteOnly));
        manifest.close();
        QVERIFY(isPackagedInstall(exe));
        p.exePath = exe;
        QVERIFY(launchMessage(p, 7).contains(QStringLiteral("Microsoft Store / Xbox package")));
    }

    // ---- GPU placeholders + known options -----------------------------------------------------------
    void gpuPlaceholdersAreFilledInForTheMode()
    {
        QCOMPARE(expandGpuPlaceholders(QStringLiteral("{luid}|{luid_pair}|{luid_high}|{luid_low}|{dxgi_index}|{vendor_id}|{vendor_hex}|{gpu_name}"), kIntel),
                 QStringLiteral("00000000_0000870A|0,34570|0|34570|1|32902|0x8086|Intel(R) HD Graphics"));
        SelectableGpu negativeHigh = kNvidia; // HighPart is signed
        negativeHigh.luid = QStringLiteral("FFFFFFFF_00000001");
        QCOMPARE(expandGpuPlaceholders(QStringLiteral("{luid_pair}"), negativeHigh), QStringLiteral("-1,1"));

        const QString args = QStringLiteral("--use-adapter-luid={luid_pair} --gpu \"{gpu_name}\" page.html");
        QVERIFY(usesGpuPlaceholders(args));
        QVERIFY(!usesGpuPlaceholders(QStringLiteral("--render 7 {input}")));
        QCOMPARE(launchArguments(args, GpuMode::Integrated, kGpus),
                 (QStringList{ QStringLiteral("--use-adapter-luid=0,34570"), QStringLiteral("--gpu"), QStringLiteral("Intel(R) HD Graphics"),
                               QStringLiteral("page.html") }));
        QCOMPARE(launchArguments(args, GpuMode::Discrete, kGpus).first(), QStringLiteral("--use-adapter-luid=0,32236"));
        QCOMPARE(launchArguments(args, GpuMode::Both, kGpus).first(), QStringLiteral("--use-adapter-luid=0,32236"));
        // Automatic, or no GPU for the mode: arguments with a placeholder are left out, the rest kept.
        QCOMPARE(launchArguments(args, GpuMode::Automatic, kGpus), (QStringList{ QStringLiteral("--gpu"), QStringLiteral("page.html") }));
        QCOMPARE(launchArguments(args, GpuMode::Integrated, { kNvidia }), (QStringList{ QStringLiteral("--gpu"), QStringLiteral("page.html") }));
    }

    void placeholderArgumentsMakeAnUnenforceableModeExpected()
    {
        AppProfile p = newProfile(QStringLiteral("C:\\Browser\\msedge.exe"));
        const CapabilityInputs in = referenceInputs();
        QCOMPARE(assessMode(p, GpuMode::Integrated, in).status, ModeStatus::NeedsAppSupport);
        QVERIFY(assessMode(p, GpuMode::Integrated, in).reasons.join(u' ').contains(QStringLiteral("{luid_pair}"))); // the hint
        p.arguments = QStringLiteral("--use-adapter-luid={luid_pair}");
        const ModeCapability c = assessMode(p, GpuMode::Integrated, in);
        QCOMPARE(c.status, ModeStatus::Expected);       // not "Verified" until this program was observed
        QVERIFY(!c.write.has_value());                  // still no Windows value
        QVERIFY(c.mechanism.contains(QStringLiteral("own GPU option")));
        QCOMPARE(assessMode(p, GpuMode::Discrete, in).status, ModeStatus::Verified); // unaffected

        CapabilityInputs unverified = in;   // no probe verification yet: Windows unknown, the option still applies
        unverified.probeVerification.reset();
        QCOMPARE(assessMode(p, GpuMode::Integrated, unverified).status, ModeStatus::Expected);
        QVERIFY(!assessMode(p, GpuMode::Integrated, unverified).write.has_value());
        p.arguments.clear();
        QCOMPARE(assessMode(p, GpuMode::Integrated, unverified).status, ModeStatus::Unknown);
    }

    void knownOptionsAreSuggestionsWithEvidence()
    {
        const QList<KnownGpuOption> builtIn = builtInKnownOptions();
        for (const KnownGpuOption& k : builtIn) {
            QVERIFY(!k.evidence.isEmpty());
            QVERIFY(usesGpuPlaceholders(k.arguments)); // every suggestion depends on the chosen GPU
        }
        const auto edge = knownOptionFor(QStringLiteral("C:\\Program Files (x86)\\Microsoft\\Edge\\Application\\MSEDGE.EXE"), builtIn);
        QVERIFY(edge.has_value());
        QCOMPARE(edge->arguments, QStringLiteral("--use-adapter-luid={luid_pair}"));
        QVERIFY(edge->evidence.startsWith(QStringLiteral("Verified")));
        QVERIFY(knownOptionFor(QStringLiteral("C:\\x\\chrome.exe"), builtIn)->evidence.contains(QStringLiteral("not verified")));
        QVERIFY(!knownOptionFor(QStringLiteral("C:\\Games\\game.exe"), builtIn).has_value());

        QCOMPARE(withKnownOption(QStringLiteral("page.html"), *edge), QStringLiteral("--use-adapter-luid={luid_pair} page.html"));
        QCOMPARE(withKnownOption(QStringLiteral("--use-adapter-luid={luid_pair} page.html"), *edge),
                 QStringLiteral("--use-adapter-luid={luid_pair} page.html")); // not twice
        QCOMPARE(withKnownOption(QString(), *edge), edge->arguments);

        // User entries come first and override; invalid ones are reported, not used.
        const QString file = m_dir.filePath(QStringLiteral("gpu-options.json"));
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(R"([{"executables":["msedge.exe"],"arguments":"--my-edge {dxgi_index}"},{"executables":[],"arguments":"x"},{"arguments":"y"}])");
        f.close();
        QStringList problems;
        const QList<KnownGpuOption> all = loadKnownOptions(file, &problems);
        QCOMPARE(problems.size(), 2);
        QCOMPARE(knownOptionFor(QStringLiteral("msedge.exe"), all)->arguments, QStringLiteral("--my-edge {dxgi_index}"));
        QVERIFY(knownOptionFor(QStringLiteral("msedge.exe"), all)->userDefined);
        QCOMPARE(loadKnownOptions(m_dir.filePath(QStringLiteral("missing.json"))).size(), builtIn.size());
    }

    // ---- per-mode verdict -------------------------------------------------------------------------
    void verdictPerMode()
    {
        ProfileVerification r;
        UsageEvidence nvOnly = evidence();
        addWork(nvOnly, kNvLuid, QStringLiteral("3D"), 50);
        QCOMPARE(evaluateProfileEvidence(nvOnly, GpuMode::Discrete, kGpus, &r).outcome, UsageOutcome::Confirmed);
        QVERIFY(r.confirmed);
        QCOMPARE(r.outcome, QStringLiteral("confirmed"));
        QCOMPARE(evaluateProfileEvidence(nvOnly, GpuMode::Integrated, kGpus, &r).outcome, UsageOutcome::WrongAdapter);
        QVERIFY(!r.confirmed);
        QCOMPARE(r.outcome, QStringLiteral("not-applied"));
        QCOMPARE(evaluateProfileEvidence(nvOnly, GpuMode::Automatic, kGpus, &r).outcome, UsageOutcome::Observed);
        QVERIFY(!r.confirmed);                                      // nothing was requested
        const UsageVerdict oneOfBoth = evaluateProfileEvidence(nvOnly, GpuMode::Both, kGpus, &r);
        QVERIFY(!r.confirmed);                                      // one GPU is not both
        QCOMPARE(r.outcome, QStringLiteral("one-gpu"));
        QVERIFY2(oneOfBoth.headline.startsWith(QStringLiteral("One GPU only")) && oneOfBoth.headline.contains(QStringLiteral("GT 730")),
                 qPrintable(oneOfBoth.headline));
        QVERIFY(!oneOfBoth.headline.contains(QStringLiteral("Confirmed")));

        UsageEvidence intelOnly = evidence();
        addWork(intelOnly, kIntelLuid, QStringLiteral("Engine 0"), 80);
        QCOMPARE(evaluateProfileEvidence(intelOnly, GpuMode::Integrated, kGpus, &r).outcome, UsageOutcome::ConfirmedUnnamed);
        QVERIFY(r.confirmed);

        UsageEvidence both = evidence();
        addWork(both, kNvLuid, QStringLiteral("3D"), 45);
        addWork(both, kIntelLuid, QStringLiteral("Engine 0"), 90); // Intel's driver does not name engines
        const UsageVerdict v = evaluateProfileEvidence(both, GpuMode::Both, kGpus, &r);
        QCOMPARE(v.outcome, UsageOutcome::BothAdapters);
        QVERIFY(r.confirmed);
        QCOMPARE(r.outcome, QStringLiteral("partly"));
        QVERIFY(v.headline.startsWith(QStringLiteral("Both GPUs used")));
        QVERIFY(v.details.join(u' ').contains(QStringLiteral("does not show that its rendering was split or faster")));
        // Discrete: it rendered on the GT 730; the Intel work of unknown type may be incidental.
        const UsageVerdict discrete = evaluateProfileEvidence(both, GpuMode::Discrete, kGpus, &r);
        QVERIFY(r.confirmed);
        QVERIFY(discrete.stages.last().detail.contains(QStringLiteral("may be incidental")));
    }

    // ---- discovery helpers ------------------------------------------------------------------------
    void discoveryHelpers()
    {
        QCOMPARE(executableFromRegistryValue(QStringLiteral("C:\\x\\app.exe,0")), QStringLiteral("C:\\x\\app.exe"));
        QCOMPARE(executableFromRegistryValue(QStringLiteral("\"C:\\Program Files\\x\\app.exe\",1")), QStringLiteral("C:\\Program Files\\x\\app.exe"));
        QCOMPARE(executableFromRegistryValue(QStringLiteral("\"C:\\x\\uninstall.exe\" /S")), QStringLiteral("C:\\x\\uninstall.exe"));
        QVERIFY(executableFromRegistryValue(QStringLiteral("C:\\x\\icon.ico")).isEmpty());
        QVERIFY(looksLikeHelperExecutable(QStringLiteral("C:\\x\\unins000.exe")));
        QVERIFY(looksLikeHelperExecutable(QStringLiteral("C:\\x\\Uninstall.exe")));
        QVERIFY(looksLikeHelperExecutable(QStringLiteral("C:\\x\\setup.exe")));
        QVERIFY(!looksLikeHelperExecutable(QStringLiteral("C:\\x\\brave.exe")));
        QVERIFY(!looksLikeHelperExecutable(QStringLiteral("C:\\x\\shotcut.exe")));
    }

    // Chromium-based programs are recognised by their runtime files, in both install layouts.
    void chromiumProgramsAreDetectedByTheirFiles()
    {
        const QString electron = m_dir.filePath(QStringLiteral("electronapp"));           // Electron: files next to the exe
        touch(electron + QStringLiteral("/ChatApp.exe"));
        touch(electron + QStringLiteral("/icudtl.dat"));
        touch(electron + QStringLiteral("/v8_context_snapshot.bin"));
        QCOMPARE(chromiumRuntimeFiles(electron + QStringLiteral("/ChatApp.exe")).size(), 2);

        const QString browser = m_dir.filePath(QStringLiteral("browser"));                // Chrome layout: runtime in <app>/<version>
        touch(browser + QStringLiteral("/SomeBrowser.exe"));
        touch(browser + QStringLiteral("/121.0.6167.85/icudtl.dat"));
        touch(browser + QStringLiteral("/121.0.6167.85/snapshot_blob.bin"));
        QVERIFY(chromiumRuntimeFiles(browser + QStringLiteral("/SomeBrowser.exe")).value(1).endsWith(QStringLiteral("snapshot_blob.bin")));

        const QString plain = m_dir.filePath(QStringLiteral("plain"));                    // icudtl alone (e.g. a Qt app) is not enough
        touch(plain + QStringLiteral("/tool.exe"));
        touch(plain + QStringLiteral("/icudtl.dat"));
        QVERIFY(chromiumRuntimeFiles(plain + QStringLiteral("/tool.exe")).isEmpty());

        const QList<KnownGpuOption> builtIn = builtInKnownOptions();
        const auto detected = suggestedOptionFor(electron + QStringLiteral("/ChatApp.exe"), builtIn);
        QVERIFY(detected.has_value());
        QCOMPARE(detected->arguments, QStringLiteral("--use-adapter-luid={luid_pair}"));
        QVERIFY(detected->evidence.contains(QStringLiteral("not verified with this program")));
        QVERIFY(!suggestedOptionFor(plain + QStringLiteral("/tool.exe"), builtIn).has_value());
        // A program known by name keeps its own entry (and evidence) even if it is also detected.
        const QString edgeDir = m_dir.filePath(QStringLiteral("edge"));
        touch(edgeDir + QStringLiteral("/msedge.exe"));
        touch(edgeDir + QStringLiteral("/icudtl.dat"));
        touch(edgeDir + QStringLiteral("/v8_context_snapshot.bin"));
        QVERIFY(suggestedOptionFor(edgeDir + QStringLiteral("/msedge.exe"), builtIn)->evidence.startsWith(QStringLiteral("Verified")));
    }

    // ---- variants -----------------------------------------------------------------------------------
    void variantsAreSeparateProfilesOfOneProgram()
    {
        const QString exe = QStringLiteral("C:\\Tools\\ffmpeg.exe");
        const QString mainId = profileIdFor(exe);
        QCOMPARE(profileIdFor(exe, QString()), mainId);                          // main ids unchanged
        QVERIFY(profileIdFor(exe, QStringLiteral("Intel")) != mainId);
        QCOMPARE(profileIdFor(exe, QStringLiteral("Intel")), profileIdFor(exe, QStringLiteral(" intel ")));
        QVERIFY(profileIdFor(exe, QStringLiteral("Intel")) != profileIdFor(exe, QStringLiteral("NVIDIA")));

        AppProfile base = newProfile(exe);
        base.mode = GpuMode::Discrete;
        base.appliedValue = QStringLiteral("GpuPreference=2;");
        QVERIFY(!newVariant(base, QStringLiteral("  ")).has_value());
        AppProfile v = *newVariant(base, QStringLiteral("Intel"));
        QCOMPARE(v.variant, QStringLiteral("Intel"));
        QVERIFY(v.appliedValue.isEmpty());
        QCOMPARE(displayName(v), QStringLiteral("ffmpeg — Intel"));
        const auto back = AppProfile::fromJson(v.toJson());
        QCOMPARE(back->variant, QStringLiteral("Intel"));
        QCOMPARE(back->id, v.id);
        QVERIFY(shortcutName(v).startsWith(QStringLiteral("ffmpeg — Intel (")));

        ProfileStore store(m_dir.filePath(QStringLiteral("variants")));
        QString error;
        QVERIFY(store.save(base, &error));
        QVERIFY(store.save(v, &error));
        QCOMPARE(store.list().size(), 2);
        QVERIFY(store.find(exe)->variant.isEmpty());                              // the path finds the main profile
        QCOMPARE(store.find(v.id)->variant, QStringLiteral("Intel"));
    }

    // Windows keeps one GPU preference per executable: a variant never uses, writes or removes it.
    void variantsNeverUseTheWindowsValue()
    {
        const CapabilityInputs in = referenceInputs();
        AppProfile v = *newVariant(newProfile(QStringLiteral("C:\\Tools\\tool.exe")), QStringLiteral("second"));
        const ModeCapability discrete = assessMode(v, GpuMode::Discrete, in);   // Verified via Windows for the main profile
        QCOMPARE(discrete.status, ModeStatus::NeedsAppSupport);
        QVERIFY(!discrete.write.has_value());
        QVERIFY(discrete.summary.contains(QStringLiteral("one GPU preference per program")));
        QVERIFY(!assessMode(v, GpuMode::Automatic, in).write.has_value());      // not even "remove"
        v.arguments = QStringLiteral("--gpu {dxgi_index}");
        QCOMPARE(assessMode(v, GpuMode::Discrete, in).status, ModeStatus::Expected);
        QCOMPARE(assessMode(v, GpuMode::Integrated, in).status, ModeStatus::Expected);
        QVERIFY(!assessMode(v, GpuMode::Discrete, in).write.has_value());

        const QString exe = QDir::toNativeSeparators(m_dir.filePath(QStringLiteral("variantapply/tool.exe")));
        touch(exe);
        FakeStore registry;
        registry.values[exe] = QStringLiteral("GpuPreference=2;");               // the main profile's value
        ChangeJournal journal(m_dir.filePath(QStringLiteral("variant-journal.json")));
        PreferenceManager manager(registry, journal);
        AppProfile real = *newVariant(newProfile(exe), QStringLiteral("Intel"));
        for (GpuMode m : { GpuMode::Integrated, GpuMode::Automatic, GpuMode::Discrete }) {
            real.mode = m;
            const ApplyResult r = applyProfile(real, manager, in);
            QVERIFY2(r.ok, qPrintable(r.message));
            QVERIFY(r.message.contains(QStringLiteral("no Windows setting changed")));
            QCOMPARE(registry.values[exe], QStringLiteral("GpuPreference=2;"));    // untouched
        }
    }

    // ---- shortcuts ----------------------------------------------------------------------------------
    void shortcutsStartTheProgramThroughItsProfile()
    {
        AppProfile p = newProfile(QStringLiteral("C:\\Apps\\Tool\\tool.exe"), QStringLiteral("My: Tool?"));
        p.mode = GpuMode::Integrated;
        QCOMPARE(shortcutName(p), QStringLiteral("My_ Tool_ (Integrated GPU)"));          // safe as a file name
        QCOMPARE(shortcutArguments(p), (QStringList{ QStringLiteral("--launch-profile"), p.id, QStringLiteral("--interactive") }));
        QCOMPARE(shortcutArguments(p, QStringLiteral("D:/Data Folder")).mid(3),
                 (QStringList{ QStringLiteral("--data-dir"), QStringLiteral("D:\\Data Folder") }));
        QCOMPARE(joinArguments({ QStringLiteral("--a"), QStringLiteral("b c"), QString() }), QStringLiteral("--a \"b c\" \"\""));

        const QString exe = m_dir.filePath(QStringLiteral("shortcut/app/My App.exe"));
        touch(exe);
        AppProfile real = newProfile(exe);
        const ShortcutSpec spec = profileShortcut(real, QStringLiteral("C:\\GPU Fusion\\GPUFusion.exe"), m_dir.filePath(QStringLiteral("shortcut")),
                                                  QStringLiteral("D:\\My Data"));
        QVERIFY(spec.path.endsWith(QStringLiteral("My App (Automatic).lnk")));
        QCOMPARE(spec.iconPath, QDir::toNativeSeparators(exe));                            // the program's own icon
        QString error;
        QVERIFY2(writeShortcut(spec, &error), qPrintable(error));
        const auto back = readShortcut(spec.path, &error);
        QVERIFY2(back.has_value(), qPrintable(error));
        QCOMPARE(back->target, spec.target);
        QCOMPARE(back->arguments, spec.arguments);                                          // round trip, incl. paths with spaces
        QCOMPARE(back->workingDirectory, QDir::toNativeSeparators(QFileInfo(exe).absolutePath()));
        QCOMPARE(back->iconPath, spec.iconPath);
        QVERIFY(back->description.contains(QStringLiteral("through GPU Fusion")));
        QVERIFY(!readShortcut(m_dir.filePath(QStringLiteral("missing.lnk")), &error).has_value());
    }

    // ---- applying ---------------------------------------------------------------------------------
    void applyWritesOnlyJustifiedValues()
    {
        const QString exe = QDir::toNativeSeparators(m_dir.filePath(QStringLiteral("apply/app.exe")));
        touch(exe);
        FakeStore store;
        store.values[exe] = QStringLiteral("SwapEffectUpgradeEnable=1;GpuPreference=1;");
        store.values[QStringLiteral("C:\\Other\\other.exe")] = QStringLiteral("GpuPreference=2;");
        ChangeJournal journal(m_dir.filePath(QStringLiteral("journal.json")));
        PreferenceManager manager(store, journal);
        const CapabilityInputs in = referenceInputs();
        AppProfile p = newProfile(exe);

        p.mode = GpuMode::Discrete;                    // verified here: stored and read back
        ApplyResult r = applyProfile(p, manager, in);
        QVERIFY2(r.ok, qPrintable(r.message));
        QCOMPARE(store.values[exe], QStringLiteral("SwapEffectUpgradeEnable=1;GpuPreference=2;"));
        QCOMPARE(p.appliedValue, store.values[exe]);

        p.mode = GpuMode::Integrated;                  // not enforceable here: no value (other keys kept)
        r = applyProfile(p, manager, in);
        QVERIFY2(r.ok, qPrintable(r.message));
        QCOMPARE(store.values[exe], QStringLiteral("SwapEffectUpgradeEnable=1;"));
        QVERIFY(r.message.contains(QStringLiteral("passed to the program")));

        p.mode = GpuMode::Both;
        QVERIFY(applyProfile(p, manager, in).ok);
        QCOMPARE(store.values[exe], QStringLiteral("SwapEffectUpgradeEnable=1;"));

        QCOMPARE(store.values[QStringLiteral("C:\\Other\\other.exe")], QStringLiteral("GpuPreference=2;")); // untouched

        AppProfile missing = newProfile(m_dir.filePath(QStringLiteral("apply/missing.exe")));
        missing.mode = GpuMode::Discrete;
        QVERIFY(!applyProfile(missing, manager, in).ok);
    }
};

QTEST_GUILESS_MAIN(TestProfiles)
#include "tst_profiles.moc"
