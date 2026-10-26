#include "ui/widgets/GuideDialog.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QAbstractTextDocumentLayout>
#include <QScrollBar>
#include <QTextBlock>
#include <QTextBrowser>
#include <QTimer>
#include <QVBoxLayout>

namespace gf::ui {

QString GuideDialog::docsFolder()
{
    const QDir app(QCoreApplication::applicationDirPath());
    // Packages: <app>\docs. Development builds: <repo>\build\<preset>\bin -> <repo>\docs.
    for (const QString& candidate : { app.filePath(QStringLiteral("docs")), app.filePath(QStringLiteral("../../../docs")) }) {
        if (QFileInfo::exists(QDir(candidate).filePath(QStringLiteral("USER_GUIDE.md"))))
            return QDir::cleanPath(candidate);
    }
    return {};
}

QString GuideDialog::slugOf(const QString& heading)
{
    // GitHub's anchor rule: lower case, keep letters, digits, spaces and hyphens, spaces become hyphens.
    QString s;
    for (const QChar c : heading.toLower())
        if (c.isLetterOrNumber() || c == u'-' || c == u' ')
            s += c == u' ' ? u'-' : c;
    return s;
}

void GuideDialog::jumpToHeading(const QString& fragment)
{
    for (QTextBlock b = m_view->document()->begin(); b.isValid(); b = b.next()) {
        if (b.blockFormat().headingLevel() > 0 && slugOf(b.text()) == fragment) {
            QTextCursor cursor(b);
            m_view->setTextCursor(cursor);
            const qreal top = m_view->document()->documentLayout()->blockBoundingRect(b).top();
            m_view->verticalScrollBar()->setValue(int(top));
            return;
        }
    }
}

GuideDialog::GuideDialog(const QString& document, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("GPU Fusion — user guide"));
    resize(900, 760);
    auto* layout = new QVBoxLayout(this);
    m_view = new QTextBrowser;
    m_view->setAccessibleName(tr("User guide"));
    m_view->setOpenLinks(true);
    m_view->setOpenExternalLinks(false); // handled below: only web links leave the app
    layout->addWidget(m_view, 1);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Close);
    layout->addWidget(box);
    connect(box, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(m_view, &QTextBrowser::anchorClicked, this, [this](const QUrl& url) {
        if (url.scheme() == QLatin1StringView("http") || url.scheme() == QLatin1StringView("https")) {
            QDesktopServices::openUrl(url);
            m_view->setSource(m_view->source()); // stay on the current page
            return;
        }
        // Qt's Markdown import gives headings no anchors, so GitHub-style "#section" links (e.g. the
        // contents list) are resolved here: after the browser's own (unsuccessful) jump, find the
        // heading whose slug matches and show it at the top.
        const QString file = QFileInfo(url.path()).fileName();
        if (url.hasFragment() && (file.isEmpty() || file == QFileInfo(m_view->source().path()).fileName())) {
            const QString fragment = url.fragment();
            QTimer::singleShot(0, this, [this, fragment] { jumpToHeading(fragment); });
        }
    });

    const QString folder = docsFolder();
    if (folder.isEmpty()) {
        m_view->setMarkdown(tr("The user guide was not found next to GPU Fusion (expected in a `docs` folder). It is also in the "
                               "project's repository: `docs/USER_GUIDE.md`."));
        return;
    }
    m_view->setSearchPaths({ folder });
    m_view->setSource(QUrl::fromLocalFile(QDir(folder).filePath(document)), QTextDocument::MarkdownResource);
    m_loaded = !m_view->toPlainText().trimmed().isEmpty();
}

} // namespace gf::ui
