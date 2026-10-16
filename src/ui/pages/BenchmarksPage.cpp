#include "ui/pages/BenchmarksPage.h"

#include "benchmarks/BenchmarkService.h"
#include "core/Logging.h"
#include "hardware/HardwareService.h"
#include "monitoring/MonitorService.h"
#include "preferences/PreferenceService.h"
#include "ui/AppContext.h"
#include "ui/Theme.h"
#include "ui/widgets/Card.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUrl>
#include <QVBoxLayout>

namespace gf::ui {

using bench::BenchmarkResult;
using bench::Status;

namespace {

QLabel* muted(const QString& text)
{
    auto* l = new QLabel(text);
    l->setObjectName(QStringLiteral("muted"));
    l->setWordWrap(true);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return l;
}

QString num(double v)
{
    return QString::number(v, 'f', v < 1 ? 3 : v < 100 ? 2 : 1);
}

QString metricName(const QString& key)
{
    static const QHash<QString, QString> names{
        { QStringLiteral("gpuTimestampMs"), QStringLiteral("GPU time (timestamp queries)") },
        { QStringLiteral("readbackMs"), QStringLiteral("Read-back of the result") },
        { QStringLiteral("gigaStepsPerSecond"), QStringLiteral("Hash steps per second (billions)") },
        { QStringLiteral("uploadMBps"), QStringLiteral("Upload rate (MB/s)") },
        { QStringLiteral("readbackMBps"), QStringLiteral("Read-back rate (MB/s)") },
        { QStringLiteral("adapters"), QStringLiteral("Adapters found") },
        { QStringLiteral("processes"), QStringLiteral("Processes listed") },
    };
    return names.value(key, key);
}

// Everything about one result that does not fit the table, for its tooltip.
QString detailsHtml(const BenchmarkResult& r)
{
    QStringList lines;
    lines << QStringLiteral("<b>%1</b>").arg(r.name.toHtmlEscaped());
    if (!r.measures.isEmpty())
        lines << QObject::tr("Measures: %1").arg(r.measures.toHtmlEscaped());
    if (!r.api.isEmpty())
        lines << QObject::tr("Runs on: %1").arg(r.api.toHtmlEscaped());
    lines << QObject::tr("Runs: %1 measured, after %2 warm-up run(s) that are not counted").arg(r.duration.n).arg(r.warmupRuns);
    QStringList params;
    for (auto it = r.params.begin(); it != r.params.end(); ++it)
        params << QStringLiteral("%1 %2").arg(it.key(), it.value().toVariant().toString());
    if (!params.isEmpty())
        lines << QObject::tr("Parameters: %1").arg(params.join(QStringLiteral(", ")).toHtmlEscaped());
    for (auto it = r.metrics.begin(); it != r.metrics.end(); ++it)
        lines << QStringLiteral("%1: %2 (± %3)").arg(metricName(it.key())).arg(it.value().mean, 0, 'g', 4).arg(it.value().stddev, 0, 'g', 2);
    lines << QObject::tr("GPU busy during the runs: %1").arg(mon::formatMetric(r.gpuBusyPercent, mon::Unit::Percent));
    lines << QObject::tr("GPU Fusion CPU use during the runs: %1").arg(mon::formatMetric(r.cpuPercent, mon::Unit::Percent));
    if (r.gpuDedicatedMB.isOk())
        lines << QObject::tr("Dedicated GPU memory in use afterwards: %1 MB").arg(r.gpuDedicatedMB.value, 0, 'f', 0);
    if (r.peakWorkingSetMB.isOk())
        lines << QObject::tr("GPU Fusion peak memory so far: %1 MB").arg(r.peakWorkingSetMB.value, 0, 'f', 0);
    lines << QObject::tr("Correctness check: %1").arg(r.correctness.toHtmlEscaped());
    for (const QString& w : r.warnings)
        lines << QStringLiteral("⚠ %1").arg(w.toHtmlEscaped());
    if (!r.error.isEmpty())
        lines << QObject::tr("Error: %1").arg(r.error.toHtmlEscaped());
    return lines.join(QStringLiteral("<br>"));
}

} // namespace

BenchmarksPage::BenchmarksPage(AppContext& context, QWidget* parent) : QWidget(parent), m_context(context)
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    layout->setContentsMargins(0, 0, 8, 0);
    layout->setSpacing(14);
    scroll->setWidget(content);
    outer->addWidget(scroll);

