#include "ui/pages/GpusPage.h"

#include "hardware/HardwareService.h"
#include "ui/AppContext.h"
#include "ui/widgets/AdapterCard.h"

#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QVBoxLayout>

namespace gf::ui {

GpusPage::GpusPage(AppContext& context, QWidget* parent) : QWidget(parent), m_context(context)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(12);

    auto* top = new QHBoxLayout;
    m_summary = new QLabel;
    m_summary->setObjectName(QStringLiteral("muted"));
    m_summary->setWordWrap(true);
    m_refresh = new QPushButton(tr("Refresh"));
    m_refresh->setToolTip(tr("Detect adapters again and re-test their capabilities"));
    top->addWidget(m_summary, 1);
    top->addWidget(m_refresh);
    outer->addLayout(top);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* content = new QWidget;
    m_cards = new QVBoxLayout(content);
    m_cards->setContentsMargins(0, 0, 8, 0);
    m_cards->setSpacing(14);
    m_cards->addStretch();
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);

    hw::HardwareService& hardware = m_context.hardware();
    connect(m_refresh, &QPushButton::clicked, this, [&hardware] { hardware.refresh(true); });
    connect(&hardware, &hw::HardwareService::detectionStarted, this, &GpusPage::showDetecting);
    connect(&hardware, &hw::HardwareService::detectionFinished, this, &GpusPage::showResult);
    // Cards use theme colours directly; rebuild them when the theme changes.
    connect(&m_context, &AppContext::settingsChanged, this, [this, &hardware] {
        if (hardware.latest())
            showResult(*hardware.latest());
    });

    if (hardware.latest())
        showResult(*hardware.latest());
    else
        showDetecting();
}

void GpusPage::clearCards()
{
    while (m_cards->count() > 1) { // keep the trailing stretch
        QLayoutItem* item = m_cards->takeAt(0);
        delete item->widget();
        delete item;
    }
}

void GpusPage::showDetecting()
{
    m_refresh->setEnabled(false);
    if (!m_context.hardware().latest())
        m_summary->setText(tr("Detecting graphics adapters…"));
    else
        m_summary->setText(m_summary->text() + tr(" · refreshing…"));
}

void GpusPage::showResult(const hw::DetectionResult& result)
{
    m_refresh->setEnabled(!m_context.hardware().isRunning());
    clearCards();

    const int deviceManagerOnly = int(std::count_if(result.adapters.begin(), result.adapters.end(),
                                                    [](const hw::Adapter& a) { return !a.dxgi; }));
    const auto count = [](int n, const QString& one, const QString& many) { return (n == 1 ? one : many).arg(n); };
    QStringList parts;
    parts << count(result.hardwareCount(), tr("%1 physical GPU available to DirectX"), tr("%1 physical GPUs available to DirectX"));
    if (deviceManagerOnly)
        parts << count(deviceManagerOnly, tr("%1 Device Manager entry not available to DirectX"), tr("%1 Device Manager entries not available to DirectX"));
    if (result.softwareCount())
        parts << count(result.softwareCount(), tr("%1 software renderer (not a GPU)"), tr("%1 software renderers (not GPUs)"));
    if (result.staleDeviceEntries)
        parts << count(result.staleDeviceEntries, tr("%1 stale Device Manager record hidden"), tr("%1 stale Device Manager records hidden"));
    parts << tr("detected %1 in %2 ms").arg(QLocale().toString(result.finishedAt.time(), QLocale::ShortFormat)).arg(result.durationMs);
    m_summary->setText(parts.join(QStringLiteral(" · ")));

    int insertAt = 0;
    if (!result.errors.isEmpty()) {
        auto* banner = new QLabel(tr("Detection was incomplete:\n• %1").arg(result.errors.join(QStringLiteral("\n• "))));
        banner->setObjectName(QStringLiteral("errorBanner"));
        banner->setWordWrap(true);
        m_cards->insertWidget(insertAt++, banner);
    }
    if (result.hardwareCount() == 0) {
        auto* empty = new QLabel(tr("No physical graphics adapter is available to DirectX. Windows may be using only a software "
                                    "renderer; check the entries below and Device Manager for driver problems."));
        empty->setObjectName(QStringLiteral("warningBanner"));
        empty->setWordWrap(true);
        m_cards->insertWidget(insertAt++, empty);
    }
    for (const hw::Adapter& adapter : result.adapters)
        m_cards->insertWidget(insertAt++, new AdapterCard(adapter));
}

} // namespace gf::ui
