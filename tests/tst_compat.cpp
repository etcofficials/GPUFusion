// The compatibility registry and classifier, from constructed program facts (no files read, nothing
// started): six capabilities answered separately, registry entries with evidence, the user's entries
// first, what GPU Fusion verified outranking expectations.
#include "compat/Compatibility.h"

#include <QJsonArray>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QTest>

using namespace gf::compat;

namespace {
// The reference PC: GT 730 with Direct3D 12, HD 2000 without; no Vulkan; Windows' preference cannot
// reach the integrated GPU.
SystemFacts referencePc()
{
    SystemFacts s;
    s.physicalGpus = 2;
    s.gpus = { QStringLiteral("NVIDIA GeForce GT 730"), QStringLiteral("Intel(R) HD Graphics") };
    s.discreteGpu = s.gpus[0];
    s.integratedGpu = s.gpus[1];
    s.d3d12Gpus = { s.gpus[0] };
    s.preferenceReachesIntegrated = false;
    return s;
}
} // namespace

class TestCompat : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void factsFromFiles()
    {
        const ProgramFacts game = factsFrom(QStringLiteral("C:/Games/Game/Game-Win64-Shipping.exe"),
                                            QStringList{ QStringLiteral("D3D12.dll"), QStringLiteral("DXGI.dll"), QStringLiteral("KERNEL32.dll") }, {});
        QVERIFY(game.d3d12 && game.dxgi && !game.d3d11);
        QVERIFY(game.runtimes.contains(QStringLiteral("Unreal Engine")));
        QCOMPARE(game.apis(), QStringList{ QStringLiteral("Direct3D 12") });

        const ProgramFacts electron = factsFrom(QStringLiteral("C:/Apps/Chat/chat.exe"), QStringList{},
                                                { QStringLiteral("icudtl.dat"), QStringLiteral("v8_context_snapshot.bin"), QStringLiteral("resources.pak"),
                                                  QStringLiteral("resources") });
        QVERIFY(electron.runtimes.contains(QStringLiteral("Electron")));
        const ProgramFacts edge = factsFrom(QStringLiteral("C:/Edge/msedge.exe"), QStringList{},
                                            { QStringLiteral("icudtl.dat"), QStringLiteral("v8_context_snapshot.bin"), QStringLiteral("resources.pak") });
        QVERIFY(edge.runtimes.contains(QStringLiteral("Chromium")) && !edge.runtimes.contains(QStringLiteral("Electron")));

        const ProgramFacts unity = factsFrom(QStringLiteral("G:/XboxGames/Thing/Content/thing.exe"), QStringList{}, { QStringLiteral("UnityPlayer.dll") });
        QVERIFY(unity.runtimes.contains(QStringLiteral("Unity")));
        QVERIFY(unity.storePackage);
        QVERIFY(factsFrom(QStringLiteral("C:/jdk/bin/javaw.exe"), QStringList{}, {}).runtimes.contains(QStringLiteral("Java")));
        QVERIFY(factsFrom(QStringLiteral("C:/x/tool.exe"), QStringList{ QStringLiteral("gpufusionsdk.dll") }, {}).usesGpuFusionSdk);
        QVERIFY(!factsFrom(QStringLiteral("C:/x/tool.exe"), std::nullopt, {}).importsRead);
    }

    // The classifier alone: what the program is, against what this PC can do.
    void classifierKeepsCapabilitiesApart()
    {
        const SystemFacts pc = referencePc();
        const Assessment d3d12 = assess(factsFrom(QStringLiteral("C:/g/new.exe"), QStringList{ QStringLiteral("d3d12.dll") }, {}), pc, {});
        QCOMPARE(d3d12.answers.size(), 6);
        QCOMPARE(d3d12.answer(Capability::SplitFrames).status, Status::Unsupported);
        QVERIFY(d3d12.answer(Capability::SplitFrames).reasons.join(u' ').contains(QStringLiteral("No Direct3D 12 on: Intel(R) HD Graphics")));
        QCOMPARE(d3d12.answer(Capability::MoveOffGameGpu).status, Status::Unsupported); // the HD 2000 has no D3D12
        QCOMPARE(d3d12.answer(Capability::SelectGpu).status, Status::Partial);
        QCOMPARE(d3d12.answer(Capability::Scheduling).status, Status::Partial);

        const Assessment d3d11 = assess(factsFrom(QStringLiteral("C:/g/old.exe"), QStringList{ QStringLiteral("d3d11.dll") }, {}), pc, {});
        QCOMPARE(d3d11.answer(Capability::MoveOffGameGpu).status, Status::NotTested); // could run there, no known way
        QVERIFY(d3d11.answer(Capability::MoveOffGameGpu).reasons.join(u' ').contains(QStringLiteral("preference cannot select the integrated GPU")));

        const Assessment gl = assess(factsFrom(QStringLiteral("C:/g/gl.exe"), QStringList{ QStringLiteral("opengl32.dll") }, {}), pc, {});
        QCOMPARE(gl.answer(Capability::MoveOffGameGpu).status, Status::Unsupported);
        QVERIFY(gl.answer(Capability::MoveOffGameGpu).reasons.join(u' ').contains(QStringLiteral("OpenGL")));

        // Splitting frames: never "supported" by GPU Fusion; on a PC with D3D12 on both GPUs it depends on the program.
        SystemFacts both = pc;
        both.d3d12Gpus = both.gpus;
        QCOMPARE(assess(factsFrom(QStringLiteral("C:/g/new.exe"), QStringList{ QStringLiteral("d3d12.dll") }, {}), both, {}).answer(Capability::SplitFrames).status,
                 Status::NotTested);

        // An SDK program divides its own work; only the registry (with measurements) makes that Supported.
        const Assessment sdk = assess(factsFrom(QStringLiteral("C:/x/tool.exe"), QStringList{ QStringLiteral("gpufusionsdk.dll"), QStringLiteral("d3d11.dll") }, {}), pc, {});
        QCOMPARE(sdk.answer(Capability::SplitWorkloads).status, Status::Experimental);
        QCOMPARE(sdk.answer(Capability::SplitFrames).status, Status::Unsupported);
    }

    void registryEntriesWithEvidence()
    {
        const SystemFacts pc = referencePc();
        const QList<Entry> builtIn = builtInEntries();
        const Assessment brave = assess(factsFrom(QStringLiteral("C:/b/brave.exe"), QStringList{}, {}), pc, builtIn);
        QCOMPARE(brave.answer(Capability::SelectGpu).status, Status::Supported);
        QCOMPARE(brave.answer(Capability::MoveOffGameGpu).status, Status::Supported);
        QCOMPARE(brave.answer(Capability::SplitFrames).status, Status::Unsupported); // not set by the entry: the classifier's answer
        QVERIFY(brave.answer(Capability::SelectGpu).source.startsWith(QStringLiteral("registry: chromium-verified")));
        QVERIFY(brave.answer(Capability::SelectGpu).reasons.first().startsWith(QStringLiteral("Evidence: Verified on the reference PC")));
        QVERIFY(brave.answer(Capability::SelectGpu).reasons.join(u' ').contains(QStringLiteral("--use-adapter-luid")));

        const Assessment dwm = assess(factsFrom(QStringLiteral("C:/Windows/System32/dwm.exe"), QStringList{ QStringLiteral("d3d11.dll") }, {}), pc, builtIn);
        QCOMPARE(dwm.answer(Capability::MoveOffGameGpu).status, Status::Unsupported);
        const Assessment roblox = assess(factsFrom(QStringLiteral("G:/XboxGames/Roblox/Content/RobloxPlayerBeta.exe"), QStringList{}, {}), pc, builtIn);
        QCOMPARE(roblox.answer(Capability::SplitWorkloads).status, Status::Unsupported);
        QCOMPARE(roblox.answer(Capability::MoveOffGameGpu).status, Status::Unsupported);
        const Assessment discord = assess(factsFrom(QStringLiteral("C:/d/Discord.exe"), QStringList{},
                                                    { QStringLiteral("icudtl.dat"), QStringLiteral("snapshot_blob.bin"), QStringLiteral("resources.pak"),
                                                      QStringLiteral("resources") }), pc, builtIn);
        QCOMPARE(discord.answer(Capability::MoveOffGameGpu).status, Status::Experimental);
        QVERIFY(discord.matchedEntries.contains(QStringLiteral("electron")));
    }

    // The user's entries come first and override built-in ones; bad entries are reported, not fatal.
    void userEntries()
    {
        QTemporaryDir dir;
        const QString file = dir.filePath(QStringLiteral("compatibility.json"));
        QJsonArray a;
        a << QJsonObject{ { QStringLiteral("id"), QStringLiteral("my-brave") }, { QStringLiteral("executables"), QJsonArray{ QStringLiteral("Brave.exe") } },
                          { QStringLiteral("status"), QJsonObject{ { QStringLiteral("select-gpu"), QStringLiteral("unsupported") } } },
                          { QStringLiteral("reasons"), QJsonObject{ { QStringLiteral("select-gpu"), QStringLiteral("My company build ignores switches.") } } } };
        a << QJsonObject{ { QStringLiteral("id"), QStringLiteral("broken") }, { QStringLiteral("executables"), QJsonArray{ QStringLiteral("x.exe") } },
                          { QStringLiteral("status"), QJsonObject{ { QStringLiteral("select-gpu"), QStringLiteral("works-great") } } } };
        QFile f(file);
        QVERIFY(f.open(QIODevice::WriteOnly));
        f.write(QJsonDocument(a).toJson());
        f.close();
        QStringList problems;
        const QList<Entry> entries = loadEntries(file, &problems);
        QCOMPARE(problems.size(), 1);
        QVERIFY(problems[0].contains(QStringLiteral("works-great")));
        QVERIFY(entries.first().userDefined);
        const Assessment brave = assess(factsFrom(QStringLiteral("C:/b/brave.exe"), QStringList{}, {}), referencePc(), entries);
        QCOMPARE(brave.answer(Capability::SelectGpu).status, Status::Unsupported);
        QVERIFY(brave.answer(Capability::SelectGpu).source.endsWith(QStringLiteral("(yours)")));
        QCOMPARE(brave.answer(Capability::MoveOffGameGpu).status, Status::Supported); // still the built-in entry's

        // Round trip of an entry.
        const Entry e = builtInEntries().first();
        const auto back = entryFromJson(entryToJson(e));
        QVERIFY(back);
        QCOMPARE(back->status, e.status);
        QCOMPARE(back->executables, e.executables);
    }

    void verifiedEvidenceOutranksExpectations()
    {
        ProgramEvidence ev;
        ev.ranOnIntegrated = true;
        ev.note = QStringLiteral("GPU Fusion's verification of profile “tool” (2026-10-28): Confirmed.");
        const Assessment a = assess(factsFrom(QStringLiteral("C:/x/tool.exe"), QStringList{ QStringLiteral("d3d11.dll") }, {}), referencePc(), builtInEntries(), ev);
        QCOMPARE(a.answer(Capability::SelectGpu).status, Status::Supported);
        QCOMPARE(a.answer(Capability::MoveOffGameGpu).source, QStringLiteral("verified on this PC"));
        const QString html = assessmentsHtml({ a }, referencePc());
        QVERIFY(html.contains(QStringLiteral("class=\"supported\"")));
        QVERIFY(html.contains(QStringLiteral("Nothing was started to produce this page")));
        QVERIFY(assessmentLines(a).size() == 7);
    }
};

QTEST_GUILESS_MAIN(TestCompat)
#include "tst_compat.moc"