    // ---- run ----
    auto* runCard = new Card(tr("Run benchmarks"));
    auto* row = new QHBoxLayout;
    row->setSpacing(16);
    m_tests = new QTreeWidget;
    m_tests->setHeaderHidden(true);
    m_tests->setRootIsDecorated(false);
    m_tests->setItemsExpandable(false);
    m_tests->setSelectionMode(QAbstractItemView::NoSelection);
    m_tests->setFocusPolicy(Qt::NoFocus);
    m_tests->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    m_tests->setAccessibleName(tr("Benchmarks to run"));
    row->addWidget(m_tests, 1);

    auto* side = new QVBoxLayout;
    side->setSpacing(8);
    side->addWidget(new QLabel(tr("Workload size")));
    m_preset = new QComboBox;
    m_preset->addItem(tr("Quick — 512 × 512, 256 iterations"), QStringLiteral("quick"));
    m_preset->addItem(tr("Standard — 1024 × 1024, 512 iterations"), QStringLiteral("standard"));
    m_preset->addItem(tr("Heavy — 2048 × 2048, 512 iterations"), QStringLiteral("heavy"));
    m_preset->setCurrentIndex(1);
    m_preset->setToolTip(tr("Size of the hash workload. Results are only ever compared with earlier runs of the same size."));
    side->addWidget(m_preset);
    m_runs = muted(QString());
    side->addWidget(m_runs);
    side->addSpacing(6);
    m_run = new QPushButton(tr("Run selected"));
    m_run->setDefault(true);
    m_cancel = new QPushButton(tr("Cancel"));
    m_cancel->setToolTip(tr("Stops after the run in progress; a measured run is never cut short."));
    side->addWidget(m_run);
    side->addWidget(m_cancel);
    side->addStretch(1);
    auto* sideWidget = new QWidget;
    sideWidget->setLayout(side);
    sideWidget->setFixedWidth(300);
    row->addWidget(sideWidget);
    runCard->body()->addLayout(row);

    m_progress = new QProgressBar;
    m_progress->setTextVisible(false);
    m_progress->setFixedHeight(6);
    m_progress->hide();
    runCard->body()->addWidget(m_progress);
    m_status = muted(QString());
    runCard->body()->addWidget(m_status);
    runCard->body()->addWidget(muted(tr("GPU Fusion pauses its own monitoring while benchmarks run. For repeatable numbers, close games, "
                                        "video and other GPU-heavy programs first. Each benchmark checks its own output where it can; "
                                        "a result whose output is wrong is marked and never used as a baseline.")));
    layout->addWidget(runCard);

    // ---- results ----
    auto* resultsCard = new Card(tr("Results"));
    auto* top = new QHBoxLayout;
    top->addWidget(new QLabel(tr("Session")));
    m_sessions = new QComboBox;
    m_sessions->setMinimumWidth(320);
    m_sessions->setAccessibleName(tr("Benchmark session"));
    top->addWidget(m_sessions);
    top->addStretch(1);
    auto* folder = new QPushButton(tr("Open results folder"));
    folder->setToolTip(tr("Each session is saved as JSON; results.csv has one row per result for spreadsheets."));
    top->addWidget(folder);
    resultsCard->body()->addLayout(top);
    m_sessionInfo = muted(QString());
    resultsCard->body()->addWidget(m_sessionInfo);
    m_summary = new QLabel;
    m_summary->setWordWrap(true);
    m_summary->setTextFormat(Qt::RichText);
    m_summary->setTextInteractionFlags(Qt::TextSelectableByMouse);
    resultsCard->body()->addWidget(m_summary);

