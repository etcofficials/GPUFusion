#include "ui/pages/WorkloadLabPage.h"

#include "benchmarks/BenchmarkService.h"
#include "benchmarks/LabModel.h"
#include "benchmarks/LabRunner.h"
#include "core/Logging.h"
#include "hardware/HardwareService.h"
#include "monitoring/MonitorService.h"
#include "ui/AppContext.h"
#include "ui/Theme.h"
#include "ui/widgets/BarChart.h"
#include "ui/widgets/Card.h"

#include <QComboBox>
#include <QDesktopServices>
#include <QDir>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonArray>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

namespace gf::ui {

using bench::BenchmarkResult;
using bench::Difference;

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
    return QString::number(v, 'f', v < 10 ? 2 : 1);
}

bool usable(const BenchmarkResult& r)
{
    return r.status == bench::Status::Completed && r.duration.n > 0 && r.correct.value_or(false);
}

bool isDecode(const bench::Session& s)
{
    return std::any_of(s.results.begin(), s.results.end(), [](const BenchmarkResult& r) { return r.testId.startsWith(QLatin1StringView("lab.decode")); });
}

QString tooltipFor(const BenchmarkResult& r)
{
    QStringList lines{ QStringLiteral("<b>%1</b>").arg(r.name.toHtmlEscaped()) };
    if (!r.measures.isEmpty())
        lines << QObject::tr("Measures: %1").arg(r.measures.toHtmlEscaped());
    if (!r.api.isEmpty())
        lines << QObject::tr("How: %1").arg(r.api.toHtmlEscaped());
    lines << QObject::tr("Runs: %1 measured after %2 warm-up run(s)").arg(r.duration.n).arg(r.warmupRuns);
    lines << QObject::tr("Correctness: %1").arg(r.correctness.toHtmlEscaped());
    for (const QString& n : r.notes)
        lines << QStringLiteral("• %1").arg(n.toHtmlEscaped());
    for (const QString& w : r.warnings)
        lines << QStringLiteral("⚠ %1").arg(w.toHtmlEscaped());
    if (!r.error.isEmpty())
        lines << QObject::tr("Error: %1").arg(r.error.toHtmlEscaped());
    return lines.join(QStringLiteral("<br>"));
}

} // namespace

WorkloadLabPage::WorkloadLabPage(AppContext& context, QWidget* parent) : QWidget(parent), m_context(context)
{
    m_split.kind = QStringLiteral("split");
    m_decode.kind = QStringLiteral("decode");

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

    auto* intro = new QHBoxLayout;
    intro->addWidget(muted(tr("Experiments with both GPUs working at the same time on GPU Fusion's own test workloads. Nothing here changes "
                              "Windows settings or how other programs use the GPUs. Every mode is measured like a benchmark (warm-up of at least "
                              "one second, then repeated runs), its output is checked, and it is compared with the best single device using the "
                              "same significance rule as the Benchmarks page.")), 1);
    auto* folder = new QPushButton(tr("Open results folder"));
    intro->addWidget(folder, 0, Qt::AlignTop);
    layout->addLayout(intro);

    m_size = new QComboBox;
    m_size->addItem(tr("Quick — 512 × 512"), QStringLiteral("quick"));
    m_size->addItem(tr("Standard — 1024 × 1024"), QStringLiteral("standard"));
    m_size->addItem(tr("Heavy — 2048 × 2048"), QStringLiteral("heavy"));
    m_size->setCurrentIndex(1);

    layout->addWidget(buildPanel(m_split, tr("Split compute across GPUs"),
        tr("One image of the hash workload (512 integer hash steps per pixel) is divided between the GPUs — and in one mode the CPU — "
           "which compute their rows at the same time. Each GPU's rows are copied back to CPU memory and assembled; the result must equal "
           "the CPU reference bit for bit. Modes: each worker alone, an even split, a split in proportion to each GPU's measured speed, "
           "and a shared queue of 32-row items."), m_size));
    layout->addWidget(buildPanel(m_decode, tr("Parallel video decoding"),
        tr("A 10-second 1280 × 720 H.264 test video (created once with Windows' software encoder, about 5 MB in the data folder) is "
           "decoded as fast as possible: one stream on each decoder, two streams on the same GPU, and one stream on each GPU at once. "
           "Every decoder's frames are first compared with the software decoder's — H.264 decoding is exact, so they must be identical."), nullptr));
    layout->addStretch(1);

    connect(folder, &QPushButton::clicked, this, [this] {
        const QString dir = bench::labDir(m_context.data().benchmarksDir());
        QDir().mkpath(dir);
        QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
    });
    connect(&m_context.hardware(), &hw::HardwareService::detectionStarted, this, &WorkloadLabPage::updateControls);
    connect(&m_context.hardware(), &hw::HardwareService::detectionFinished, this, &WorkloadLabPage::updateControls);
    if (bench::BenchmarkService* lab = m_context.lab()) {
        connect(lab, &bench::BenchmarkService::progress, this, [this](int done, int total, const QString& step) {
            if (!m_running)
                return;
            m_running->progress->setMaximum(std::max(1, total));
            m_running->progress->setValue(done);
            m_running->status->setText(step);
        });
        connect(lab, &bench::BenchmarkService::finished, this, &WorkloadLabPage::onFinished);
    }
    showLatest();
    updateControls();
}

