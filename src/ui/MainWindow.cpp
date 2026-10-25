#include "ui/MainWindow.h"

#include "core/Logging.h"
#include "core/Version.h"
#include "ui/AppContext.h"
#include "hardware/HardwareService.h"
#include "ui/pages/ApplicationsPage.h"
#include "ui/pages/BenchmarksPage.h"
#include "ui/pages/DashboardPage.h"
#include "ui/pages/DiagnosticsPage.h"
#include "ui/pages/GpuManagerPage.h"
#include "ui/pages/GpusPage.h"
#include "ui/pages/PlaceholderPage.h"
#include "ui/pages/PreferencesPage.h"
#include "ui/pages/SettingsPage.h"
#include "ui/pages/WorkloadLabPage.h"

#include <QCloseEvent>
#include <QDir>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QStackedWidget>
#include <QStatusBar>
#include <QVBoxLayout>

#include <windows.h>
#include <dbt.h>

namespace gf::ui {

MainWindow::MainWindow(AppContext& context, QWidget* parent) : QMainWindow(parent), m_context(context)
{
    setWindowTitle(QStringLiteral("GPU Fusion"));
    setMinimumSize(900, 600);

    auto* central = new QWidget;
    auto* root = new QHBoxLayout(central);
    root->setContentsMargins(0, 0, 0, 0);
    root->setSpacing(0);

    // Sidebar
    auto* sidebar = new QWidget;
    sidebar->setFixedWidth(210);
    auto* sideLayout = new QVBoxLayout(sidebar);
    sideLayout->setContentsMargins(0, 0, 0, 0);
    sideLayout->setSpacing(0);
    auto* appTitle = new QLabel(QStringLiteral("GPU Fusion"));
    appTitle->setObjectName(QStringLiteral("appTitle"));
    auto* appVersion = new QLabel(tr("Version %1").arg(QString::fromLatin1(version::kString)));
    appVersion->setObjectName(QStringLiteral("appVersion"));
    m_nav = new QListWidget;
    m_nav->setObjectName(QStringLiteral("navigation"));
    m_nav->setFrameShape(QFrame::NoFrame);
    m_nav->setAccessibleName(tr("Sections"));
    sideLayout->addWidget(appTitle);
    sideLayout->addWidget(appVersion);
    sideLayout->addWidget(m_nav, 1);
    root->addWidget(sidebar);

    // Content: header + pages
    auto* contentArea = new QWidget;
    auto* contentLayout = new QVBoxLayout(contentArea);
    contentLayout->setContentsMargins(28, 22, 28, 18);
    contentLayout->setSpacing(4);
    m_title = new QLabel;
    m_title->setObjectName(QStringLiteral("pageTitle"));
    m_subtitle = new QLabel;
    m_subtitle->setObjectName(QStringLiteral("pageSubtitle"));
    m_subtitle->setWordWrap(true);
    m_stack = new QStackedWidget;
    contentLayout->addWidget(m_title);
    contentLayout->addWidget(m_subtitle);
    contentLayout->addSpacing(14);
    contentLayout->addWidget(m_stack, 1);
    root->addWidget(contentArea, 1);
    setCentralWidget(central);

    addPage({ QStringLiteral("dashboard"), tr("Dashboard"), tr("Live readings for every physical GPU. Values a driver does not expose are labelled, never shown as zero.") },
            new DashboardPage(m_context));
    addPage({ QStringLiteral("gpus"), tr("GPUs"), tr("Graphics adapters Windows exposes, their drivers and tested capabilities.") },
            new GpusPage(m_context));
    addPage({ QStringLiteral("applications"), tr("Applications"), tr("Which processes use which GPU, per process. Adapter-wide totals are on the Dashboard.") },
            new ApplicationsPage(m_context));
    addPage({ QStringLiteral("manager"), tr("GPU Manager"), tr("A GPU profile for any program: choose its GPU, apply, launch and verify what it really uses.") },
            new GpuManagerPage(m_context));
    addPage({ QStringLiteral("preferences"), tr("GPU Preferences"), tr("The Windows per-app graphics preference — what it does here, and changing it safely.") },
            new PreferencesPage(m_context));
    addPage({ QStringLiteral("workloads"), tr("Workload Lab"), tr("Can both GPUs make one job faster? Measured experiments, compared with the best single device.") },
            new WorkloadLabPage(m_context));
    addPage({ QStringLiteral("benchmarks"), tr("Benchmarks"), tr("Repeatable, verified measurements on each GPU and the CPU, compared only with equivalent earlier runs.") },
            new BenchmarksPage(m_context));
    addPage({ QStringLiteral("diagnostics"), tr("Diagnostics"), tr("Application, storage and graphics-system information.") },
            new DiagnosticsPage(m_context));
    addPage({ QStringLiteral("settings"), tr("Settings"), tr("Preferences are validated and saved to the data folder.") },
            new SettingsPage(m_context));

    connect(m_nav, &QListWidget::currentRowChanged, this, &MainWindow::onPageSelected);
    m_nav->setCurrentRow(0);

    auto* dataLabel = new QLabel(tr("Data: %1").arg(QDir::toNativeSeparators(m_context.data().root)));
    statusBar()->addPermanentWidget(dataLabel);
    if (m_context.data().source == DataLocation::Source::Fallback)
        statusBar()->showMessage(m_context.data().problem);
    else if (!m_context.loadResult().warnings.isEmpty())
        statusBar()->showMessage(tr("Settings %1 — see Settings for details.").arg(describe(m_context.loadResult().status)));

    if (!m_context.settings().windowGeometry.isEmpty())
        restoreGeometry(m_context.settings().windowGeometry);
    else
        resize(1180, 760);
}

void MainWindow::addPage(const PageInfo& info, QWidget* page)
{
    m_pages.append(info);
    m_stack->addWidget(page);
    m_nav->addItem(info.title);
}

void MainWindow::onPageSelected(int index)
{
    if (index < 0 || index >= m_pages.size())
        return;
    m_stack->setCurrentIndex(index);
    m_title->setText(m_pages[index].title);
    m_subtitle->setText(m_pages[index].subtitle);
    qCDebug(lcUi) << "Page" << m_pages[index].id;
}

QStringList MainWindow::pageIds()
{
    return { QStringLiteral("dashboard"), QStringLiteral("gpus"), QStringLiteral("applications"), QStringLiteral("manager"), QStringLiteral("preferences"),
             QStringLiteral("workloads"), QStringLiteral("benchmarks"), QStringLiteral("diagnostics"), QStringLiteral("settings") };
}

bool MainWindow::showPage(const QString& id)
{
    for (int i = 0; i < m_pages.size(); ++i) {
        if (m_pages[i].id == id) {
            m_nav->setCurrentRow(i);
            return true;
        }
    }
    return false;
}

bool MainWindow::nativeEvent(const QByteArray& eventType, void* message, qintptr* result)
{
    // Windows broadcasts DBT_DEVNODES_CHANGED to top-level windows whenever devices are added,
    // removed, enabled or disabled (including a GPU driver restart).
    if (eventType == "windows_generic_MSG") {
        const auto* msg = static_cast<const MSG*>(message);
        if (msg->message == WM_DEVICECHANGE && msg->wParam == DBT_DEVNODES_CHANGED)
            m_context.hardware().deviceConfigurationChanged();
    }
    return QMainWindow::nativeEvent(eventType, message, result);
}

void MainWindow::closeEvent(QCloseEvent* event)
{
    m_context.rememberWindowGeometry(saveGeometry());
    QMainWindow::closeEvent(event);
}

} // namespace gf::ui
