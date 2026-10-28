// Tests for the header-only GPU Fusion SDK: mode and adapter requests from the environment GPU Fusion
// sets (and its format version), adapter resolution and per-adapter devices against the real DXGI
// factory, the weighted split, and the scheduler + Balancer with simulated devices (no GPU needed):
// every unit exactly once, helper failures re-run on the primary, the "would delay the end" rule,
// and how the Balancer raises, halves and switches off a helper's share.
#include <gpufusion/d3d11.h>
#include <gpufusion/gpufusion.h>
#include <gpufusion/scheduler.h>

#include <QTest>

#include <atomic>
#include <chrono>
#include <cmath>

namespace {

// Busy-waits (the system sleep granularity is too coarse for sub-millisecond work).
void spinMs(double ms)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::duration<double, std::milli>(ms);
    while (std::chrono::steady_clock::now() < end) {
    }
}

gpufusion::RunReport fakeReport(std::uint32_t total, double wallMs, std::vector<std::uint32_t> units, gpufusion::Stop helperStop)
{
    gpufusion::RunReport r;
    r.ok = r.exactCoverage = true;
    r.total = r.completed = total;
    r.wallMs = wallMs;
    for (std::uint32_t u : units) {
        gpufusion::DeviceReport d;
        d.units = u;
        d.workMs = u ? wallMs * 0.9 : 0;
        d.stop = helperStop;
        r.devices.push_back(d);
    }
    r.devices[0].stop = gpufusion::Stop::NoUnitsLeft;
    return r;
}

} // namespace