QWidget* WorkloadLabPage::buildPanel(Panel& panel, const QString& title, const QString& description, QComboBox* size)
{
    auto* card = new Card(title);
    card->addBadge(tr("Experiment"), Card::Tone::Accent);
    card->body()->addWidget(muted(description));

    auto* controls = new QHBoxLayout;
    if (size) {
        controls->addWidget(new QLabel(tr("Workload size")));
        controls->addWidget(size);
    }
    panel.run = new QPushButton(tr("Run experiment"));
    panel.run->setDefault(true);
    panel.cancel = new QPushButton(tr("Cancel"));
    panel.cancel->setToolTip(tr("Stops after the run in progress."));
    controls->addWidget(panel.run);
    controls->addWidget(panel.cancel);
    controls->addStretch(1);
    card->body()->addLayout(controls);
    panel.progress = new QProgressBar;
    panel.progress->setTextVisible(false);
    panel.progress->setFixedHeight(6);
    panel.progress->hide();
    card->body()->addWidget(panel.progress);
    panel.status = muted(QString());
    card->body()->addWidget(panel.status);

    panel.headline = new QLabel;
    panel.headline->setWordWrap(true);
    panel.headline->setTextFormat(Qt::RichText);
    panel.headline->setTextInteractionFlags(Qt::TextSelectableByMouse);
    card->body()->addWidget(panel.headline);
    panel.when = muted(QString());
    card->body()->addWidget(panel.when);
    panel.chart = new BarChart;
    card->body()->addWidget(panel.chart);
    panel.chartCaption = muted(QString());
    card->body()->addWidget(panel.chartCaption);

    panel.table = new QTableWidget(0, 6);
    panel.table->setHorizontalHeaderLabels({ tr("Mode"), tr("Mean (ms)"), tr("± SD"), tr("vs best single device"), tr("Details"), tr("Output") });
    panel.table->verticalHeader()->hide();
    panel.table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    panel.table->setSelectionMode(QAbstractItemView::NoSelection);
    panel.table->setFocusPolicy(Qt::NoFocus);
    panel.table->setShowGrid(false);
    panel.table->setAlternatingRowColors(true);
    panel.table->setWordWrap(true);
    panel.table->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    QHeaderView* h = panel.table->horizontalHeader();
    h->setSectionResizeMode(QHeaderView::ResizeToContents);
    h->setSectionResizeMode(0, QHeaderView::Fixed);
    panel.table->setColumnWidth(0, 250);
    h->setSectionResizeMode(4, QHeaderView::Stretch);
    card->body()->addWidget(panel.table);
    panel.findings = new QLabel;
    panel.findings->setWordWrap(true);
    panel.findings->setTextFormat(Qt::RichText);
    panel.findings->setTextInteractionFlags(Qt::TextSelectableByMouse);
    card->body()->addWidget(panel.findings);

    Panel* p = &panel;
    connect(panel.run, &QPushButton::clicked, this, [this, p] { start(*p); });
    connect(panel.cancel, &QPushButton::clicked, this, [this, p] {
        if (bench::BenchmarkService* lab = m_context.lab()) {
            lab->cancel();
            p->cancel->setEnabled(false);
            p->status->setText(tr("Cancelling after the current run…"));
        }
    });
    return card;
}

void WorkloadLabPage::updateControls()
{
    bench::BenchmarkService* lab = m_context.lab();
    const bool running = lab && lab->isRunning();
    const bool ready = lab && m_context.hardware().latest().has_value() && !m_context.hardware().isRunning();
    for (Panel* p : { &m_split, &m_decode }) {
        p->run->setEnabled(!running && ready);
        p->cancel->setEnabled(running && m_running == p);
        if (!lab)
            p->status->setText(tr("The Workload Lab is not available in this session."));
    }
    m_size->setEnabled(!running);
}

