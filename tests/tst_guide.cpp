// The in-app user guide: the documents are found (the repository's docs folder for development
// builds; docs\ next to GPUFusion.exe in packages), Markdown is rendered, and links between the
// documents open the linked document inside the viewer.
#include "ui/widgets/GuideDialog.h"

#include <QFile>
#include <QTest>
#include <QTextBrowser>

class TestGuide : public QObject {
    Q_OBJECT
private Q_SLOTS:
    void guideIsFoundRenderedAndLinked()
    {
        const QString folder = gf::ui::GuideDialog::docsFolder();
        QVERIFY2(!folder.isEmpty(), "docs folder with USER_GUIDE.md not found");
        gf::ui::GuideDialog dialog;
        QVERIFY(dialog.isLoaded());
        auto* view = dialog.findChild<QTextBrowser*>();
        QVERIFY(view);
        const QString text = view->toPlainText();
        QVERIFY(text.contains(QStringLiteral("Choose the GPU for a program")));
        QVERIFY(!text.contains(QStringLiteral("## ")));            // rendered, not shown as raw Markdown

        // Every document the guide links to by file name exists next to it (they are shipped together).
        QFile guide(QDir(folder).filePath(QStringLiteral("USER_GUIDE.md")));
        QVERIFY(guide.open(QIODevice::ReadOnly));
        const QString source = QString::fromUtf8(guide.readAll());
        static const QRegularExpression link(QStringLiteral(R"(\]\(([A-Z_]+\.md)\))"));
        int links = 0;
        for (auto it = link.globalMatch(source); it.hasNext(); ++links) {
            const QString target = it.next().captured(1);
            QVERIFY2(QFile::exists(QDir(folder).filePath(target)), qPrintable(target));
        }
        QVERIFY(links >= 3);

        view->setSource(QUrl(QStringLiteral("TROUBLESHOOTING.md")), QTextDocument::MarkdownResource);
        QVERIFY(view->toPlainText().contains(QStringLiteral("GPU Manager and GPU profiles")));
    }
};

QTEST_MAIN(TestGuide)
#include "tst_guide.moc"
