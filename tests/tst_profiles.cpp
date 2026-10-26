// Pure tests for generic per-application profiles: JSON, ids, the per-file store, version-folder
// resolution, the per-mode capability assessment, the SDK launch environment, the per-mode verdict and
// applying a profile (against an in-memory registry).
#include "preferences/PreferenceModel.h"
#include "profiles/AppDiscovery.h"
#include "profiles/AppProfile.h"
#include "profiles/KnownOptions.h"
#include "profiles/ProfileActions.h"
#include "profiles/ProfileStore.h"

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
        p.shareGpuChoiceWithApp = false;
        QVERIFY(launchEnvironment(p, kGpus).isEmpty());

        p.arguments = QStringLiteral("--render 7 \"C:\\My Files\\in.mp4\"");
        QCOMPARE(profileArguments(p, kGpus), (QStringList{ QStringLiteral("--render"), QStringLiteral("7"), QStringLiteral("C:\\My Files\\in.mp4") }));
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
        evaluateProfileEvidence(nvOnly, GpuMode::Both, kGpus, &r);
        QVERIFY(!r.confirmed);                                      // one GPU is not both

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
        evaluateProfileEvidence(both, GpuMode::Discrete, kGpus, &r);
        QVERIFY(!r.confirmed);                                      // Discrete asked for the GT 730 only
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
