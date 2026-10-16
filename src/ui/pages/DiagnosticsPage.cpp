#include "ui/pages/DiagnosticsPage.h"

#include "core/Logging.h"
#include "diagnostics/DiagnosticReport.h"
#include "hardware/HardwareService.h"
#include "monitoring/MonitorService.h"
#include "preferences/PreferenceService.h"
#include "ui/AppContext.h"
#include "ui/DiagnosticsCollector.h"
#include "ui/Theme.h"
#include "ui/widgets/Card.h"

#include <QApplication>
#include <QClipboard>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace gf::ui {

using diag::Issue;

namespace {

QLabel* muted(const QString& text)
{
    auto* l = new QLabel(text);
    l->setObjectName(QStringLiteral("muted"));
    l->setWordWrap(true);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return l;
}

// A read-only table sized to its content (no inner scrolling), long cells wrapped.
QTableWidget* tableFrom(const diag::Table& t, int firstColumnWidth)
{
    auto* w = new QTableWidget(int(t.rows.size()), int(t.header.size()));
    w->setHorizontalHeaderLabels(t.header);
    w->verticalHeader()->hide();
    w->setEditTriggers(QAbstractItemView::NoEditTriggers);
    w->setSelectionMode(QAbstractItemView::NoSelection);
    w->setFocusPolicy(Qt::NoFocus);
    w->setShowGrid(false);
    w->setAlternatingRowColors(true);
    w->setWordWrap(true);
    w->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    w->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    for (int r = 0; r < t.rows.size(); ++r) {
        for (int c = 0; c < t.rows[r].size() && c < t.header.size(); ++c) {
            auto* item = new QTableWidgetItem(t.rows[r][c]);
            item->setToolTip(t.rows[r][c]);
            if (c > 0 && (t.rows[r][c] == QLatin1StringView("Unsupported") || t.rows[r][c] == QLatin1StringView("Not feasible")))
                item->setForeground(colors().textMuted);
            w->setItem(r, c, item);
        }
    }
    QHeaderView* h = w->horizontalHeader();
    h->setSectionResizeMode(QHeaderView::Stretch);
    h->setSectionResizeMode(0, QHeaderView::Fixed);
    w->setColumnWidth(0, firstColumnWidth);
    w->resizeRowsToContents();
    int height = h->height() + 4;
    for (int r = 0; r < w->rowCount(); ++r)
        height += w->rowHeight(r);
    w->setFixedHeight(height);
    return w;
}

QString issueHtml(const QList<Issue>& issues)
{
    if (issues.isEmpty())
        return QObject::tr("No issues found.");
    const ThemeColors& c = colors();
    QString html;
    for (const Issue& i : issues) {
        const QColor color = i.severity == Issue::Severity::Error ? c.bad : i.severity == Issue::Severity::Warning ? c.warn : c.accent;
        html += QStringLiteral("<p style='margin-bottom:10px'><span style='color:%1'>&#9679;</span> <b>%2</b> &nbsp;<span style='color:%3'>%4 · %5</span><br>"
                               "<span style='color:%3'>%6</span>")
                    .arg(color.name(), i.title.toHtmlEscaped(), c.textMuted.name(), diag::severityName(i.severity), i.subject.toHtmlEscaped(),
                         i.detail.toHtmlEscaped());
        for (const QString& a : i.actions)
            html += QStringLiteral("<br>→ %1").arg(a.toHtmlEscaped());
        html += QStringLiteral("</p>");
    }
    return html;
}

} // namespace

DiagnosticsPage::DiagnosticsPage(AppContext& context, QWidget* parent) : QWidget(parent), m_context(context)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    outer->setSpacing(10);

    auto* top = new QHBoxLayout;
    m_summary = new QLabel;
    m_summary->setWordWrap(true);
    auto* refresh = new QPushButton(tr("Refresh"));
    auto* exportButton = new QPushButton(tr("Export report…"));
    exportButton->setToolTip(tr("Save the report as Markdown and JSON. Your user name, computer name, running programs and application paths are left out."));
    auto* copy = new QPushButton(tr("Copy report"));
    auto* folder = new QPushButton(tr("Open reports folder"));
    top->addWidget(m_summary, 1);
    top->addWidget(refresh);
    top->addWidget(exportButton);
    top->addWidget(copy);
    top->addWidget(folder);
    outer->addLayout(top);
    m_status = muted(QString());
    outer->addWidget(m_status);

    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* content = new QWidget;
    m_sections = new QVBoxLayout(content);
    m_sections->setContentsMargins(0, 0, 8, 0);
    m_sections->setSpacing(14);
    scroll->setWidget(content);
    outer->addWidget(scroll, 1);

    connect(refresh, &QPushButton::clicked, this, &DiagnosticsPage::refresh);
    connect(exportButton, &QPushButton::clicked, this, &DiagnosticsPage::exportReport);
    connect(copy, &QPushButton::clicked, this, &DiagnosticsPage::copyReport);
    connect(folder, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(m_context.data().reportsDir()));
    });
    connect(&m_context.hardware(), &hw::HardwareService::detectionFinished, this, &DiagnosticsPage::refreshIfVisible);
    connect(&m_context.monitor(), &mon::MonitorService::stateChanged, this, &DiagnosticsPage::refreshIfVisible);
    connect(&m_context.preferences(), &pref::PreferenceService::changed, this, &DiagnosticsPage::refreshIfVisible);
    connect(&m_context, &AppContext::settingsChanged, this, &DiagnosticsPage::refreshIfVisible);
}

