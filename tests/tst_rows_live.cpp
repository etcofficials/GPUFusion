// The SDK's row kernels on the real GPUs (label hardware): every GPU alone and all GPUs together must
// equal the CPU reference bit for bit (odd sizes, halo rows at both edges); a helper that fails every
// batch is replaced by the primary without losing rows; the C library (GPUFusionSDK.dll) gives the same
// result; the game check samples real processes.
#include "games/GameCheck.h"
#include "hardware/AdapterDetector.h"
#include "monitoring/MetricModel.h"

#include <gpufusion/d3d11.h>
#include <gpufusion/gpufusion_c.h>
#include <gpufusion/rows.h>

#include "shaders/blur_ps.h"

#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QTest>

#include <random>

namespace {
std::vector<uint8_t> noise(uint32_t w, uint32_t h, uint32_t seed)
{
    std::vector<uint8_t> v(size_t(w) * h * 4);
    std::mt19937 rng(seed);
    for (auto& b : v)
        b = uint8_t(rng());
    return v;
}

std::vector<gpufusion::Device> hardwareDevices()
{
    Microsoft::WRL::ComPtr<IDXGIFactory1> f;
    CreateDXGIFactory1(IID_PPV_ARGS(&f));
    std::vector<Microsoft::WRL::ComPtr<IDXGIAdapter1>> list;
    Microsoft::WRL::ComPtr<IDXGIAdapter1> a;
    for (UINT i = 0; f->EnumAdapters1(i, &a) != DXGI_ERROR_NOT_FOUND; ++i, a.Reset()) {
        DXGI_ADAPTER_DESC1 d{};
        a->GetDesc1(&d);
        if (!(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            list.push_back(a);
    }
    std::stable_sort(list.begin(), list.end(), [](const auto& x, const auto& y) {
        DXGI_ADAPTER_DESC1 p{}, q{};
        x->GetDesc1(&p);
        y->GetDesc1(&q);
        return p.DedicatedVideoMemory > q.DedicatedVideoMemory;
    });
    return gpufusion::createDevices(list);
}
} // namespace

class TestRowsLive : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void everyGpuAndAllTogetherMatchTheCpu()
    {
        const std::vector<gpufusion::Device> devices = hardwareDevices();
        if (devices.empty())
            QSKIP("No hardware GPU.");
        if (qEnvironmentVariableIntValue("GPUFUSION_EXPECT_REFERENCE_MACHINE") == 1)
            QCOMPARE(devices.size(), size_t(2));
        const uint32_t w = 333, h = 211, r = 5;
        const std::vector<uint8_t> in = noise(w, h, 7);
        std::vector<uint8_t> ref(in.size()), out(in.size());
        gpufusion::blurReference(in.data(), w * 4, ref.data(), w * 4, w, h, r);
        gpufusion::RowJob job{ in.data(), w * 4, out.data(), w * 4, w, h, { r } };

        std::vector<gpufusion::RowExecutor> all;
        for (const auto& d : devices) {
            gpufusion::RowExecutor ex;
            std::string e;
            QVERIFY2(ex.init(d, g_blur_ps, sizeof(g_blur_ps), r, &e), e.c_str());
            // This GPU alone.
            std::vector<gpufusion::RowExecutor> one;
            one.push_back(std::move(ex));
            gpufusion::Balancer b(1, gpufusion::Policy::Conservative, 8);
            std::fill(out.begin(), out.end(), 0);
            const gpufusion::RowRun run = gpufusion::runRows(one, b, job);
            QVERIFY2(run.report.ok, run.report.error.c_str());
            QVERIFY2(out == ref, "single-GPU output differs from the CPU reference");
            all.push_back(std::move(one[0]));
        }
        if (all.size() < 2)
            return;
        // Both GPUs: enough runs for the Balancer to split (baseline, warm-up split, splits).
        gpufusion::Balancer b(all.size(), gpufusion::Policy::Performance, 8);
        int splits = 0;
        for (int i = 0; i < 8; ++i) {
            std::fill(out.begin(), out.end(), 0);
            const gpufusion::RowRun run = gpufusion::runRows(all, b, job);
            QVERIFY2(run.report.ok && run.report.exactCoverage, run.report.error.c_str());
            QVERIFY2(out == ref, "multi-GPU output differs from the CPU reference");
            splits += run.plan.multi() ? 1 : 0;
        }
        QVERIFY(splits >= 1);
    }

    // A helper that fails every batch: its rows are re-run on the primary, the output stays complete.
    void failingHelperFallsBackToThePrimary()
    {
        const std::vector<gpufusion::Device> devices = hardwareDevices();
        if (devices.empty())
            QSKIP("No hardware GPU.");
        const uint32_t w = 256, h = 128, r = 3;
        const std::vector<uint8_t> in = noise(w, h, 11);
        std::vector<uint8_t> ref(in.size()), out(in.size(), 0);
        gpufusion::blurReference(in.data(), w * 4, ref.data(), w * 4, w, h, r);
        std::vector<gpufusion::RowExecutor> ex(2);
        std::string e;
        QVERIFY(ex[0].init(devices[0], g_blur_ps, sizeof(g_blur_ps), r, &e)); // ex[1] stays uninitialized: every batch fails
        gpufusion::Plan plan;
        plan.enabled = { 1, 1 };
        plan.share = { 0.7, 0.3 };
        plan.batch = { 16, 8 };
        plan.granularity = 8;
        const gpufusion::RunReport rep = gpufusion::run(h, plan, [&](gpufusion::Batch& bt) {
            return ex[bt.device].run(in.data(), w * 4, out.data(), w * 4, w, h, std::array<uint32_t, 16>{ r }.data(), bt.first, bt.count);
        });
        QVERIFY(rep.ok && rep.exactCoverage);
        QVERIFY(rep.devices[1].failedUnits > 0);
        QCOMPARE(rep.devices[0].rerunUnits, rep.devices[1].failedUnits);
        QVERIFY(out == ref);
    }

    // The C library: the same result through the plain C interface.
    void cLibraryMatchesTheReference()
    {
        HMODULE dll = LoadLibraryW(reinterpret_cast<const wchar_t*>(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("GPUFusionSDK.dll")).utf16()));
        QVERIFY2(dll, "GPUFusionSDK.dll not found next to the test");
        auto abi = reinterpret_cast<int (*)()>(GetProcAddress(dll, "gf_abi_version"));
        auto create = reinterpret_cast<gf_context* (*)(int, int)>(GetProcAddress(dll, "gf_create"));
        auto destroy = reinterpret_cast<void (*)(gf_context*)>(GetProcAddress(dll, "gf_destroy"));
        auto blurKernel = reinterpret_cast<int (*)(gf_context*)>(GetProcAddress(dll, "gf_kernel_blur"));
        auto runK = reinterpret_cast<int (*)(gf_context*, int, const unsigned char*, size_t, unsigned char*, size_t, unsigned, unsigned, const unsigned*, gf_report*)>(
            GetProcAddress(dll, "gf_run"));
        auto reference = reinterpret_cast<void (*)(const unsigned char*, size_t, unsigned char*, size_t, unsigned, unsigned, unsigned)>(
            GetProcAddress(dll, "gf_blur_reference"));
        auto learned = reinterpret_cast<const char* (*)(gf_context*, int)>(GetProcAddress(dll, "gf_learned"));
        auto restore = reinterpret_cast<int (*)(gf_context*, int, const char*)>(GetProcAddress(dll, "gf_restore"));
        QVERIFY(abi && create && destroy && blurKernel && runK && reference && learned && restore);
        QCOMPARE(abi(), GF_ABI_VERSION);
        gf_context* gf = create(GF_ADAPTERS_ALL, GF_POLICY_PERFORMANCE);
        if (!gf)
            QSKIP("No GPU can run the SDK kernels.");
        const unsigned w = 640, h = 360;
        const std::vector<uint8_t> in = noise(w, h, 3);
        std::vector<uint8_t> ref(in.size()), out(in.size());
        reference(in.data(), w * 4, ref.data(), w * 4, w, h, 4);
        const int k = blurKernel(gf);
        QVERIFY(k >= 0);
        const unsigned params[16] = { 4 };
        bool sawBoth = false;
        for (int i = 0; i < 8; ++i) {
            gf_report rep{};
            QCOMPARE(runK(gf, k, in.data(), w * 4, out.data(), w * 4, w, h, params, &rep), 0);
            QVERIFY(rep.ok);
            QVERIFY(out == ref);
            sawBoth = sawBoth || rep.multi;
        }
        const std::string state = learned(gf, k);
        QVERIFY(state.rfind("gfbalancer2", 0) == 0);
        QCOMPARE(restore(gf, k, state.c_str()), 0);
        QVERIFY(restore(gf, k, "nonsense") != 0);
        if (qEnvironmentVariableIntValue("GPUFUSION_EXPECT_REFERENCE_MACHINE") == 1)
            QVERIFY(sawBoth);
        destroy(gf);
        FreeLibrary(dll);
    }