    m_table = new QTableWidget(0, 7);
    m_table->setHorizontalHeaderLabels({ tr("Benchmark"), tr("Mean (ms)"), tr("± SD (CV)"), tr("Min – max (ms)"), tr("Runs"), tr("Output"),
                                         tr("vs previous equivalent run") });
    m_table->verticalHeader()->hide();
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionMode(QAbstractItemView::NoSelection);
    m_table->setFocusPolicy(Qt::NoFocus);
    m_table->setShowGrid(false);
    m_table->setAlternatingRowColors(true);
    m_table->setWordWrap(true);
    m_table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QHeaderView* h = m_table->horizontalHeader();
    h->setSectionResizeMode(QHeaderView::ResizeToContents);
    h->setSectionResizeMode(0, QHeaderView::Fixed);
    m_table->setColumnWidth(0, 270);
    h->setSectionResizeMode(6, QHeaderView::Stretch);
    resultsCard->body()->addWidget(m_table);
    m_notes = new QLabel;
    m_notes->setWordWrap(true);
    m_notes->setTextFormat(Qt::RichText);
    m_notes->setTextInteractionFlags(Qt::TextSelectableByMouse);
    resultsCard->body()->addWidget(m_notes);
    resultsCard->body()->addWidget(muted(tr("Mean, variation (sample standard deviation and its share of the mean) and range are over the measured runs; "
                                            "warm-up runs are not counted. A change is called faster or slower only when it is larger than twice the "
                                            "combined standard error and at least 1 %, and only against an earlier run of the same benchmark, workload "
                                            "size, GPU model and driver. Hover over a row for details.")));
    layout->addWidget(resultsCard);
    layout->addStretch(1);

    connect(m_run, &QPushButton::clicked, this, &BenchmarksPage::start);
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        if (bench::BenchmarkService* s = m_context.benchmarks()) {
            s->cancel();
            m_cancel->setEnabled(false);
            m_status->setText(tr("Cancelling after the current run…"));
        }
    });
    connect(m_sessions, &QComboBox::currentIndexChanged, this, &BenchmarksPage::showSession);
    connect(folder, &QPushButton::clicked, this, [this] {
        const QString dir = m_context.data().benchmarksDir();
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    connect(m_tests, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem* item) {
        const QString uid = item->data(0, Qt::UserRole).toString();
        if (uid.isEmpty())
            return;
        if (item->checkState(0) == Qt::Checked)
            m_unchecked.remove(uid);
        else
            m_unchecked.insert(uid);
        updateControls();
    });
    connect(&m_context.hardware(), &hw::HardwareService::detectionStarted, this, &BenchmarksPage::updateControls);
    connect(&m_context.hardware(), &hw::HardwareService::detectionFinished, this, &BenchmarksPage::rebuildChecklist);
    connect(&m_context, &AppContext::settingsChanged, this, &BenchmarksPage::updateControls);
    if (bench::BenchmarkService* s = m_context.benchmarks()) {
        connect(s, &bench::BenchmarkService::progress, this, &BenchmarksPage::onProgress);
        connect(s, &bench::BenchmarkService::finished, this, &BenchmarksPage::onFinished);
    }

    m_tests->setUniformRowHeights(true);
    rebuildChecklist();
    reloadSessions({});
}

void BenchmarksPage::rebuildChecklist()
{
    const auto& detection = m_context.hardware().latest();
    m_specs = bench::availableBenchmarks(detection ? *detection : hw::DetectionResult{},
                                         m_context.preferences().probeAvailable() ? m_context.preferences().probePath() : QString());
    QSignalBlocker block(m_tests);
    m_tests->clear();
    QHash<QString, QTreeWidgetItem*> categories;
    for (const bench::BenchmarkSpec& s : m_specs) {
        QTreeWidgetItem*& parent = categories[s.category];
        if (!parent) {
            parent = new QTreeWidgetItem(m_tests, { s.category });
            parent->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
            QFont f = parent->font(0);
            f.setBold(true);
            parent->setFont(0, f);
        }
        auto* item = new QTreeWidgetItem(parent, { s.name });
        item->setData(0, Qt::UserRole, s.uid());
        item->setToolTip(0, s.description);
        item->setFlags(Qt::ItemIsEnabled | Qt::ItemIsUserCheckable);
        item->setCheckState(0, m_unchecked.contains(s.uid()) ? Qt::Unchecked : Qt::Checked);
    }
    m_tests->expandAll();
    // Show every test without an inner scroll bar.
    int rows = 0;
    for (QTreeWidgetItemIterator it(m_tests); *it; ++it)
        ++rows;
    const int rowHeight = m_tests->topLevelItemCount() ? m_tests->sizeHintForRow(0) : 20;
    m_tests->setFixedHeight(rows * std::max(rowHeight, 18) + 2 * m_tests->frameWidth() + 6);
    updateControls();
}

QList<bench::BenchmarkSpec> BenchmarksPage::selectedSpecs() const
{
    QList<bench::BenchmarkSpec> out;
    for (const bench::BenchmarkSpec& s : m_specs) {
        if (!m_unchecked.contains(s.uid()))
            out << s;
    }
    return out;
}