void DiagnosticsPage::showEvent(QShowEvent* event)
{
    QWidget::showEvent(event);
    if (m_dirty)
        refresh();
}

void DiagnosticsPage::refreshIfVisible()
{
    m_dirty = true;
    if (isVisible())
        refresh();
}

void DiagnosticsPage::refresh()
{
    m_dirty = false;
    const diag::Report report = diag::buildReport(collectReportInputs(m_context));
    const diag::ReportInputs& in = report.inputs;
    const ThemeColors& c = colors();

    const QColor overall = report.count(Issue::Severity::Error) ? c.bad : report.count(Issue::Severity::Warning) ? c.warn : c.good;
    m_summary->setText(tr("<span style='color:%1'>&#9679;</span> <b>%2 error(s), %3 warning(s), %4 note(s)</b> &nbsp;<span style='color:%5'>as of %6</span>")
                           .arg(overall.name()).arg(report.count(Issue::Severity::Error)).arg(report.count(Issue::Severity::Warning))
                           .arg(report.count(Issue::Severity::Note)).arg(c.textMuted.name(), QLocale().toString(in.generated.time(), QLocale::ShortFormat)));

    while (QLayoutItem* item = m_sections->takeAt(0)) {
        delete item->widget();
        delete item;
    }

    auto* issues = new Card(tr("Issues and suggestions"));
    auto* issueLabel = new QLabel(issueHtml(report.issues));
    issueLabel->setWordWrap(true);
    issueLabel->setTextFormat(Qt::RichText);
    issueLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    issues->body()->addWidget(issueLabel);
    m_sections->addWidget(issues);

    const diag::Table adapters = report.adapterTable();
    if (!adapters.rows.isEmpty()) {
        auto* card = new Card(tr("Graphics adapters and APIs"));
        card->body()->addWidget(tableFrom(adapters, 170));
        card->body()->addWidget(muted(tr("Details for each adapter, including plain-language explanations of failures, are on the GPUs page.")));
        m_sections->addWidget(card);
    }

    auto* monitoring = new Card(tr("Monitoring"));
    QStringList providerLines;
    for (const mon::ProviderStatus& p : in.providers)
        providerLines << QStringLiteral("<b>%1</b> — %2. %3%4").arg(p.name.toHtmlEscaped(), p.active ? tr("active") : tr("inactive"), p.detail.toHtmlEscaped(),
            p.unsupported.isEmpty() ? QString() : tr(" Not supported: %1.").arg(p.unsupported.join(QStringLiteral(", ")).toHtmlEscaped()));
    providerLines << tr("<b>Windows GPU performance counters</b> — used for per-application data on WDDM 2.0+ GPUs only.");
    auto* providers = new QLabel(providerLines.join(QStringLiteral("<br>")));
    providers->setWordWrap(true);
    providers->setTextFormat(Qt::RichText);
    monitoring->body()->addWidget(providers);
    const diag::Table metrics = report.metricTable();
    if (!metrics.rows.isEmpty()) {
        monitoring->body()->addWidget(tableFrom(metrics, 200));
        monitoring->body()->addWidget(muted(tr("\"Available\" means the latest sample had a real reading. Unsupported metrics are never shown as zero. "
                                               "Last collection took %1 ms.").arg(in.snapshot->collectionMs, 0, 'f', 2)));
    } else {
        monitoring->body()->addWidget(muted(in.monitoringPaused ? tr("Monitoring is paused, so there are no current readings.") : tr("No readings yet.")));
    }
    m_sections->addWidget(monitoring);

    if (!report.multiGpu.isEmpty()) {
        auto* card = new Card(tr("Multi-GPU compatibility"));
        card->body()->addWidget(muted(tr("Which ways of sharing work between the GPUs can run here, judged from the detected capabilities. "
                                         "\"Feasible\" does not mean faster: that is measured in the Workload Lab (Phase 8).")));
        card->body()->addWidget(tableFrom(report.multiGpuTable(), 300));
        m_sections->addWidget(card);
    }

    auto* preference = new Card(tr("GPU preference"));
    if (in.preferenceVerification) {
        preference->body()->addWidget(muted(tr("Verified %1 with GPUFusionProbe.exe; original value restored: %2. Applications with a preference: %3.")
                                                .arg(QLocale().toString(in.preferenceVerification->time, QLocale::ShortFormat),
                                                     in.preferenceVerification->restored ? tr("yes") : tr("no")).arg(in.configuredPreferences)));
        preference->body()->addWidget(tableFrom(report.preferenceTable(), 260));
    } else {
        preference->body()->addWidget(muted(tr("Not verified yet. Run the verification on the GPU Preferences page.")));
    }
    m_sections->addWidget(preference);

    auto* app = new Card(tr("Application and storage"));
    auto* grid = new InfoGrid;
    grid->addRow(tr("Version"), QStringLiteral("%1 (%2)").arg(in.appVersion, in.buildDescription));
    grid->addRow(tr("Qt"), in.qtVersion);
    grid->addRow(tr("Operating system"), in.osDescription);
    grid->addRow(tr("Data folder"), QStringLiteral("%1 (%2)").arg(QDir::toNativeSeparators(in.dataRoot), describe(m_context.data().source)));
    grid->addRow(tr("Log file"), QDir::toNativeSeparators(log::activeLogFile()));
    grid->addRow(tr("Settings file"), QStringLiteral("%1 (%2)").arg(QDir::toNativeSeparators(m_context.store().filePath()), describe(m_context.loadResult().status)));
    grid->addRow(tr("OpenCL"), in.openCl.loaderPresent ? tr("Loader installed; drivers: %1").arg(in.openCl.icds.join(QStringLiteral(", ")))
                                                       : tr("Loader not installed; drivers registered: %1").arg(in.openCl.icds.isEmpty() ? tr("none") : in.openCl.icds.join(QStringLiteral(", "))));
    app->body()->addWidget(grid);
    auto* buttons = new QHBoxLayout;
    auto* openData = new QPushButton(tr("Open data folder"));
    auto* openLog = new QPushButton(tr("Open log file"));
    connect(openData, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(m_context.data().root)); });
    connect(openLog, &QPushButton::clicked, this, [] { QDesktopServices::openUrl(QUrl::fromLocalFile(log::activeLogFile())); });
    buttons->addWidget(openData);
    buttons->addWidget(openLog);
    buttons->addStretch();
    app->body()->addLayout(buttons);
    m_sections->addWidget(app);

    auto* logCard = new Card(tr("Recent warnings and errors in the log"));
    if (in.recentProblems.isEmpty()) {
        logCard->body()->addWidget(muted(tr("None in the recent part of the log.")));
    } else {
        QStringList lines;
        for (const diag::LogLine& l : in.recentProblems)
            lines << QStringLiteral("%1  %2  [%3]  %4").arg(l.time.toString(QStringLiteral("MM-dd HH:mm:ss")), l.severity, l.subsystem, l.message);
        auto* text = muted(lines.join(u'\n'));
        QFont mono(QStringLiteral("Consolas"));
        mono.setPointSizeF(9);
        text->setFont(mono);
        logCard->body()->addWidget(text);
    }
    m_sections->addWidget(logCard);
    m_sections->addStretch();
}

void DiagnosticsPage::exportReport()
{
    const QString suggested = QDir(m_context.data().reportsDir())
                                  .filePath(QStringLiteral("gpufusion-diagnostics-%1.md").arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyyMMdd-HHmmss"))));
    QString path = QFileDialog::getSaveFileName(this, tr("Export diagnostic report"), suggested, tr("Markdown report (*.md)"));
    if (path.isEmpty())
        return;
    if (path.endsWith(QLatin1StringView(".md"), Qt::CaseInsensitive))
        path.chop(3);
    QStringList written;
    const QString error = ui::exportReport(diag::buildReport(collectReportInputs(m_context)), path, &written);
    m_status->setText(error.isEmpty() ? tr("Saved %1").arg(written.join(QStringLiteral(" and "))) : error);
}

void DiagnosticsPage::copyReport()
{
    const diag::Report report = diag::buildReport(collectReportInputs(m_context));
    QApplication::clipboard()->setText(diag::currentRedactor().apply(report.toMarkdown()));
    m_status->setText(tr("Report copied to the clipboard as Markdown (personal identifiers removed)."));
}

} // namespace gf::ui