    // The game check's sampling on this PC: a GPU-bound test window is found on the GT 730 and nothing
    // is started by the check itself.
    void gameCheckSamplesRealProcesses()
    {
        QProcess game;
        game.start(QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("GPUFusionFrameTest.exe")),
                   { QStringLiteral("--api"), QStringLiteral("d3d11"), QStringLiteral("--load"), QStringLiteral("256"), QStringLiteral("--seconds"),
                     QStringLiteral("12") });
        QVERIFY(game.waitForStarted());
        QTest::qWait(1500);
        const gf::hw::DetectionResult detection = gf::hw::detectAdapters(nullptr);
        gf::games::CheckOptions o;
        o.pid = quint32(game.processId());
        o.name = QStringLiteral("GPUFusionFrameTest.exe");
        o.seconds = 5;
        const gf::games::GameCheck c = gf::games::runCheck(gf::mon::targetsFrom(detection), o, {});
        game.waitForFinished(20000);
        QCOMPARE(c.samples, 5);
        QVERIFY2(c.gameLuid != 0, qPrintable(c.notes.join(u' ')));
        QVERIFY2(c.gameGpuMean > 50, qPrintable(QString::number(c.gameGpuMean)));
        if (qEnvironmentVariableIntValue("GPUFUSION_EXPECT_REFERENCE_MACHINE") == 1)
            QVERIFY(c.gameAdapter.contains(QStringLiteral("NVIDIA")));
        QVERIFY(c.cpu.cores > 0);
        QVERIFY(c.checkCpuPercent >= 0 && c.checkCpuPercent < 10);
    }
};

QTEST_GUILESS_MAIN(TestRowsLive)
#include "tst_rows_live.moc"
