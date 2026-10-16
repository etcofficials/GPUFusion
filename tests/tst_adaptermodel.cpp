// Pure-logic tests for adapter classification, merging and findings, using synthetic data that
// mirrors the reference machine (GT 730 + HD Graphics 2000 + WARP) and harder edge cases.
#include "hardware/AdapterModel.h"

#include <QTest>

#include <windows.h>

using namespace gf::hw;

namespace {

DxgiAdapter nvidia()
{
    DxgiAdapter a;
    a.index = 0;
    a.description = QStringLiteral("NVIDIA GeForce GT 730");
    a.vendorId = 0x10DE; a.deviceId = 0x0F02; a.subSysId = 0x098F10DE; a.revision = 0xA1;
    a.luid = 0x7EDA;
    a.dedicatedVideoMemory = 4031ull << 20;
    a.sharedSystemMemory = 6086ull << 20;
    a.wddmVersion = 2300;
    a.location = PciLocation{ 1, 0, 0 };
    a.d3d11.device = { ApiProbe::State::Succeeded, QStringLiteral("11_0"), 0, {} };
    a.d3d11.computeShaders = true;
    a.d3d12.device = { ApiProbe::State::Succeeded, QStringLiteral("11_0"), 0, {} };
    a.d3d12.maxFeatureLevel = QStringLiteral("11_0");
    a.d3d12.uma = false;
    a.outputs = { DisplayOutput{ QStringLiteral("\\\\.\\DISPLAY1"), {}, 1920, 1080, true } };
    return a;
}

DxgiAdapter intel()
{
    DxgiAdapter a;
    a.index = 1;
    a.description = QStringLiteral("Intel(R) HD Graphics");
    a.vendorId = 0x8086; a.deviceId = 0x0102; a.subSysId = 0x21118086; a.revision = 0x09;
    a.luid = 0x896D;
    a.dedicatedVideoMemory = 32ull << 20;
    a.sharedSystemMemory = 1632ull << 20;
    a.wddmVersion = 1200;
    a.location = PciLocation{ 0, 2, 0 };
    a.d3d11.device = { ApiProbe::State::Succeeded, QStringLiteral("10_1"), 0, {} };
    a.d3d12.device = { ApiProbe::State::Failed, {}, qint32(DXGI_ERROR_UNSUPPORTED), describeHresult(DXGI_ERROR_UNSUPPORTED) };
    return a;
}

DxgiAdapter warp()
{
    DxgiAdapter a;
    a.index = 2;
    a.description = QStringLiteral("Microsoft Basic Render Driver");
    a.vendorId = 0x1414; a.deviceId = 0x008C;
    a.luid = 0x8946;
    a.softwareFlag = true;
    return a;
}

PnpDevice pnp(const QString& id, std::optional<PciLocation> loc, bool present = true)
{
    PnpDevice d;
    d.instanceId = id;
    d.description = QStringLiteral("device ") + id.section(u'\\', 1, 1);
    d.present = present;
    d.location = loc;
    d.driverProvider = QStringLiteral("Vendor");
    d.driverDate = QDate(2018, 3, 23);
    return d;
}

const QString kNvId = QStringLiteral("PCI\\VEN_10DE&DEV_0F02&SUBSYS_098F10DE&REV_A1\\4&465B5C5&0&0008");
const QString kIntelId = QStringLiteral("PCI\\VEN_8086&DEV_0102&SUBSYS_21118086&REV_09\\3&11583659&0&10");

bool hasFinding(const QList<Finding>& findings, const QString& titleStart, Finding::Severity s)
{
    return std::any_of(findings.begin(), findings.end(), [&](const Finding& f) { return f.severity == s && f.title.startsWith(titleStart); });
}

} // namespace

