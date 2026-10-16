// End to end: run the real GPUFusion.exe with --export-diagnostics (headless) against a temporary
// data folder and check the Markdown and JSON it writes — including that they contain no personal
// identifiers.
#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QTest>

class TestExportLive : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void exportsAReportWithoutPersonalIdentifiers()
    {
        const QString exe = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("GPUFusion.exe"));
        if (!QFileInfo::exists(exe))
            QSKIP("GPUFusion.exe is not built next to the tests");
        QTemporaryDir dir;
        QVERIFY(dir.isValid());
        const QString base = dir.filePath(QStringLiteral("report"));

        QProcess p;
        p.start(exe, { QStringLiteral("--data-dir=%1").arg(dir.filePath(QStringLiteral("data"))), QStringLiteral("--export-diagnostics"), base });
        QVERIFY(p.waitForStarted(10000));
        QVERIFY2(p.waitForFinished(60000), "export did not finish");
        QCOMPARE(p.exitStatus(), QProcess::NormalExit);
        QCOMPARE(p.exitCode(), 0);

        QFile md(base + QStringLiteral(".md"));
        QFile js(base + QStringLiteral(".json"));
        QVERIFY(md.open(QIODevice::ReadOnly));
        QVERIFY(js.open(QIODevice::ReadOnly));
        const QString text = QString::fromUtf8(md.readAll());
        const QByteArray json = js.readAll();

        QVERIFY(text.contains(QStringLiteral("# GPU Fusion diagnostic report")));
        QVERIFY(text.contains(QStringLiteral("## Graphics adapters")));
        QVERIFY(text.contains(QStringLiteral("## Monitoring")));
        const QJsonDocument doc = QJsonDocument::fromJson(json);
        QVERIFY(doc.isObject());
        QCOMPARE(doc.object().value(QStringLiteral("schemaVersion")).toInt(), 1);
        QVERIFY(!doc.object().value(QStringLiteral("adapters")).toArray().isEmpty());
        // Live readings were captured (the exporter waits for real samples).
        QVERIFY(!doc.object().value(QStringLiteral("monitoring")).toObject().value(QStringLiteral("metrics")).toArray().isEmpty());

        for (const QString& id : { qEnvironmentVariable("COMPUTERNAME"), qEnvironmentVariable("USERPROFILE") }) {
            if (id.size() < 3)
                continue;
            QVERIFY2(!text.contains(id, Qt::CaseInsensitive), qPrintable(QStringLiteral("report contains %1").arg(id)));
            QVERIFY2(!QString::fromUtf8(json).contains(QString(id).replace(u'\\', QStringLiteral("\\\\")), Qt::CaseInsensitive), "json contains an identifier");
        }
        if (qEnvironmentVariableIntValue("GPUFUSION_EXPECT_REFERENCE_MACHINE") == 1) {
            QVERIFY(text.contains(QStringLiteral("NVIDIA GeForce GT 730")));
            QVERIFY(text.contains(QStringLiteral("Intel(R) HD Graphics")));
            QVERIFY(text.contains(QStringLiteral("Legacy WDDM 1.2 driver")));
        }
    }
};

QTEST_GUILESS_MAIN(TestExportLive)
#include "tst_export_live.moc"
