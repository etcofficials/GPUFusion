#include "ui/pages/DashboardPage.h"

#include "hardware/HardwareService.h"
#include "monitoring/MonitorService.h"
#include "ui/AppContext.h"
#include "ui/widgets/LiveText.h"
#include "ui/widgets/MonitorCard.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace gf::ui {

DashboardPage::DashboardPage(AppContext& context, QWidget* parent) : QWidget(parent), m_context(context)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(12);

    auto* top = new QHBoxLayout;
    m_status = new LiveText(1); // changes every sample: repaint only, no relayout
    m_status->setTone(LiveText::Tone::Muted);
    m_pause = new QPushButton;
    top->addWidget(m_status, 1);
    top->addWidget(m_pause);
    outer->addLayout(top);

    m_banner = new QLabel;
    m_banner->setObjectName(QStringLiteral("warningBanner"));
    m_banner->setWordWrap(true);
    m_banner->hide();
    outer->addWidget(m_banner);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* content = new QWidget;
    m_cards = new QVBoxLayout(content);
    m_cards->setContentsMargins(0, 0, 8, 0);
    m_cards->setSpacing(14);
    m_cards->addStretch();
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);

    mon::MonitorService& monitor = m_context.monitor();
    connect(m_pause, &QPushButton::clicked, this, [&monitor] { monitor.setPaused(!monitor.isPaused()); });
    connect(&monitor, &mon::MonitorService::updated, this, &DashboardPage::refresh);
    connect(&monitor, &mon::MonitorService::stateChanged, this, &DashboardPage::updateStatus);
    connect(&m_context.hardware(), &hw::HardwareService::detectionFinished, this, &DashboardPage::rebuildCards);
    connect(&m_context, &AppContext::settingsChanged, this, &DashboardPage::rebuildCards); // theme colours

    rebuildCards();
    updateStatus();
}

void DashboardPage::rebuildCards()
{
    while (m_cards->count() > 1) {
        QLayoutItem* item = m_cards->takeAt(0);
        delete item->widget();
        delete item;
    }
    m_cardByLuid.clear();

    const auto& detection = m_context.hardware().latest();
    if (!detection) {
        auto* waiting = new QLabel(tr("Detecting graphics adapters…"));
        waiting->setObjectName(QStringLiteral("muted"));
        m_cards->insertWidget(0, waiting);
        return;
    }
    int at = 0;
    for (const hw::Adapter& a : detection->adapters) {
        if (!a.dxgi || a.isSoftware())
            continue;
        const QString cls = a.classification.value == hw::AdapterClass::Discrete ? tr("Discrete")
                          : a.classification.value == hw::AdapterClass::Integrated ? tr("Integrated") : tr("Type unknown");
        mon::MonitorTarget target;
        for (const mon::MonitorTarget& t : mon::targetsFrom(*detection)) {
            if (t.luid == a.dxgi->luid)
                target = t;
        }
        auto* card = new MonitorCard(target, cls);
        m_cards->insertWidget(at++, card);
        m_cardByLuid.insert(target.luid, card);
    }
    if (at == 0) {
        auto* none = new QLabel(tr("No physical GPU is available to DirectX, so there is nothing to monitor. See the GPUs page."));
        none->setObjectName(QStringLiteral("warningBanner"));
        none->setWordWrap(true);
        m_cards->insertWidget(0, none);
    }
    refresh();
}

void DashboardPage::refresh()
{
    const mon::MonitorService& monitor = m_context.monitor();
    const auto& snap = monitor.latest();
    if (snap) {
        for (const mon::AdapterSample& s : snap->adapters) {
            if (MonitorCard* card = m_cardByLuid.value(s.luid))
                card->showSample(s, monitor.history(), snap->steadyMs, monitor.intervalMs(), monitor.historySeconds());
        }
    }
    updateStatus();
}

void DashboardPage::updateStatus()
{
    const mon::MonitorService& monitor = m_context.monitor();
    m_pause->setText(monitor.isPaused() ? tr("Resume") : tr("Pause"));
    m_pause->setToolTip(monitor.isPaused() ? tr("Resume live monitoring") : tr("Stop polling the GPUs; graphs keep their history"));

    const auto& snap = monitor.latest();
    QStringList parts;
    parts << (monitor.isPaused() ? tr("Paused") : tr("Live, every %1 s").arg(monitor.intervalMs() / 1000.0, 0, 'g', 3));
    if (snap) {
        parts << tr("last sample %1").arg(QLocale().toString(snap->wallTime.time(), QStringLiteral("HH:mm:ss")));
        parts << tr("collection %1 ms").arg(snap->collectionMs, 0, 'f', 2);
        if (snap->processCpuPercent.isOk())
            parts << tr("GPU Fusion CPU %1").arg(mon::formatMetric(snap->processCpuPercent, mon::Unit::Percent));
    }
    m_status->setText(parts.join(QStringLiteral(" · ")));

    if (monitor.isPaused()) {
        m_banner->setText(tr("Monitoring is paused. The values below are the last readings taken, not current ones."));
        m_banner->show();
    } else if (monitor.isStale()) {
        m_banner->setText(tr("No new readings for several seconds. The values below are out of date; a graphics driver "
                             "reset or a busy system can cause this. GPU Fusion will re-detect adapters if one disappeared."));
        m_banner->show();
    } else if (!m_banner->isHidden()) {
        m_banner->hide();
    }
}

} // namespace gf::ui
