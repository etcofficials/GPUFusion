#include "ui/pages/ApplicationsPage.h"

#include "hardware/HardwareService.h"
#include "monitoring/MonitorService.h"
#include "platform/windows/GpuPreferenceReader.h"
#include "profiles/ProfileActions.h"
#include "profiles/ProfileStore.h"
#include "ui/AppContext.h"
#include "ui/Theme.h"
#include "ui/models/ProcessTableModel.h"
#include "ui/widgets/Card.h"
#include "ui/widgets/LiveText.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QProcess>
#include <QPushButton>
#include <QSplitter>
#include <QTableView>
#include <QVBoxLayout>

namespace gf::ui {

using namespace gf::mon;

ApplicationsPage::ApplicationsPage(AppContext& context, QWidget* parent) : QWidget(parent), m_context(context)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(10);

    auto* tools = new QHBoxLayout;
    m_search = new QLineEdit;
    m_search->setPlaceholderText(tr("Search by name, path or PID"));
    m_search->setClearButtonEnabled(true);
    m_search->setAccessibleName(tr("Search processes"));
    m_adapter = new QComboBox;
    m_adapter->setAccessibleName(tr("Filter by GPU"));
    m_adapter->setMinimumWidth(200);
    m_showAll = new QCheckBox(tr("Include processes without GPU use"));
    m_refresh = new QPushButton(tr("Refresh"));
    tools->addWidget(m_search, 1);
    tools->addWidget(m_adapter);
    tools->addWidget(m_showAll);
    tools->addWidget(m_refresh);
    outer->addLayout(tools);

    m_status = new LiveText(1);
    m_status->setTone(LiveText::Tone::Muted);
    outer->addWidget(m_status);

    m_banner = new QLabel;
    m_banner->setObjectName(QStringLiteral("warningBanner"));
    m_banner->setWordWrap(true);
    m_banner->hide();
    outer->addWidget(m_banner);

    m_model = new ProcessTableModel(this);
    m_filter = new ProcessFilterModel(this);
    m_filter->setSourceModel(m_model);
    m_filter->setSortRole(ProcessTableModel::SortRole);
    m_filter->setDynamicSortFilter(true);

    m_table = new QTableView;
    m_table->setModel(m_filter);
    m_table->setSortingEnabled(true);
    m_table->sortByColumn(ProcessTableModel::Usage, Qt::DescendingOrder);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setSelectionMode(QAbstractItemView::SingleSelection);
    m_table->setAlternatingRowColors(true);
    m_table->setShowGrid(false);
    m_table->verticalHeader()->hide();
    m_table->verticalHeader()->setDefaultSectionSize(26);
    // The name column absorbs spare width; the rest are sized for their content so the table fits
    // a ~900 px content area without horizontal scrolling.
    QHeaderView* header = m_table->horizontalHeader();
    header->setStretchLastSection(false);
    header->setSectionResizeMode(QHeaderView::Interactive);
    header->setSectionResizeMode(ProcessTableModel::Application, QHeaderView::Stretch);
    header->setMinimumSectionSize(40);
    m_table->setColumnWidth(ProcessTableModel::Pid, 50);
    m_table->setColumnWidth(ProcessTableModel::Gpu, 140);
    m_table->setColumnWidth(ProcessTableModel::Usage, 80);
    m_table->setColumnWidth(ProcessTableModel::Engine, 90);
    m_table->setColumnWidth(ProcessTableModel::Dedicated, 72);
    m_table->setColumnWidth(ProcessTableModel::Shared, 66);
    m_table->setColumnWidth(ProcessTableModel::Activity, 86);
    m_table->setColumnWidth(ProcessTableModel::Preference, 96);
    m_table->setColumnWidth(ProcessTableModel::Profile, 120);
    m_table->setAccessibleName(tr("Processes and their GPU usage"));

