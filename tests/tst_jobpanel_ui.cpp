// Drives the Workload Lab's job batch panel on the real GPUs: built-in H.264 decoding of a small
// synthetic clip, once and as a measured comparison. Changes nothing outside a temporary folder.
#include "core/AppPaths.h"
#include "core/Settings.h"
#include "hardware/HardwareService.h"
#include "monitoring/MonitorService.h"
#include "preferences/PreferenceService.h"
#include "ui/AppContext.h"
#include "ui/widgets/JobBatchPanel.h"
#include "workloads/VideoDecode.h"

#include <QSignalSpy>
#include <QTemporaryDir>
#include <QTest>

using namespace gf;

class TestJobPanelUi : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void decodeBatchThroughThePanel()
    {
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        DataLocation data = resolveDataLocation(dir.path(), dir.filePath(QStringLiteral("data")), dir.filePath(QStringLiteral("fb")));
        QVERIFY(ensureDataLayout(data).isEmpty());
        hw::HardwareService hardware;
        QSignalSpy detected(&hardware, &hw::HardwareService::detectionFinished);
        hardware.refresh(true);
        QVERIFY(detected.wait(20000));
        int gpus = 0;
        for (const hw::Adapter& a : hardware.latest()->adapters)
            gpus += a.dxgi && !a.isSoftware() ? 1 : 0;
        if (gpus < 2)
            QSKIP("Needs two GPUs");

        work::ComScope com;
        QString error;
        QVERIFY2(work::startMediaFoundation(&error), qPrintable(error));
        work::TestClipSpec spec;
        spec.width = 320;
        spec.height = 240;
        spec.frames = 60;
        spec.bitrate = 500'000;
        const QString inputs = dir.filePath(QStringLiteral("clips"));
        const QString clip = work::ensureTestClip(inputs, spec, &error);
        QVERIFY2(!clip.isEmpty(), qPrintable(error));
        for (int i = 1; i < 4; ++i)
            QVERIFY(QFile::copy(clip, QDir(inputs).filePath(QStringLiteral("copy%1.mp4").arg(i))));

        SettingsStore settingsStore(data.settingsFile());
        auto loaded = settingsStore.load();
        mon::MonitorService monitor(hardware, loaded.settings);
        pref::PreferenceService prefs(data.configDir(), data.reportsDir(), QString());
        ui::AppContext context(data, &settingsStore, loaded, &hardware, &monitor, &prefs);
        ui::JobBatchPanel panel(context);
        panel.show();
        QSignalSpy finished(&panel, &ui::JobBatchPanel::finished);

        panel.configure(QStringLiteral("decode"), QDir::toNativeSeparators(inputs), QString(), QString(), QStringLiteral("all"), 0);
        panel.start();
        QVERIFY(panel.isRunning());
        QVERIFY(finished.wait(60000));
        qInfo().noquote() << panel.resultText();
        QVERIFY2(panel.resultText().contains(QStringLiteral("4 job(s)")) && panel.resultText().contains(QStringLiteral("0 failed")),
                 qPrintable(panel.resultText()));

        panel.configure(QStringLiteral("decode"), QDir::toNativeSeparators(inputs), QString(), QString(), QStringLiteral("all"), 2);
        panel.start();
        QVERIFY(finished.wait(120000));
        qInfo().noquote() << panel.resultText();
        const QString text = panel.resultText();
        QVERIFY2(text.contains(QStringLiteral("Faster")) || text.contains(QStringLiteral("Slower")) ||
                 text.contains(QStringLiteral("No significant difference")), qPrintable(text));
        QVERIFY(text.contains(QStringLiteral("jobs at a time"))); // the concurrency control was measured
    }
};

QTEST_MAIN(TestJobPanelUi)
#include "tst_jobpanel_ui.moc"