void BenchmarksPage::updateControls()
{
    bench::BenchmarkService* service = m_context.benchmarks();
    const bool running = service && service->isRunning();
    const bool ready = service && m_context.hardware().latest().has_value() && !m_context.hardware().isRunning();
    const int selected = int(selectedSpecs().size());
    m_run->setEnabled(!running && ready && selected > 0);
    m_cancel->setEnabled(running);
    m_preset->setEnabled(!running);
    m_tests->setEnabled(!running);
    const Settings& st = m_context.settings();
    m_runs->setText(tr("%1 warm-up and %2 measured runs per benchmark (change in Settings). %3 selected.")
                        .arg(st.benchmarkWarmupRuns).arg(st.benchmarkMeasuredRuns).arg(selected));
    if (!service)
        m_status->setText(tr("Benchmarking is not available in this session."));
    else if (!running && !ready)
        m_status->setText(tr("Waiting for adapter detection…"));
    else if (!running && m_status->text() == tr("Waiting for adapter detection…"))
        m_status->clear();
}

void BenchmarksPage::start()
{
    bench::BenchmarkService* service = m_context.benchmarks();
    const auto& detection = m_context.hardware().latest();
    const QList<bench::BenchmarkSpec> specs = selectedSpecs();
    if (!service || service->isRunning() || !detection || specs.isEmpty())
        return;
    const Settings& st = m_context.settings();
    bench::RunOptions options;
    options.warmupRuns = st.benchmarkWarmupRuns;
    options.measuredRuns = st.benchmarkMeasuredRuns;
    options.preset = m_preset->currentData().toString();
    options.hash = bench::hashPreset(options.preset);
    options.probePath = m_context.preferences().probePath();

    // Our own sampling would add load to the very thing being measured.
    m_resumeMonitoring = !m_context.monitor().isPaused();
    if (m_resumeMonitoring)
        m_context.monitor().setPaused(true);
    qCInfo(lcBench).noquote() << QStringLiteral("Running %1 benchmark(s), preset %2, %3 warm-up + %4 measured runs")
                                     .arg(specs.size()).arg(options.preset).arg(options.warmupRuns).arg(options.measuredRuns);
    m_progress->setRange(0, int(specs.size()) * (options.warmupRuns + options.measuredRuns));
    m_progress->setValue(0);
    m_progress->show();
    m_status->setText(tr("Starting…"));
    service->start(specs, options, *detection, st.benchmarkRetentionDays);
    updateControls();
}

void BenchmarksPage::onProgress(int done, int total, const QString& step)
{
    m_progress->setMaximum(total);
    m_progress->setValue(done);
    m_status->setText(tr("%1 (%2 of %3 runs)").arg(step).arg(done).arg(total));
}

void BenchmarksPage::onFinished(const bench::Session& session)
{
    if (m_resumeMonitoring)
        m_context.monitor().setPaused(false);
    m_resumeMonitoring = false;
    m_progress->hide();
    int completed = 0, failed = 0, cancelled = 0, wrong = 0;
    for (const BenchmarkResult& r : session.results) {
        completed += r.status == Status::Completed;
        failed += r.status == Status::Failed;
        cancelled += r.status == Status::Cancelled;
        wrong += r.correct.has_value() && !*r.correct;
    }
    QStringList parts{ tr("Finished: %1 completed").arg(completed) };
    if (failed)
        parts << tr("%1 failed").arg(failed);
    if (cancelled)
        parts << tr("%1 cancelled").arg(cancelled);
    if (wrong)
        parts << tr("%1 with wrong output").arg(wrong);
    QString status = parts.join(QStringLiteral(", ")) + u'.';
    if (const QString error = m_context.benchmarks()->lastSaveError(); !error.isEmpty())
        status += u' ' + tr("The results could not be saved: %1").arg(error);
    m_status->setText(status);
    reloadSessions(session.id);
    updateControls();
}

void BenchmarksPage::reloadSessions(const QString& selectId)
{
    bench::BenchmarkService* service = m_context.benchmarks();
    QSignalBlocker block(m_sessions);
    m_sessions->clear();
    if (service) {
        for (const bench::Session& s : service->history())
            m_sessions->addItem(tr("%1 — %2 result(s)").arg(QLocale().toString(s.started, QLocale::ShortFormat)).arg(s.results.size()), s.id);
    }
    const int index = selectId.isEmpty() ? 0 : std::max(0, m_sessions->findData(selectId));
    m_sessions->setCurrentIndex(m_sessions->count() ? index : -1);
    m_sessions->setEnabled(m_sessions->count() > 0);
    showSession(m_sessions->currentIndex());
}