    auto* detailsCard = new Card(tr("Selected process"));
    m_details = new QLabel(tr("Select a process to see its per-engine usage and details."));
    m_details->setObjectName(QStringLiteral("muted"));
    m_details->setWordWrap(true);
    m_details->setTextFormat(Qt::RichText);
    m_details->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_details->setAlignment(Qt::AlignTop | Qt::AlignLeft);
    m_openFolder = new QPushButton(tr("Open file location"));
    m_openFolder->setEnabled(false);
    m_profileButton = new QPushButton(tr("GPU profile…"));
    m_profileButton->setToolTip(tr("Open this program in the GPU Manager (a profile is created if it has none)"));
    m_profileButton->setEnabled(false);
    auto* detailButtons = new QHBoxLayout;
    detailButtons->addWidget(m_openFolder);
    detailButtons->addWidget(m_profileButton);
    detailButtons->addStretch();
    detailsCard->body()->addWidget(m_details, 1);
    detailsCard->body()->addLayout(detailButtons);

    auto* split = new QSplitter(Qt::Vertical);
    split->addWidget(m_table);
    split->addWidget(detailsCard);
    split->setStretchFactor(0, 3);
    split->setStretchFactor(1, 1);
    split->setChildrenCollapsible(false);
    outer->addWidget(split, 1);

    m_coverage = new QLabel;
    m_coverage->setObjectName(QStringLiteral("muted"));
    m_coverage->setWordWrap(true);
    outer->addWidget(m_coverage);

    mon::MonitorService& monitor = m_context.monitor();
    connect(m_search, &QLineEdit::textChanged, m_filter, &ProcessFilterModel::setSearch);
    connect(m_adapter, &QComboBox::currentIndexChanged, this, [this] { m_filter->setAdapter(m_adapter->currentData().value<quint64>()); });
    connect(m_showAll, &QCheckBox::toggled, m_filter, &ProcessFilterModel::setShowAll);
    connect(m_refresh, &QPushButton::clicked, this, [this, &monitor] {
        reloadPreferences();
        monitor.refreshProcesses();
    });
    connect(&monitor, &MonitorService::processesUpdated, this, &ApplicationsPage::onProcesses);
    connect(&monitor, &MonitorService::stateChanged, this, &ApplicationsPage::onProcesses);
    connect(&m_context.hardware(), &hw::HardwareService::detectionFinished, this, &ApplicationsPage::onAdaptersChanged);
    connect(m_table->selectionModel(), &QItemSelectionModel::selectionChanged, this, &ApplicationsPage::updateDetails);
    connect(m_table, &QTableView::pressed, this, [this] { m_userPicked = true; });
    connect(m_profileButton, &QPushButton::clicked, this, [this] {
        if (const ProcessSample* p = m_model->process(m_detailsKey); p && !p->path.isEmpty())
            Q_EMIT profileRequested(QDir::toNativeSeparators(p->path));
    });
    connect(m_openFolder, &QPushButton::clicked, this, [this] {
        if (const ProcessSample* p = m_model->process(m_detailsKey); p && !p->path.isEmpty())
            QProcess::startDetached(QStringLiteral("explorer.exe"), { QStringLiteral("/select,"), QDir::toNativeSeparators(p->path) });
    });

    // Preferences change rarely and only through Windows Settings (or Phase 5); re-read every 10 s while visible.
    m_preferenceTimer.setInterval(10'000);
    connect(&m_preferenceTimer, &QTimer::timeout, this, &ApplicationsPage::reloadPreferences);

    onAdaptersChanged();
    onProcesses();
}

void ApplicationsPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    reloadPreferences();
    m_preferenceTimer.start();
    m_context.monitor().setProcessTracking(true);
}

void ApplicationsPage::hideEvent(QHideEvent* event)
{
    m_preferenceTimer.stop();
    m_context.monitor().setProcessTracking(false);
    QWidget::hideEvent(event);
}

void ApplicationsPage::reloadPreferences()
{
    const win::GpuPreferences prefs = win::readUserGpuPreferences();
    m_model->setPreferences(prefs.byPath);
    const auto& detection = m_context.hardware().latest();
    if (!m_context.profiles())
        return;
    // Re-read the profiles only when the profile folder or the GPUs changed: this runs every 10 s on the
    // UI thread, and the data folder may be on a slow or failing disk.
    const QList<pref::SelectableGpu> gpus = detection ? profiles::selectableGpus(*detection) : QList<pref::SelectableGpu>{};
    QStringList signature;
    for (const QFileInfo& fi : QDir(m_context.profiles()->directory()).entryInfoList({ QStringLiteral("*.json") }, QDir::Files))
        signature << QStringLiteral("%1|%2|%3").arg(fi.fileName()).arg(fi.size()).arg(fi.lastModified().toMSecsSinceEpoch());
    for (const pref::SelectableGpu& g : gpus)
        signature << g.luid;
    const QString joined = signature.join(u';');
    if (joined == m_profileSignature)
        return;
    m_profileSignature = joined;
    m_model->setProfiles(m_context.profiles()->list(), gpus);
}