void WorkloadLabPage::start(Panel& panel)
{
    bench::BenchmarkService* lab = m_context.lab();
    const auto& detection = m_context.hardware().latest();
    if (!lab || lab->isRunning() || !detection)
        return;
    const Settings& st = m_context.settings();
    const QString preset = &panel == &m_split ? m_size->currentData().toString() : QStringLiteral("standard");
    const bench::LabOptions options = bench::labOptions(st.benchmarkWarmupRuns, st.benchmarkMeasuredRuns, preset, lab->benchmarksDir());
    const hw::DetectionResult d = *detection;
    const bool split = &panel == &m_split;

    m_resumeMonitoring = !m_context.monitor().isPaused();
    if (m_resumeMonitoring)
        m_context.monitor().setPaused(true); // our own sampling would add load to what is measured
    qCInfo(lcBench).noquote() << "Workload Lab:" << panel.kind << "experiment started, size" << preset;
    m_running = &panel;
    panel.progress->setValue(0);
    panel.progress->show();
    panel.status->setText(tr("Starting…"));
    lab->startJob([split, options, d](const bench::Progress& progress, const std::atomic<bool>* cancel) {
        return split ? bench::runSplitExperiment(d, options, progress, cancel) : bench::runDecodeExperiment(d, options, progress, cancel);
    }, st.benchmarkRetentionDays);
    updateControls();
}

void WorkloadLabPage::onFinished(const bench::Session& session)
{
    if (m_resumeMonitoring)
        m_context.monitor().setPaused(false);
    m_resumeMonitoring = false;
    Panel* panel = m_running ? m_running : (isDecode(session) ? &m_decode : &m_split);
    m_running = nullptr;
    panel->progress->hide();
    int failed = 0, wrong = 0, cancelled = 0;
    for (const BenchmarkResult& r : session.results) {
        failed += r.status == bench::Status::Failed;
        cancelled += r.status == bench::Status::Cancelled;
        wrong += r.correct.has_value() && !*r.correct;
    }
    QString status = tr("Finished.");
    if (failed || wrong || cancelled)
        status = tr("Finished: %1 mode(s) failed, %2 with wrong output, %3 cancelled.").arg(failed).arg(wrong).arg(cancelled);
    if (const QString e = m_context.lab()->lastSaveError(); !e.isEmpty())
        status += u' ' + tr("The results could not be saved: %1").arg(e);
    panel->status->setText(status);
    showLatest();
    updateControls();
}

void WorkloadLabPage::showLatest()
{
    const bench::Session* split = nullptr;
    const bench::Session* decode = nullptr;
    if (bench::BenchmarkService* lab = m_context.lab()) {
        for (const bench::Session& s : lab->history()) { // newest first
            if (isDecode(s)) {
                if (!decode)
                    decode = &s;
            } else if (!split) {
                split = &s;
            }
        }
    }
    showPanel(m_split, split);
    showPanel(m_decode, decode);
}