class TestAdapterModel : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void parsesPciInstanceIds()
    {
        const PciIds ids = parsePciInstanceId(kNvId);
        QVERIFY(ids.valid);
        QCOMPARE(ids.vendor, 0x10DEu);
        QCOMPARE(ids.device, 0x0F02u);
        QCOMPARE(ids.subsystem, 0x098F10DEu);
        QCOMPARE(ids.revision, 0xA1u);
        QVERIFY(parsePciInstanceId(QStringLiteral("pci\\ven_8086&dev_0102\\x")).valid); // case-insensitive, optional parts
        QVERIFY(!parsePciInstanceId(QStringLiteral("ROOT\\BasicRender\\0000")).valid);
        QVERIFY(!parsePciInstanceId(QString()).valid);
    }

    void warpIsSoftwareEvenWithoutFlag()
    {
        QVERIFY(isSoftwareAdapter(warp()));
        DxgiAdapter noFlag = warp();
        noFlag.softwareFlag = false;
        QVERIFY(isSoftwareAdapter(noFlag));
        QCOMPARE(classify(noFlag).value, AdapterClass::Software);
        QVERIFY(!isSoftwareAdapter(nvidia()));
    }

    void classifiesReferenceAdapters()
    {
        QCOMPARE(classify(nvidia()).value, AdapterClass::Discrete);
        QCOMPARE(classify(intel()).value, AdapterClass::Integrated);
    }

    void classificationEdgeCases()
    {
        DxgiAdapter apu = nvidia();
        apu.vendorId = 0x1002;
        apu.d3d12.uma = true; // UMA wins over a large carve-out
        QCOMPARE(classify(apu).value, AdapterClass::Integrated);

        DxgiAdapter arc = intel();
        arc.dedicatedVideoMemory = 8ull << 30;
        arc.d3d12.uma = false;
        QCOMPARE(classify(arc).value, AdapterClass::Discrete);

        DxgiAdapter tiny = nvidia();
        tiny.dedicatedVideoMemory = 64ull << 20;
        tiny.d3d12.uma.reset();
        QCOMPARE(classify(tiny).value, AdapterClass::Unknown);
    }

    void mergesReferenceMachine()
    {
        const DetectionResult r = mergeAdapters({ nvidia(), intel(), warp() },
                                                { pnp(kNvId, PciLocation{ 1, 0, 0 }), pnp(kIntelId, PciLocation{ 0, 2, 0 }) });
        QCOMPARE(r.adapters.size(), 3);
        QCOMPARE(r.hardwareCount(), 2);
        QCOMPARE(r.softwareCount(), 1);
        QCOMPARE(r.adapters[0].name(), QStringLiteral("NVIDIA GeForce GT 730"));
        QCOMPARE(r.adapters[0].pnp->instanceId, kNvId);
        QCOMPARE(r.adapters[0].matchMethod, QStringLiteral("PCI location"));
        QCOMPARE(r.adapters[1].pnp->instanceId, kIntelId);
        QVERIFY(r.adapters[2].isSoftware());
        QVERIFY(!r.adapters[2].pnp.has_value());
    }

    void identicalCardsAreMatchedByLocation()
    {
        DxgiAdapter a = nvidia(), b = nvidia();
        b.index = 1; b.luid = 0x9000; b.location = PciLocation{ 2, 0, 0 };
        const QString idA = kNvId, idB = QString(kNvId).replace(QStringLiteral("0008"), QStringLiteral("0010"));
        // Device Manager order deliberately reversed.
        const DetectionResult r = mergeAdapters({ a, b }, { pnp(idB, PciLocation{ 2, 0, 0 }), pnp(idA, PciLocation{ 1, 0, 0 }) });
        QCOMPARE(r.hardwareCount(), 2);
        QCOMPARE(r.adapters[0].pnp->instanceId, idA);
        QCOMPARE(r.adapters[1].pnp->instanceId, idB);
    }

    void identicalCardsWithoutLocationAreNotGuessed()
    {
        DxgiAdapter a = nvidia(), b = nvidia();
        a.location.reset(); b.location.reset(); b.luid = 0x9000;
        const QString idB = QString(kNvId).replace(QStringLiteral("0008"), QStringLiteral("0010"));
        const DetectionResult r = mergeAdapters({ a, b }, { pnp(kNvId, std::nullopt), pnp(idB, std::nullopt) });
        // Ambiguous: neither DXGI adapter may be paired with an arbitrary entry.
        QCOMPARE(r.adapters[0].matchMethod, QStringLiteral("no matching Device Manager entry"));
        QCOMPARE(r.adapters[1].matchMethod, QStringLiteral("no matching Device Manager entry"));
        QCOMPARE(r.hardwareCount(), 2);
    }

    void fallsBackToPciIdsWhenLocationUnknown()
    {
        DxgiAdapter a = intel();
        a.location.reset();
        const DetectionResult r = mergeAdapters({ a }, { pnp(kIntelId, std::nullopt) });
        QCOMPARE(r.adapters[0].matchMethod, QStringLiteral("PCI vendor, device, subsystem and revision IDs"));
    }

    void duplicateLuidsAreReportedOnce()
    {
        const DetectionResult r = mergeAdapters({ nvidia(), nvidia(), warp() }, { pnp(kNvId, PciLocation{ 1, 0, 0 }) });
        QCOMPARE(r.hardwareCount(), 1);
        QCOMPARE(r.adapters.size(), 2);
    }

    void deviceManagerOnlyEntriesAreKept()
    {
        PnpDevice disabled = pnp(kIntelId, PciLocation{ 0, 2, 0 });
        disabled.hasProblem = true;
        disabled.problemCode = 22;
        const DetectionResult r = mergeAdapters({ nvidia(), warp() }, { pnp(kNvId, PciLocation{ 1, 0, 0 }), disabled });
        QCOMPARE(r.adapters.size(), 3);
        QCOMPARE(r.hardwareCount(), 1); // the disabled Intel is not counted as usable
        const Adapter& intelEntry = r.adapters[1];
        QVERIFY(!intelEntry.dxgi.has_value());
        QCOMPARE(intelEntry.matchMethod, QStringLiteral("Device Manager only"));
        const auto findings = findingsFor(intelEntry, QDate(2026, 10, 16));
        QVERIFY(hasFinding(findings, QStringLiteral("Device Manager reports a problem (code 22)"), Finding::Severity::Error));
        QVERIFY(findings[0].detail.contains(QStringLiteral("disabled in Device Manager")));
    }

    void staleNonPresentDuplicatesAreHidden()
    {
        const DetectionResult r = mergeAdapters({ nvidia() }, {
            pnp(kNvId, PciLocation{ 1, 0, 0 }),
            pnp(QString(kNvId).replace(QStringLiteral("0008"), QStringLiteral("00E0")), std::nullopt, /*present*/ false),
        });
        QCOMPARE(r.adapters.size(), 1);
        QCOMPARE(r.staleDeviceEntries, 1);
    }

    void absentIntelIsReportedWithFirmwareHint()
    {
        const DetectionResult r = mergeAdapters({ nvidia() }, { pnp(kNvId, PciLocation{ 1, 0, 0 }), pnp(kIntelId, std::nullopt, false) });
        QCOMPARE(r.adapters.size(), 2);
        QCOMPARE(r.staleDeviceEntries, 0);
        const auto findings = findingsFor(r.adapters[1], QDate(2026, 10, 16));
        QVERIFY(hasFinding(findings, QStringLiteral("Not currently detected"), Finding::Severity::Warning));
        QVERIFY(findings.last().detail.contains(QStringLiteral("iGPU")));
        QVERIFY(findings.last().detail.contains(QStringLiteral("does not change firmware settings")));
    }

    void intelFindingsMatchItsLimits()
    {
        Adapter a;
        a.dxgi = intel();
        a.pnp = pnp(kIntelId, PciLocation{ 0, 2, 0 });
        a.pnp->driverDate = QDate(2016, 5, 19);
        a.classification = classify(*a.dxgi);
        const auto f = findingsFor(a, QDate(2026, 10, 16));
        QVERIFY(hasFinding(f, QStringLiteral("Legacy WDDM 1.2 driver"), Finding::Severity::Warning));
        QVERIFY(hasFinding(f, QStringLiteral("Direct3D feature level 10_1"), Finding::Severity::Info));
        QVERIFY(hasFinding(f, QStringLiteral("No Direct3D 12 support"), Finding::Severity::Info));
        QVERIFY(hasFinding(f, QStringLiteral("Driver from 2016"), Finding::Severity::Warning));
        QVERIFY(hasFinding(f, QStringLiteral("No display connected"), Finding::Severity::Info));
        QVERIFY(!hasFinding(f, QString(), Finding::Severity::Error));
    }

    void healthyModernAdapterHasNoWarnings()
    {
        Adapter a;
        a.dxgi = nvidia();
        a.dxgi->wddmVersion = 3100;
        a.pnp = pnp(kNvId, PciLocation{ 1, 0, 0 });
        a.pnp->driverDate = QDate(2025, 1, 1);
        a.classification = classify(*a.dxgi);
        const auto f = findingsFor(a, QDate(2026, 10, 16));
        QVERIFY2(f.isEmpty(), qPrintable(f.isEmpty() ? QString() : f.first().title));
    }

    void failedD3D11AndBasicDisplayAreFlagged()
    {
        Adapter a;
        a.dxgi = nvidia();
        a.dxgi->d3d11.device = { ApiProbe::State::Failed, {}, qint32(E_FAIL), describeHresult(E_FAIL) };
        a.pnp = pnp(kNvId, PciLocation{ 1, 0, 0 });
        a.pnp->driverProvider = QStringLiteral("Microsoft");
        a.pnp->description = QStringLiteral("Microsoft Basic Display Adapter");
        const auto f = findingsFor(a, QDate(2026, 10, 16));
        QVERIFY(hasFinding(f, QStringLiteral("Direct3D 11 device creation failed"), Finding::Severity::Error));
        QVERIFY(hasFinding(f, QStringLiteral("No vendor driver installed"), Finding::Severity::Warning));
    }

    void softwareAdapterExplainsItself()
    {
        Adapter a;
        a.dxgi = warp();
        a.classification = classify(*a.dxgi);
        const auto f = findingsFor(a, QDate(2026, 10, 16));
        QCOMPARE(f.size(), 1);
        QVERIFY(f[0].detail.contains(QStringLiteral("not a physical GPU")));
    }

    void explainsHresultsInPlainLanguage()
    {
        const QString unsupported = describeHresult(DXGI_ERROR_UNSUPPORTED);
        QVERIFY(unsupported.contains(QStringLiteral("does not support")));
        QVERIFY(unsupported.contains(QStringLiteral("0x887A0004")));
        QVERIFY(describeHresult(DXGI_ERROR_DEVICE_REMOVED).contains(QStringLiteral("removed")));
        const QString unknown = describeHresult(qint32(0x80070005)); // E_ACCESSDENIED via system text
        QVERIFY(unknown.contains(QStringLiteral("0x80070005")));
        QVERIFY(unknown.size() > QStringLiteral(" (HRESULT 0x80070005)").size());
    }

    void explainsProblemCodes()
    {
        QVERIFY(describeProblemCode(43).contains(QStringLiteral("code 43")));
        QVERIFY(describeProblemCode(28).contains(QStringLiteral("No driver")));
        QVERIFY(describeProblemCode(999).contains(QStringLiteral("999")));
    }

    void namesDecoderProfilesAndCodecs()
    {
        QCOMPARE(decoderProfileName(QStringLiteral("{1b81be68-a0c7-11d3-b984-00c04f2e73c5}")), QStringLiteral("H.264 VLD"));
        QVERIFY(decoderProfileName(QStringLiteral("{32FCFE3F-DE46-4A49-861B-AC71110649D5}")).isEmpty()); // vendor-specific
        const QStringList codecs = decoderCodecs({ QStringLiteral("{1B81BE68-A0C7-11D3-B984-00C04F2E73C5}"),
                                                   QStringLiteral("{EE27417F-5E28-4E65-BEEA-1D26B508ADC9}"),
                                                   QStringLiteral("{86695F12-340E-4F04-9FD3-9253DD327460}"),
                                                   QStringLiteral("{32FCFE3F-DE46-4A49-861B-AC71110649D5}") });
        QCOMPARE(codecs, (QStringList{ QStringLiteral("H.264"), QStringLiteral("MPEG-2") }));
    }

    void formatsValues()
    {
        QCOMPARE(formatWddm(1200), QStringLiteral("1.2"));
        QCOMPARE(formatWddm(2300), QStringLiteral("2.3"));
        QCOMPARE(formatWddm(1105), QStringLiteral("1.1"));
        QCOMPARE(formatWddm(0), QStringLiteral("unknown"));
        QCOMPARE(formatLuid(0x7EDA), QStringLiteral("00000000_00007EDA"));
        QCOMPARE(formatBytes(4031ull << 20), QStringLiteral("3.94 GB"));
        QCOMPARE(formatBytes(32ull << 20), QStringLiteral("32 MB"));
        QCOMPARE(PciLocation({ 1, 0, 0 }).toString(), QStringLiteral("bus 1, device 0, function 0"));
    }

    void rejectsNonPciLocationSentinels()
    {
        QVERIFY(PciLocation::fromRaw(1, 0, 0).has_value());
        QVERIFY(PciLocation::fromRaw(255, 31, 7).has_value());
        QVERIFY(!PciLocation::fromRaw(0xFFFFFFFF, 0xFFFF, 0xFFFF).has_value()); // WARP via D3DKMT
        QVERIFY(!PciLocation::fromRaw(0, 32, 0).has_value());
        QVERIFY(!PciLocation::fromRaw(0, 0, 8).has_value());
    }
};

QTEST_GUILESS_MAIN(TestAdapterModel)
#include "tst_adaptermodel.moc"
