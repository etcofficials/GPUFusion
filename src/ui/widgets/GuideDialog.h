#pragma once

#include <QDialog>

class QTextBrowser;

namespace gf::ui {

// Shows GPU Fusion's documentation (Markdown) inside the app: no Markdown viewer needs to be
// installed, links between the documents work, and web links open in the browser.
class GuideDialog : public QDialog {
    Q_OBJECT
public:
    explicit GuideDialog(const QString& document = QStringLiteral("USER_GUIDE.md"), QWidget* parent = nullptr);

    // The folder with the documents: docs\ next to GPUFusion.exe (packages), or the repository's docs\
    // for development builds. Empty if not found.
    static QString docsFolder();
    bool isLoaded() const { return m_loaded; }

private:
    QTextBrowser* m_view;
    bool m_loaded = false;
};

} // namespace gf::ui
