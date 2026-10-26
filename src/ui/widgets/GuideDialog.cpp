#include "ui/widgets/GuideDialog.h"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileInfo>
#include <QTextBrowser>
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

GuideDialog::GuideDialog(const QString& document, QWidget* parent) : QDialog(parent)
{
    setWindowTitle(tr("GPU Fusion — user guide"));
    resize(900, 760);
    auto* layout = new QVBoxLayout(this);
    m_view = new QTextBrowser;
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
