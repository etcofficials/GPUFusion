// Tests for the header-only GPU Fusion SDK: mode and adapter requests from the environment GPU Fusion
// sets, adapter resolution against the real DXGI factory, and the weighted split.
#include <gpufusion/gpufusion.h>

#include <QTest>

class TestSdk : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void cleanup()
    {
        SetEnvironmentVariableW(L"GPUFUSION_MODE", nullptr);
        SetEnvironmentVariableW(L"GPUFUSION_ADAPTERS", nullptr);
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
