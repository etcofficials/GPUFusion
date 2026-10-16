#pragma once

#include <QMainWindow>
#include <QStringList>

class QLabel;
class QListWidget;
class QStackedWidget;

namespace gf::ui {

class AppContext;

class MainWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(AppContext& context, QWidget* parent = nullptr);

    // Page ids: dashboard, gpus, applications, preferences, workloads, benchmarks, diagnostics, settings
    bool showPage(const QString& id);
    static QStringList pageIds();

protected:
    void closeEvent(QCloseEvent* event) override;
    bool nativeEvent(const QByteArray& eventType, void* message, qintptr* result) override;
private:
    struct PageInfo { QString id, title, subtitle; };
    void addPage(const PageInfo& info, QWidget* page);
    void onPageSelected(int index);

    AppContext& m_context;
    QListWidget* m_nav;
    QStackedWidget* m_stack;
    QLabel* m_title;
    QLabel* m_subtitle;
    QList<PageInfo> m_pages;
};

} // namespace gf::ui