void ApplicationsPage::onAdaptersChanged()
{
    const auto& detection = m_context.hardware().latest();
    QHash<quint64, ProcessTableModel::AdapterInfo> adapters;
    const quint64 current = m_adapter->currentData().value<quint64>();
    m_adapter->blockSignals(true);
    m_adapter->clear();
    m_adapter->addItem(tr("All GPUs and renderers"), QVariant::fromValue<quint64>(0));
    if (detection) {
        for (const MonitorTarget& t : targetsFrom(*detection)) {
            adapters.insert(t.luid, { t.name, t.software });
            m_adapter->addItem(t.software ? tr("%1 (software)").arg(t.name) : t.name, QVariant::fromValue(t.luid));
        }
    }
    m_adapter->setCurrentIndex(std::max(0, m_adapter->findData(QVariant::fromValue(current))));
    m_adapter->blockSignals(false);
    m_filter->setAdapter(m_adapter->currentData().value<quint64>());
    m_model->setAdapters(adapters);
}

void ApplicationsPage::onProcesses()
{
    const mon::MonitorService& monitor = m_context.monitor();
    if (monitor.isPaused()) {
        m_banner->setText(tr("Monitoring is paused, so the list is not updating. Resume it on the Dashboard, or press Refresh for a one-off update "
                             "(GPU usage needs two samples: press Refresh twice a few seconds apart)."));
        m_banner->show();
    } else if (!m_banner->isHidden()) {
        m_banner->hide();
    }

    const auto& snap = monitor.latestProcesses();
    if (!snap) {
        m_status->setText(tr("Collecting process information…"));
        return;
    }
    m_model->update(*snap);

    int withFootprint = 0, active = 0;
    for (const ProcessSample& p : snap->processes) {
        bool footprint = false, isActive = false;
        for (const ProcessAdapterUsage& u : p.adapters) {
            const Activity a = activityOf(u);
            footprint = footprint || a != Activity::None;
            isActive = isActive || a == Activity::Active;
        }
        withFootprint += footprint ? 1 : 0;
        active += isActive ? 1 : 0;
    }
    m_status->setText(tr("Updated %1 · %2 processes · %3 using a GPU now · %4 holding GPU memory · collected in %5 ms")
                          .arg(QLocale().toString(snap->wallTime.time(), QStringLiteral("HH:mm:ss")))
                          .arg(snap->processCount).arg(active).arg(withFootprint).arg(snap->collectionMs, 0, 'f', 1));

    // Coverage: say exactly which processes each source can see.
    QStringList coverage;
    if (!snap->pdhAdapterNames.isEmpty()) {
        coverage << (snap->pdhError.isEmpty()
                         ? tr("%1: Windows performance counters, all processes.").arg(snap->pdhAdapterNames.join(QStringLiteral(", ")))
                         : tr("%1: performance counters unavailable (%2).").arg(snap->pdhAdapterNames.join(QStringLiteral(", ")), snap->pdhError));
    }
    if (!snap->legacyAdapterNames.isEmpty()) {
        coverage << tr("%1 (driver older than WDDM 2.0): D3DKMT, %2 of %3 processes inspectable. Protected and other accounts' processes "
                       "(such as dwm.exe and csrss.exe) need administrator rights and are not shown for this adapter; GPU Fusion does not "
                       "request elevation.")
                        .arg(snap->legacyAdapterNames.join(QStringLiteral(", ")))
                        .arg(snap->legacyInspectable).arg(snap->legacyInspectable + snap->legacyNotInspectable);
    }
    coverage << tr("Per-process figures: they need not add up to the adapter totals on the Dashboard.");
    const QString text = coverage.join(QLatin1Char(' '));
    if (m_coverage->text() != text) // QLabel::setText re-lays out the page; only when it changes
        m_coverage->setText(text);
    // Once a sample has real rates (priming samples have Unknown utilization everywhere, so the sort
    // order means nothing), select the top row — the busiest GPU user — for the details panel. Qt may
    // already have made a row current when the table got focus; that is not the user's choice, so it
    // is replaced once, unless the user has clicked a row.
    if (snap->ratesReady && !m_autoSelected && !m_userPicked && m_filter->rowCount() > 0) {
        m_autoSelected = true;
        m_table->selectRow(0);
        m_table->scrollToTop();
    }
    updateDetails();
}