void WorkloadLabPage::showPanel(Panel& panel, const bench::Session* session)
{
    const ThemeColors& c = colors();
    const bool hasSession = session != nullptr;
    for (QWidget* w : std::initializer_list<QWidget*>{ panel.chart, panel.chartCaption, panel.table, panel.findings, panel.when })
        w->setVisible(hasSession);
    if (!session) {
        panel.headline->setText(tr("Not run yet. The experiment takes about %1 and keeps both GPUs fully busy meanwhile.")
                                    .arg(panel.kind == QLatin1StringView("split") ? tr("half a minute") : tr("one to two minutes")));
        return;
    }
    const bool decode = panel.kind == QLatin1StringView("decode");
    const bench::LabAnalysis a = decode ? bench::analyzeDecode(*session) : bench::analyzeSplit(*session);

    QColor dot = c.textMuted;
    if (a.verdict == Difference::Verdict::Faster)
        dot = c.good;
    else if (a.verdict == Difference::Verdict::Slower)
        dot = c.warn;
    panel.headline->setText(QStringLiteral("<span style='color:%1'>&#9679;</span> <b>%2</b>").arg(dot.name(), a.headline.toHtmlEscaped()));

    QString size;
    for (const BenchmarkResult& r : session->results) {
        if (!decode && r.params.contains(QLatin1StringView("width"))) {
            size = tr("%1 × %2 image, %3 iterations").arg(r.params.value(QLatin1StringView("width")).toInteger())
                       .arg(r.params.value(QLatin1StringView("height")).toInteger()).arg(r.params.value(QLatin1StringView("iterations")).toInteger());
            break;
        }
        if (decode && r.params.contains(QLatin1StringView("frames"))) {
            size = tr("%1 frames of %2 × %3 H.264 per stream").arg(r.params.value(QLatin1StringView("frames")).toInteger())
                       .arg(r.params.value(QLatin1StringView("width")).toInteger()).arg(r.params.value(QLatin1StringView("height")).toInteger());
            break;
        }
    }
    panel.when->setText(tr("Measured %1 · %2 · hover over a row for the busy GPU engines and performance states")
                            .arg(QLocale().toString(session->started, QLocale::ShortFormat), size));

    // Chart: split → time per mode with markers; decode → throughput (comparable across 1 and 2 streams).
    QList<BarChart::Bar> bars;
    QList<BarChart::Marker> markers;
    for (const bench::LabRow& row : a.rows) {
        const BenchmarkResult& r = *row.result;
        // The CPU on its own is an order of magnitude slower; drawn, it would squash the comparisons
        // that matter. It stays in the table.
        if (!decode && r.testId == QLatin1StringView("lab.split.single") && r.target.vendorId == 0)
            continue;
        BarChart::Bar bar;
        bar.label = r.name;
        bar.emphasized = row.reference || row.vsReference.has_value();
        if (usable(r)) {
            if (decode) {
                bar.value = r.metrics.value(QStringLiteral("fps")).mean;
                bar.error = r.metrics.value(QStringLiteral("fps")).stddev;
            } else {
                bar.value = r.duration.mean;
                bar.error = r.duration.stddev;
            }
        } else {
            bar.note = r.status != bench::Status::Completed ? bench::statusName(r.status) : tr("Wrong output — not shown");
        }
        bars << bar;
    }
    if (!decode && a.reference) {
        markers << BarChart::Marker{ a.reference->duration.mean, a.reference->name, false };
        for (const bench::LabRow& row : a.rows) {
            if (row.idealMs && row.result->testId == QLatin1StringView("lab.split.balanced")) {
                markers << BarChart::Marker{ *row.idealMs, tr("perfect split of both GPUs, no overhead"), true };
                break;
            }
        }
    }
    panel.chart->setData(bars, markers, decode ? tr("frames/s") : tr("ms"));
    panel.chartCaption->setText(decode ? tr("Decoded frames per second, all streams together (longer is better). Whiskers: ± one standard deviation.")
                                       : tr("Time to finish the whole image (shorter is better). Whiskers: ± one standard deviation. "
                                            "Solid line: the best single GPU; dashed line: the theoretical best for both GPUs together. The CPU-only time is in the table."));

    // Table.
    QTableWidget* t = panel.table;
    t->setRowCount(int(a.rows.size()));
    for (int i = 0; i < a.rows.size(); ++i) {
        const bench::LabRow& row = a.rows[i];
        const BenchmarkResult& r = *row.result;
        const QString tip = tooltipFor(r);
        auto set = [&](int col, const QString& text, const QColor& color = {}) {
            auto* item = new QTableWidgetItem(text);
            item->setToolTip(tip);
            if (color.isValid())
                item->setForeground(color);
            if (col == 1 || col == 2)
                item->setTextAlignment(Qt::AlignRight | Qt::AlignVCenter);
            t->setItem(i, col, item);
        };
        set(0, r.name);
        if (r.status == bench::Status::Completed && r.duration.n > 0) {
            set(1, num(r.duration.mean));
            set(2, num(r.duration.stddev), r.duration.cv() > 0.10 ? c.warn : QColor());
        } else {
            set(1, bench::statusName(r.status), r.status == bench::Status::Failed ? c.bad : c.textMuted);
            set(2, QString());
        }
        if (row.reference) {
            set(3, tr("reference"), c.textMuted);
        } else if (row.vsReference) {
            const QColor color = row.vsReference->verdict == Difference::Verdict::Faster ? c.good
                               : row.vsReference->verdict == Difference::Verdict::Slower ? c.warn
                                                                                         : c.textMuted;
            set(3, bench::describeDifference(*row.vsReference), color);
        } else {
            set(3, QStringLiteral("—"), c.textMuted);
        }
        QString detail = row.detail;
        if (row.idealMs)
            detail += (detail.isEmpty() ? QString() : QStringLiteral(" · ")) + tr("ideal %1 ms").arg(num(*row.idealMs));
        set(4, detail);
        if (!r.correct.has_value())
            set(5, r.status == bench::Status::Completed ? tr("Not checked") : QString(), c.textMuted);
        else
            set(5, *r.correct ? tr("Correct") : tr("Wrong"), *r.correct ? c.good : c.bad);
    }
    const int rowHeight = 2 * fontMetrics().lineSpacing() + 10;
    for (int i = 0; i < t->rowCount(); ++i)
        t->setRowHeight(i, rowHeight);
    t->setFixedHeight(t->horizontalHeader()->sizeHint().height() + rowHeight * t->rowCount() + 4);

    QStringList findings;
    for (const QString& f : a.findings)
        findings << QStringLiteral("• %1").arg(f.toHtmlEscaped());
    panel.findings->setText(findings.join(QStringLiteral("<br>")));
    panel.findings->setVisible(!findings.isEmpty());
}

} // namespace gf::ui