void BenchmarksPage::showSession(int index)
{
    bench::BenchmarkService* service = m_context.benchmarks();
    const ThemeColors& c = colors();
    m_table->setRowCount(0);
    if (!service || index < 0 || index >= service->history().size()) {
        m_sessionInfo->setText(tr("No benchmark sessions yet. Results are saved in %1.").arg(QDir::toNativeSeparators(m_context.data().benchmarksDir())));
        m_summary->clear();
        m_notes->clear();
        m_table->hide();
        return;
    }
    const bench::Session& s = service->history().at(index);
    m_sessionInfo->setText(tr("Started %1 · GPU Fusion %2 · %3 · %4 GB RAM · %5")
                               .arg(QLocale().toString(s.started, QLocale::LongFormat), s.appVersion, s.cpu)
                               .arg(s.ramGB, 0, 'f', 1).arg(s.adapters.join(QStringLiteral(", "))));
    const QStringList summary = bench::sessionSummary(s);
    QStringList escaped;
    for (const QString& line : summary)
        escaped << line.toHtmlEscaped();
    m_summary->setText(escaped.join(QStringLiteral("<br>")));
    m_summary->setVisible(!summary.isEmpty());

    QStringList notes;
    m_table->show();
    m_table->setRowCount(int(s.results.size()));
    for (int row = 0; row < s.results.size(); ++row) {
        const BenchmarkResult& r = s.results[row];
        const QString tip = detailsHtml(r);
        auto set = [&](int col, const QString& text, const QColor& color = {}) {
            auto* item = new QTableWidgetItem(text);
            item->setToolTip(tip);
            if (color.isValid())
                item->setForeground(color);
            if (col >= 1 && col <= 4)
                item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            m_table->setItem(row, col, item);
        };
        set(0, r.name);
        const bool hasData = r.duration.n > 0;
        if (r.status == Status::Completed && hasData) {
            set(1, num(r.duration.mean));
            set(2, QStringLiteral("± %1 (%2 %)").arg(num(r.duration.stddev)).arg(100 * r.duration.cv(), 0, 'f', 1), r.duration.cv() > 0.10 ? c.warn : QColor());
            set(3, QStringLiteral("%1 – %2").arg(num(r.duration.min), num(r.duration.max)));
        } else {
            set(1, bench::statusName(r.status), r.status == Status::Failed ? c.bad : c.textMuted);
            set(2, QString());
            set(3, QString());
        }
        set(4, QString::number(r.duration.n));
        if (!r.correct.has_value())
            set(5, tr("Not checked"), c.textMuted);
        else
            set(5, *r.correct ? tr("Correct") : tr("Wrong"), *r.correct ? c.good : c.bad);
        if (r.status == Status::Completed && hasData) {
            const bench::Comparison cmp = bench::compare(r, bench::findBaseline(r, service->history(), s.id));
            const QColor color = cmp.verdict == bench::Comparison::Verdict::Faster ? c.good
                               : cmp.verdict == bench::Comparison::Verdict::Slower ? c.warn
                                                                                    : c.textMuted;
            set(6, cmp.text, color);
        } else {
            set(6, QString());
        }
        for (const QString& w : r.warnings)
            notes << QStringLiteral("<span style='color:%1'>⚠</span> <b>%2</b>: %3").arg(c.warn.name(), r.name.toHtmlEscaped(), w.toHtmlEscaped());
        if (!r.error.isEmpty())
            notes << QStringLiteral("<span style='color:%1'>●</span> <b>%2</b>: %3").arg(c.bad.name(), r.name.toHtmlEscaped(), r.error.toHtmlEscaped());
    }
    // Two lines per row (long names and comparisons wrap). Measuring contents here would use the
    // column widths from before the first layout, which gave rows many times too tall.
    const int rowHeight = 2 * fontMetrics().lineSpacing() + 10;
    for (int r = 0; r < m_table->rowCount(); ++r)
        m_table->setRowHeight(r, rowHeight);
    const int height = m_table->horizontalHeader()->sizeHint().height() + rowHeight * m_table->rowCount() + 4;
    m_table->setFixedHeight(height);
    m_notes->setText(notes.join(QStringLiteral("<br>")));
    m_notes->setVisible(!notes.isEmpty());
}

} // namespace gf::ui