class TestSdk : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void cleanup()
    {
        SetEnvironmentVariableW(L"GPUFUSION_MODE", nullptr);
        SetEnvironmentVariableW(L"GPUFUSION_ADAPTERS", nullptr);
        SetEnvironmentVariableW(L"GPUFUSION_ENV_VERSION", nullptr);
    }

    // GPUFUSION_ENV_VERSION: missing -> 0 (not launched) or 1 (older GPU Fusion); a newer format is
    // ignored, so such a program falls back to the default adapter instead of misreading it.
    void environmentVersion()
    {
        QCOMPARE(gpufusion::environmentVersion(), 0);
        SetEnvironmentVariableW(L"GPUFUSION_MODE", L"both");
        SetEnvironmentVariableW(L"GPUFUSION_ADAPTERS", L"00000000_00007DEC,00000000_0000870A");
        QCOMPARE(gpufusion::environmentVersion(), 1);
        SetEnvironmentVariableW(L"GPUFUSION_ENV_VERSION", L"1");
        QVERIFY(gpufusion::environmentSupported());
        QCOMPARE(gpufusion::mode(), gpufusion::Mode::Both);
        SetEnvironmentVariableW(L"GPUFUSION_ENV_VERSION", L"2");
        QVERIFY(!gpufusion::environmentSupported());
        QCOMPARE(gpufusion::mode(), gpufusion::Mode::NotLaunchedByGpuFusion);
        QVERIFY(gpufusion::requestedLuids().empty());
        // A value that is not a version number is an unknown format too, not "version 0".
        for (const wchar_t* bad : { L"abc", L"1x", L"-1", L"0" }) {
            SetEnvironmentVariableW(L"GPUFUSION_ENV_VERSION", bad);
            QCOMPARE(gpufusion::environmentVersion(), -1);
            QVERIFY(!gpufusion::environmentSupported());
            QCOMPARE(gpufusion::mode(), gpufusion::Mode::NotLaunchedByGpuFusion);
        }
        QCOMPARE(gpufusion::kSdkVersion, 2);
    }

    // One Direct3D 11 device per adapter with its capabilities (CI machines have the software adapter).
    void devicesPerAdapter()
    {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        QVERIFY(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
        std::vector<Microsoft::WRL::ComPtr<IDXGIAdapter1>> all;
        Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
        for (UINT i = 0; factory->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset())
            all.push_back(a);
        QVERIFY(!all.empty());
        std::wstring errors;
        const std::vector<gpufusion::Device> devices = gpufusion::createDevices(all, &errors);
        QVERIFY2(!devices.empty(), qPrintable(QString::fromStdWString(errors)));
        for (const gpufusion::Device& d : devices) {
            QVERIFY(d.device && d.context);
            QVERIFY(d.caps.usable());
            QVERIFY(d.caps.pixelShaders);
            QCOMPARE(d.caps.luid.size(), size_t(17));
            QVERIFY(gpufusion::featureLevelText(d.caps.featureLevel) != L"none");
            if (d.caps.featureLevel >= D3D_FEATURE_LEVEL_11_0)
                QVERIFY(d.caps.computeShaders);
        }
        QCOMPARE(gpufusion::describe(all).size(), all.size());
    }

    void policies()
    {
        gpufusion::Policy p = gpufusion::Policy::Performance;
        QVERIFY(gpufusion::parsePolicy("conservative", &p));
        QCOMPARE(p, gpufusion::Policy::Conservative);
        QVERIFY(!gpufusion::parsePolicy("turbo", &p));
        // Every policy keeps the primary the main device: helpers start small and are capped.
        for (gpufusion::Policy q : { gpufusion::Policy::Conservative, gpufusion::Policy::Balanced, gpufusion::Policy::Performance }) {
            const gpufusion::PolicyLimits l = gpufusion::limits(q);
            QVERIFY(l.initialShare >= 0.10 && l.initialShare <= 0.20);
            QVERIFY(l.maxShare <= 0.5 && l.maxShare >= l.initialShare);
            QVERIFY(l.minGain >= 0);
        }
        QVERIFY(gpufusion::limits(gpufusion::Policy::Conservative).minGain > gpufusion::limits(gpufusion::Policy::Performance).minGain);
    }

    // Every unit runs exactly once, whatever the batch sizes, with a primary and two helpers.
    void everyUnitExactlyOnce()
    {
        for (std::uint32_t total : { 1u, 7u, 1000u, 4099u }) {
            std::vector<std::atomic<int>> hits(total);
            gpufusion::Plan plan;
            plan.enabled = { 1, 1, 1 };
            plan.share = { 0.6, 0.25, 0.15 };
            plan.batch = { 37, 5, 3 };
            const gpufusion::RunReport r = gpufusion::run(total, plan, [&](gpufusion::Batch& b) {
                for (std::uint32_t u = b.first; u < b.first + b.count; ++u)
                    hits[u]++;
                spinMs(0.002 * b.count * double(b.device + 1));
                return true;
            });
            QVERIFY2(r.ok, r.error.c_str());
            QVERIFY(r.exactCoverage);
            QCOMPARE(r.completed, total);
            for (std::uint32_t u = 0; u < total; ++u)
                QCOMPARE(hits[u].load(), 1);
            std::uint32_t sum = 0;
            for (const gpufusion::DeviceReport& d : r.devices)
                sum += d.units;
            QCOMPARE(sum, total);
            // Helpers never take more than their planned share.
            QVERIFY(r.devices[1].units <= std::uint32_t(std::ceil(0.25 * total)));
            QVERIFY(r.devices[2].units <= std::uint32_t(std::ceil(0.15 * total)));
        }
        gpufusion::Plan bad;
        bad.enabled = { 0, 1 };
        bad.share = { 0, 1 };
        bad.batch = { 1, 1 };
        QVERIFY(!gpufusion::run(10, bad, [](gpufusion::Batch&) { return true; }).ok); // the primary must take part
    }

    // A helper's failed batch is re-run on the primary; the output is complete and the Balancer
    // switches that helper off. A failing primary fails the run (nothing is silently missing).
    void helperFailureFallsBackToThePrimary()
    {
        const std::uint32_t total = 600;
        std::vector<std::atomic<int>> hits(total);
        gpufusion::Plan plan;
        plan.enabled = { 1, 1 };
        plan.share = { 0.8, 0.2 };
        plan.batch = { 50, 20 };
        const gpufusion::RunReport r = gpufusion::run(total, plan, [&](gpufusion::Batch& b) {
            if (b.device == 1)
                return false; // e.g. the device was lost
            for (std::uint32_t u = b.first; u < b.first + b.count; ++u)
                hits[u]++;
            spinMs(0.02 * b.count); // the primary takes time, so the helper starts while units are left
            return true;
        });
        QVERIFY2(r.ok, r.error.c_str());
        QCOMPARE(r.devices[1].stop, gpufusion::Stop::Failed);
        QCOMPARE(r.devices[1].units, 0u);
        QVERIFY(r.devices[1].failedUnits > 0);
        QCOMPARE(r.devices[0].rerunUnits, r.devices[1].failedUnits);
        for (std::uint32_t u = 0; u < total; ++u)
            QCOMPARE(hits[u].load(), 1);

        gpufusion::Balancer b(2, gpufusion::Policy::Performance);
        b.record(plan, r);
        QVERIFY(!b.helper(1).enabled);
        QVERIFY(b.helper(1).note.find("failed") != std::string::npos);

        const gpufusion::RunReport broken = gpufusion::run(total, plan, [&](gpufusion::Batch& batch) { return batch.device != 0; });
        QVERIFY(!broken.ok);
        QVERIFY(!broken.error.empty());
        QVERIFY(broken.completed < total);
    }

    // A helper known to be far too slow takes nothing (it would finish after the primary), and a
    // moderately slow one only takes batches that fit, so the end of the job is not delayed by it.
    void slowHelperDoesNotDelayTheEnd()
    {
        gpufusion::Plan plan;
        plan.enabled = { 1, 1 };
        plan.share = { 0.5, 0.5 }; // a deliberately bad plan
        plan.batch = { 100, 100 };
        const gpufusion::RunReport none = gpufusion::run(1000, plan, [&](gpufusion::Batch& b) {
            spinMs(0.02 * b.count);
            return true;
        }, { 50.0, 0.0001 });
        QVERIFY(none.ok);
        QCOMPARE(none.devices[1].units, 0u);
        QCOMPARE(none.devices[1].stop, gpufusion::Stop::WouldDelay);

        // Primary: 0.02 ms per unit; helper: 0.2 ms per unit (10x slower). The primary alone needs ~20 ms.
        const gpufusion::RunReport r = gpufusion::run(1000, plan, [&](gpufusion::Batch& b) {
            spinMs((b.device == 0 ? 0.02 : 0.2) * b.count);
            return true;
        }, { 50.0, 5.0 });
        QVERIFY(r.ok);
        QVERIFY(r.devices[1].units > 0);
        QVERIFY2(r.devices[1].units < 200, qPrintable(QString::number(r.devices[1].units))); // far below its 50 % "share"
        QVERIFY2(r.devices[1].busyMs <= r.devices[0].busyMs * 1.5 + 5,
                 qPrintable(QStringLiteral("%1 vs %2").arg(r.devices[1].busyMs).arg(r.devices[0].busyMs)));
        QVERIFY(r.overheadMs() >= 0);
    }

    // The Balancer: baseline first, then a small helper share; grows after gains (capped by the
    // policy), halves after a split that was not faster, switches the helper off after repeated
    // misses, and re-measures the primary alone now and then.
    void balancerAdaptsToMeasuredResults()
    {
        using gpufusion::Stop;
        gpufusion::Balancer b(2, gpufusion::Policy::Conservative, 8);
        gpufusion::Plan p = b.plan(1000);
        QVERIFY(!p.multi());
        QVERIFY(p.reason.find("baseline") != std::string::npos);
        b.record(p, fakeReport(1000, 140, { 1000, 0 }, Stop::None)); // a cold first run
        p = b.plan(1000);
        QVERIFY(!p.multi());                                          // a second baseline run replaces it
        b.record(p, fakeReport(1000, 100, { 1000, 0 }, Stop::None));
        QCOMPARE(b.baselineMsPerUnit(), 0.1);

        p = b.plan(1000);
        QVERIFY(p.multi());
        QVERIFY(std::abs(p.share[1] - 0.10) < 1e-9); // the policy's initial share
        QVERIFY(p.share[0] >= 0.8);
        QCOMPARE(p.batch[1] % 8, 0u);
        b.record(p, fakeReport(1000, 85, { 900, 100 }, Stop::ShareReached)); // 17.6 % faster
        QVERIFY(b.hasGain() && b.lastGain() > 0.15);
        QVERIFY(b.helper(1).enabled);
        QVERIFY(b.helper(1).share > 0.10); // raised after a gain that used its whole share

        for (int i = 0; i < 4; ++i) {
            p = b.plan(1000);
            b.record(p, fakeReport(1000, 85, { 850, 150 }, Stop::ShareReached));
        }
        QVERIFY(b.helper(1).share <= gpufusion::limits(gpufusion::Policy::Conservative).maxShare + 1e-9);

        // One run disturbed by other activity (3x slower) is not a miss: nothing changes.
        p = b.plan(1000);
        if (!p.multi()) {
            b.record(p, fakeReport(1000, 100, { 1000, 0 }, Stop::None));
            p = b.plan(1000);
        }
        const double shareBefore = b.helper(1).share;
        b.record(p, fakeReport(1000, 300, { 850, 150 }, Stop::NoUnitsLeft));
        QVERIFY(b.helper(1).enabled);
        QCOMPARE(b.helper(1).strikes, 0);
        QCOMPARE(b.helper(1).share, shareBefore);

        // Splits that stay slower than the primary alone: once that persists over the comparison window,
        // the share is halved, then the helper is switched off (conservative: 2 misses in a row).
        int slowSplits = 0;
        for (int i = 0; i < 12 && b.helper(1).enabled; ++i) {
            p = b.plan(1000);
            if (!p.multi()) { // the periodic baseline refresh
                b.record(p, fakeReport(1000, 100, { 1000, 0 }, Stop::None));
                continue;
            }
            const double before = b.helper(1).share;
            const int strikesBefore = b.helper(1).strikes;
            b.record(p, fakeReport(1000, 130, { 850, 150 }, Stop::ShareReached));
            ++slowSplits;
            if (b.helper(1).enabled && b.helper(1).strikes > strikesBefore) { // a miss: share halved, said why
                QVERIFY(b.helper(1).share < before);
                QVERIFY(b.helper(1).note.find("slower") != std::string::npos);
            }
        }
        QVERIFY2(slowSplits >= 2 && slowSplits <= int(gpufusion::Balancer::kWindow) + 2, qPrintable(QString::number(slowSplits)));
        QVERIFY(!b.helper(1).enabled);
        QVERIFY(b.helper(1).note.find("switched off") != std::string::npos);
        p = b.plan(1000);
        QVERIFY(!p.multi());
        QVERIFY(p.reason.find("switched off") != std::string::npos);
        b.reset();
        QVERIFY(b.helper(1).enabled);

        // A helper whose measured speed is low is never planned above what its speed justifies.
        gpufusion::Balancer perf(2, gpufusion::Policy::Performance);
        for (int i = 0; i < gpufusion::Balancer::kBaselineRuns; ++i) {
            p = perf.plan(1000);
            perf.record(p, fakeReport(1000, 100, { 1000, 0 }, Stop::None));
        }
        p = perf.plan(1000);
        gpufusion::RunReport slowHelper = fakeReport(1000, 95, { 950, 50 }, Stop::ShareReached);
        slowHelper.devices[0].workMs = 95; // 10 units per ms
        slowHelper.devices[1].workMs = 50; // 1 unit per ms
        perf.record(p, slowHelper);
        p = perf.plan(1000);
        QVERIFY(p.multi());
        QVERIFY2(p.share[1] <= 1.1 * 1.0 / 11.0 + 1e-9, qPrintable(QString::number(p.share[1])));

        // The baseline is re-measured after kRefreshEvery splits.
        gpufusion::Balancer r(2, gpufusion::Policy::Performance);
        for (int i = 0; i < gpufusion::Balancer::kBaselineRuns; ++i) {
            p = r.plan(100);
            r.record(p, fakeReport(100, 100, { 100, 0 }, Stop::None));
        }
        int singles = 0;
        for (int i = 0; i < gpufusion::Balancer::kRefreshEvery + 1; ++i) {
            p = r.plan(100);
            singles += p.multi() ? 0 : 1;
            r.record(p, p.multi() ? fakeReport(100, 80, { 85, 15 }, Stop::ShareReached) : fakeReport(100, 100, { 100, 0 }, Stop::None));
        }
        QCOMPARE(singles, 1);
    }

    void modeAndRequests()
    {
        QCOMPARE(gpufusion::mode(), gpufusion::Mode::NotLaunchedByGpuFusion);
        QVERIFY(gpufusion::requestedLuids().empty());
        SetEnvironmentVariableW(L"GPUFUSION_MODE", L"both");
        SetEnvironmentVariableW(L"GPUFUSION_ADAPTERS", L"00000000_00007DEC,,00000000_0000870A");
        QCOMPARE(gpufusion::mode(), gpufusion::Mode::Both);
        const auto luids = gpufusion::requestedLuids();
        QCOMPARE(luids.size(), size_t(2));
        QVERIFY(luids[1] == L"00000000_0000870A");
        SetEnvironmentVariableW(L"GPUFUSION_MODE", L"turbo");
        QCOMPARE(gpufusion::mode(), gpufusion::Mode::NotLaunchedByGpuFusion);
    }

    void adaptersFallBackToTheDefault()
    {
        Microsoft::WRL::ComPtr<IDXGIFactory1> factory;
        QVERIFY(SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))));
        Microsoft::WRL::ComPtr<IDXGIAdapter1> first;
        QVERIFY(SUCCEEDED(factory->EnumAdapters1(0, &first)));
        DXGI_ADAPTER_DESC1 d{};
        first->GetDesc1(&d);

        SetEnvironmentVariableW(L"GPUFUSION_ADAPTERS", L"FFFFFFFF_FFFFFFFF");   // not on this PC
        auto a = gpufusion::adapters(factory.Get());
        QCOMPARE(a.size(), size_t(1));
        DXGI_ADAPTER_DESC1 got{};
        a[0]->GetDesc1(&got);
        QCOMPARE(got.AdapterLuid.LowPart, d.AdapterLuid.LowPart);

        const std::wstring luid = gpufusion::detail::luidText(d.AdapterLuid);
        SetEnvironmentVariableW(L"GPUFUSION_ADAPTERS", (L"ffffffff_ffffffff," + luid).c_str()); // case-insensitive
        a = gpufusion::adapters(factory.Get());
        QCOMPARE(a.size(), size_t(1));
        SetEnvironmentVariableW(L"GPUFUSION_ADAPTERS", (luid + L"," + luid).c_str()); // listed twice: one device, not two
        a = gpufusion::adapters(factory.Get());
        QCOMPARE(a.size(), size_t(1));
        QVERIFY(gpufusion::adapters(nullptr).empty());
    }

    void weightedSplit()
    {
        using R = std::vector<std::pair<uint32_t, uint32_t>>;
        QVERIFY((gpufusion::split(600, { 5.0, 1.0 }) == R{ { 0, 500 }, { 500, 100 } }));
        QVERIFY((gpufusion::split(10, { 1.0, 0.0, 1.0 }) == R{ { 0, 5 }, { 5, 0 }, { 5, 5 } }));
        QVERIFY((gpufusion::split(7, {}) == R{}));
        QVERIFY((gpufusion::split(7, { 0.0, 0.0 }) == R{ { 0, 7 }, { 0, 0 } }));
        uint32_t covered = 0;
        for (const auto& [first, count] : gpufusion::split(1001, { 0.3, 0.3, 0.4 })) {
            QCOMPARE(first, covered);
            covered += count;
        }
        QCOMPARE(covered, 1001u);
    }
};

QTEST_GUILESS_MAIN(TestSdk)
#include "tst_sdk.moc"