QString ApplicationsPage::selectedKey() const
{
    const QModelIndexList rows = m_table->selectionModel()->selectedRows();
    return rows.isEmpty() ? QString() : rows.first().data(ProcessTableModel::KeyRole).toString();
}

void ApplicationsPage::updateDetails()
{
    const QString key = selectedKey();
    if (key.isEmpty() && m_detailsKey.isEmpty())
        return;
    if (!key.isEmpty())
        m_detailsKey = key;
    const ThemeColors& c = colors();
    const ProcessSample* p = m_model->process(m_detailsKey);
    QString html;
    if (!p) {
        html = tr("The selected process has exited.");
        m_openFolder->setEnabled(false);
        m_profileButton->setEnabled(false);
    } else {
        html = QStringLiteral("<b style='color:%1'>%2</b> &nbsp; PID %3<br>%4<br>")
                   .arg(c.text.name(), p->name.toHtmlEscaped()).arg(p->pid)
                   .arg((p->path.isEmpty() ? p->pathError : QDir::toNativeSeparators(p->path)).toHtmlEscaped());
        const GpuPreference pref = m_model->preferenceFor(p->path);
        html += tr("Windows graphics preference: <b>%1</b>").arg(p->path.isEmpty() ? tr("unknown (path not readable)") : gpuPreferenceText(pref));
        if (pref == GpuPreference::PowerSaving || pref == GpuPreference::HighPerformance)
            html += tr(" — a preference only influences apps that let Windows pick the adapter; the rows above show where this one actually ran.");
        html += QStringLiteral("<br>");
        const profiles::LiveCheck check = m_model->profileCheck(*p);
        if (check.state != profiles::LiveCheck::State::NoProfile) {
            const QColor tone = check.state == profiles::LiveCheck::State::Matches ? c.good
                              : check.state == profiles::LiveCheck::State::Elsewhere ? c.warn : c.textMuted;
            html += tr("GPU profile: <span style='color:%1'>%2</span><br>").arg(tone.name(), check.detail.toHtmlEscaped());
        }
        if (p->adapters.isEmpty())
            html += tr("No GPU footprint on any adapter GPU Fusion can inspect for this process.");
        for (const ProcessAdapterUsage& u : p->adapters) {
            QStringList engines;
            for (const EngineSample& e : u.engines)
                engines << QStringLiteral("%1 %2").arg(e.name.toHtmlEscaped(), formatMetric(e.utilization, Unit::Percent));
            const bool software = m_model->isSoftwareAdapter(u.luid);
            html += QStringLiteral("<br><b style='color:%1'>%2</b> — %3, dedicated %4, shared %5 <span>(source: %6)</span><br>%7%8")
                        .arg(c.text.name(), m_model->adapterName(u.luid).toHtmlEscaped(), activityText(activityOf(u)),
                             formatMetric(u.dedicated, Unit::Bytes), formatMetric(u.shared, Unit::Bytes), u.source.toHtmlEscaped(),
                             engines.isEmpty() ? tr("No engine readings") : engines.join(QStringLiteral(" · ")),
                             software && activityOf(u) == Activity::Active
                                 ? tr("<br>This process is rendering on the CPU through the software renderer (WARP), not on a GPU.")
                                 : QString());
        }
        m_openFolder->setEnabled(!p->path.isEmpty());
        m_profileButton->setEnabled(!p->path.isEmpty() && m_context.profiles());
    }
    if (m_details->text() != html) // QLabel::setText re-lays out the page
        m_details->setText(html);
}

} // namespace gf::ui
