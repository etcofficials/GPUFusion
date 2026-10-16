// Hardware-dependent tests: run detection on the machine executing the tests. Assertions are
// limited to invariants that must hold on any Windows system; machine-specific expectations are
// checked only when GPUFUSION_EXPECT_REFERENCE_MACHINE=1 (the GT 730 + HD 2000 development PC).
#include "hardware/AdapterDetector.h"

#include <QElapsedTimer>
#include <QSet>
#include <QTest>

using namespace gf::hw;

class TestHardwareLive : public QObject {
    Q_OBJECT

    DetectionResult m_first;

private Q_SLOTS:
    void initTestCase()
    {
        m_first = detectAdapters(nullptr);
        for (const Adapter& a : m_first.adapters)
            qInfo().noquote() << summarize(a);
    }

    void detectionReportsNoGlobalErrors()
    {
        QVERIFY2(m_first.errors.isEmpty(), qPrintable(m_first.errors.join(QStringLiteral("; "))));
    }

    void softwareAdaptersAreNeverCountedAsGpus()
    {
        int hardware = 0;
        for (const Adapter& a : m_first.adapters) {
            if (a.dxgi && isSoftwareAdapter(*a.dxgi))
                QCOMPARE(a.classification.value, AdapterClass::Software);
            if (a.dxgi && !a.isSoftware())
                ++hardware;
        }
        QCOMPARE(m_first.hardwareCount(), hardware);
    }

    void adaptersAreNotDuplicated()
    {
        QSet<quint64> luids;
        QSet<QString> instances;
        for (const Adapter& a : m_first.adapters) {
            if (a.dxgi) {
                QVERIFY2(!luids.contains(a.dxgi->luid), qPrintable(formatLuid(a.dxgi->luid)));
                luids.insert(a.dxgi->luid);
            }
            if (a.pnp) {
                QVERIFY2(!instances.contains(a.pnp->instanceId), qPrintable(a.pnp->instanceId));
                instances.insert(a.pnp->instanceId);
            }
        }
    }

    void everyHardwareAdapterWasProbed()
    {
        if (m_first.hardwareCount() == 0)
            QSKIP("No physical GPU is available to DirectX on this machine");
        for (const Adapter& a : m_first.adapters) {
            if (!a.dxgi || a.isSoftware())
                continue;
            QVERIFY(a.dxgi->d3d11.device.state != ApiProbe::State::NotAttempted);
            QVERIFY(a.dxgi->d3d12.device.state != ApiProbe::State::NotAttempted);
            QVERIFY(!a.dxgi->capsReused);
            if (a.dxgi->d3d11.device.state == ApiProbe::State::Failed)
                QVERIFY(!a.dxgi->d3d11.device.error.isEmpty());
        }
    }

    void secondDetectionReusesProbes()
    {
        if (m_first.hardwareCount() == 0)
            QSKIP("No physical GPU is available to DirectX on this machine");
        QElapsedTimer t;
        t.start();
        const DetectionResult second = detectAdapters(&m_first);
        qInfo() << "full detection" << m_first.durationMs << "ms; with reused probes" << t.elapsed() << "ms";
        QCOMPARE(second.hardwareCount(), m_first.hardwareCount());
        for (const Adapter& a : second.adapters) {
            if (a.dxgi && !a.isSoftware())
                QVERIFY2(a.dxgi->capsReused, qPrintable(a.name()));
        }
    }

    void referenceMachineExpectations()
    {
        if (qEnvironmentVariableIntValue("GPUFUSION_EXPECT_REFERENCE_MACHINE") != 1)
            QSKIP("Set GPUFUSION_EXPECT_REFERENCE_MACHINE=1 to check the development PC's known adapters");
        const Adapter* nv = nullptr;
        const Adapter* igpu = nullptr;
        for (const Adapter& a : m_first.adapters) {
            if (!a.dxgi)
                continue;
            if (a.dxgi->vendorId == 0x10DE && a.dxgi->deviceId == 0x0F02) nv = &a;
            if (a.dxgi->vendorId == 0x8086 && a.dxgi->deviceId == 0x0102) igpu = &a;
        }
        QVERIFY2(nv, "GT 730 (10DE:0F02) not found");
        QVERIFY2(igpu, "HD Graphics 2000 (8086:0102) not found");
        QCOMPARE(nv->classification.value, AdapterClass::Discrete);
        QCOMPARE(nv->dxgi->d3d11.device.featureLevel, QStringLiteral("11_0"));
        QVERIFY(nv->dxgi->d3d12.device.ok());
        QVERIFY(nv->pnp.has_value());
        QCOMPARE(igpu->classification.value, AdapterClass::Integrated);
        QCOMPARE(igpu->dxgi->d3d11.device.featureLevel, QStringLiteral("10_1"));
        QVERIFY(!igpu->dxgi->d3d11.computeShaders);
        QVERIFY(igpu->dxgi->d3d12.device.state == ApiProbe::State::Failed);
        QCOMPARE(igpu->dxgi->wddmVersion, 1200);
        QVERIFY(igpu->pnp.has_value());
        QCOMPARE(m_first.hardwareCount(), 2);
    }
};

QTEST_GUILESS_MAIN(TestHardwareLive)
#include "tst_hardware_live.moc"
