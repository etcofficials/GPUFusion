#include "ui/widgets/JobBatchPanel.h"

#include "core/Logging.h"
#include "dispatch/JobDispatcher.h"
#include "hardware/HardwareService.h"
#include "ui/AppContext.h"
#include "ui/Theme.h"

#include <QComboBox>
#include <QDir>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPushButton>
#include <QSpinBox>
#include <QVBoxLayout>

#include <memory>

namespace gf::ui {

namespace {

QLabel* muted(const QString& text)
{
    auto* l = new QLabel(text);
    l->setObjectName(QStringLiteral("muted"));
    l->setWordWrap(true);
    return l;
}

struct BatchOutcome {
    QList<dispatch::Worker> workers;
    std::optional<dispatch::RunResult> run;
    std::optional<dispatch::Experiment> experiment;
    QString error;
};

QString describe(const BatchOutcome& o)
{
    const ThemeColors& c = colors();
    QStringList html;
    if (!o.error.isEmpty())
        return QStringLiteral("<span style='color:%1'>%2</span>").arg(c.bad.name(), o.error.toHtmlEscaped());
    if (o.experiment) {
        const dispatch::Experiment& e = *o.experiment;
        const QColor tone = e.verdict.startsWith(QLatin1StringView("Faster")) ? c.good
                          : e.verdict.startsWith(QLatin1StringView("Not comparable")) ? c.bad : c.warn;
        html << QStringLiteral("<b style='color:%1'>%2</b>").arg(tone.name(), e.verdict.toHtmlEscaped());
        for (const dispatch::ConfigResult& cfg : e.configs)
            html << QStringLiteral("%1: %2").arg(cfg.label.toHtmlEscaped(),
                        cfg.allOk ? QStringLiteral("mean %1 ms over %2 run(s) (sd %3 ms)").arg(cfg.wall.mean, 0, 'f', 0).arg(cfg.wall.n).arg(cfg.wall.stddev, 0, 'f', 0)
                                  : QStringLiteral("<span style='color:%1'>jobs failed</span>").arg(c.bad.name()));
        for (const QString& n : e.notes)
            html << QStringLiteral("<span style='color:%1'>%2</span>").arg(c.textMuted.name(), n.toHtmlEscaped());
        return html.join(QStringLiteral("<br>"));
    }
    if (o.run) {
        const dispatch::RunResult& r = *o.run;
        html << QStringLiteral("<b>%1 job(s) in %2 ms, %3 failed.</b>").arg(r.jobs.size()).arg(r.wallMs, 0, 'f', 0).arg(r.failed());
        const QMap<int, int> per = r.jobsPerWorker();
        for (auto it = per.begin(); it != per.end(); ++it)
            html << QStringLiteral("%1: %2 job(s)").arg(o.workers.value(it.key()).name.toHtmlEscaped()).arg(it.value());
        int shown = 0;
        for (const dispatch::JobResult& j : r.jobs) {
            if (!j.ok && shown++ < 5)
                html << QStringLiteral("<span style='color:%1'>Job %2: %3</span>").arg(c.bad.name()).arg(j.job).arg(j.error.toHtmlEscaped());
        }
        html << QStringLiteral("<span style='color:%1'>%2</span>").arg(c.textMuted.name(),
                    QObject::tr("A single run shows that the jobs ran, not that several GPUs were faster — use “Compare” for that.").toHtmlEscaped());
    }
    return html.join(QStringLiteral("<br>"));
}

} // namespace

JobBatchPanel::JobBatchPanel(AppContext& context, QWidget* parent) : Card(tr("Your own jobs on several GPUs"), parent), m_context(context)
{
    body()->addWidget(muted(tr("Independent jobs — one per input file — spread over the GPUs: each GPU takes the next job as soon as it is "
                               "free. Any program can be used if it lets you choose its GPU on the command line; the placeholders "
                               "{dxgi_index} {luid} {luid_pair} {vendor_id} {vendor_hex} {gpu_name} {input} {output} {stem} {job} are filled "
                               "in per job, "
                               "and GPUFUSION_ADAPTERS is set for programs that use the GPU Fusion SDK. Example (FFmpeg decoding): "
                               "ffmpeg -y -init_hw_device d3d11va=g:{dxgi_index} -hwaccel d3d11va -hwaccel_device g -i {input} -f framemd5 {output}")));
    auto* form = new QFormLayout;
    m_kind = new QComboBox;
    m_kind->addItem(tr("Command template"), QStringLiteral("command"));
    m_kind->addItem(tr("Built-in: H.264 decoding (Media Foundation)"), QStringLiteral("decode"));
    form->addRow(tr("Job:"), m_kind);
    m_command = new QLineEdit;
    m_command->setPlaceholderText(tr("program.exe --gpu {dxgi_index} -i {input} -o {output}"));
    form->addRow(tr("Command:"), m_command);
    auto* inputsRow = new QHBoxLayout;
    m_inputs = new QLineEdit;
    m_inputs->setPlaceholderText(tr("a folder, or files separated by ;"));
    auto* browse = new QPushButton(tr("Folder…"));
    inputsRow->addWidget(m_inputs, 1);
    inputsRow->addWidget(browse);
    form->addRow(tr("Inputs:"), inputsRow);
    m_output = new QLineEdit;
    m_output->setPlaceholderText(QDir::toNativeSeparators(m_context.data().tempDir() + QStringLiteral("/dispatch")));
    form->addRow(tr("Output folder:"), m_output);
    m_pattern = new QLineEdit;
    m_pattern->setPlaceholderText(tr("e.g. {stem}.mkv — empty if the jobs write no file"));
    form->addRow(tr("Output name:"), m_pattern);
    m_gpus = new QComboBox;
    m_gpus->addItem(tr("All GPUs"), QStringLiteral("all"));
    m_gpus->addItem(tr("Integrated GPU only"), QStringLiteral("integrated"));
    m_gpus->addItem(tr("Discrete GPU only"), QStringLiteral("discrete"));
    form->addRow(tr("GPUs:"), m_gpus);
    m_compare = new QSpinBox;
    m_compare->setRange(0, 20);
    m_compare->setValue(3);
    m_compare->setSpecialValueText(tr("off — run once"));
    m_compare->setToolTip(tr("Measure the batch this many times on each GPU alone, on the fastest GPU running as many jobs at once, and on "
                             "all GPUs; check that outputs match; report a difference only when it is statistically significant."));
    form->addRow(tr("Compare runs:"), m_compare);
    body()->addLayout(form);
    auto* buttons = new QHBoxLayout;
    m_run = new QPushButton(tr("Run"));
    m_cancelButton = new QPushButton(tr("Cancel"));
    buttons->addWidget(m_run);
    buttons->addWidget(m_cancelButton);
    buttons->addStretch(1);
    body()->addLayout(buttons);
    m_result = new QLabel;
    m_result->setWordWrap(true);
    m_result->setTextFormat(Qt::RichText);
    m_result->setTextInteractionFlags(Qt::TextSelectableByMouse);
    body()->addWidget(m_result);

    connect(m_kind, &QComboBox::currentIndexChanged, this, &JobBatchPanel::updateControls);
    connect(browse, &QPushButton::clicked, this, &JobBatchPanel::browseInputs);
    connect(m_run, &QPushButton::clicked, this, &JobBatchPanel::start);
    connect(m_cancelButton, &QPushButton::clicked, this, [this] { m_cancel = true; });
    connect(&m_context.hardware(), &hw::HardwareService::detectionFinished, this, &JobBatchPanel::updateControls);
    updateControls();
}

JobBatchPanel::~JobBatchPanel()
{
    m_cancel = true;
    if (m_thread)
        m_thread->wait();
}

void JobBatchPanel::updateControls()
{
    const bool command = m_kind->currentData().toString() == QLatin1StringView("command");
    m_command->setEnabled(command && !m_thread);
    m_pattern->setEnabled(command && !m_thread);
    m_output->setEnabled(command && !m_thread);
    m_run->setEnabled(!m_thread && m_context.hardware().latest().has_value());
    m_cancelButton->setEnabled(m_thread != nullptr);
}

void JobBatchPanel::browseInputs()
{
    const QString dir = QFileDialog::getExistingDirectory(this, tr("Folder with the input files"));
    if (!dir.isEmpty())
        m_inputs->setText(QDir::toNativeSeparators(dir));
}

void JobBatchPanel::configure(const QString& command, const QString& inputs, const QString& outputFolder, const QString& pattern,
                              const QString& gpus, int compareRuns)
{
    m_kind->setCurrentIndex(command == QLatin1StringView("decode") ? 1 : 0);
    if (command != QLatin1StringView("decode"))
        m_command->setText(command);
    m_inputs->setText(inputs);
    m_output->setText(outputFolder);
    m_pattern->setText(pattern);
    m_gpus->setCurrentIndex(std::max(0, m_gpus->findData(gpus)));
    m_compare->setValue(compareRuns);
}

QString JobBatchPanel::resultText() const { return m_result->text(); }

void JobBatchPanel::start()
{
    const auto& detection = m_context.hardware().latest();
    if (m_thread || !detection)
        return;
    auto outcome = std::make_shared<BatchOutcome>();
    outcome->workers = dispatch::workersFrom(*detection);
    QString error;
    const auto use = dispatch::selectWorkers(outcome->workers, m_gpus->currentData().toString(), &error);
    const QStringList inputs = dispatch::inputFiles(m_inputs->text().trimmed());
    const bool decode = m_kind->currentData().toString() == QLatin1StringView("decode");
    const QString command = m_command->text().trimmed();
    if (!use || inputs.isEmpty() || (!decode && command.isEmpty())) {
        m_result->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(colors().bad.name(),
            (!use ? error : inputs.isEmpty() ? tr("Choose input files.") : tr("Enter a command template.")).toHtmlEscaped()));
        return;
    }
    const int runs = m_compare->value();
    if (runs > 0 && use->size() < 2) {
        m_result->setText(tr("Comparing needs at least two GPUs."));
        return;
    }
    const QString outDir = m_output->text().trimmed().isEmpty() ? m_context.data().tempDir() + QStringLiteral("/dispatch") : m_output->text().trimmed();
    const QString pattern = decode ? QString() : m_pattern->text().trimmed();
    const dispatch::Executor executor = decode ? dispatch::decodeExecutor() : dispatch::commandExecutor(command);
    const QList<int> chosen = *use;
    m_cancel = false;
    QPointer<JobBatchPanel> self(this);
    auto report = [self](const QString& text) {
        QMetaObject::invokeMethod(self, [self, text] { if (self) self->m_result->setText(text.toHtmlEscaped()); }, Qt::QueuedConnection);
    };
    m_thread = QThread::create([this, outcome, chosen, inputs, outDir, pattern, executor, runs, report] {
        if (runs > 0) {
            outcome->experiment = dispatch::compareConfigurations(outcome->workers, chosen, inputs, outDir, pattern, executor, runs, &m_cancel,
                                                                  [report](const QString& stage) { report(tr("Measuring: %1…").arg(stage)); });
        } else {
            outcome->run = dispatch::runJobs(outcome->workers, chosen, dispatch::jobsFor(inputs, outDir, pattern), executor, &m_cancel,
                                             [report](int done, int total) { report(tr("%1 of %2 jobs done…").arg(done).arg(total)); });
        }
    });
    connect(m_thread, &QThread::finished, this, [this, outcome] {
        m_thread->deleteLater();
        m_thread = nullptr;
        m_result->setText(describe(*outcome));
        if (outcome->experiment)
            qCInfo(lcBench).noquote() << "Job batch comparison:" << outcome->experiment->verdict;
        else if (outcome->run)
            qCInfo(lcBench).noquote() << "Job batch:" << outcome->run->jobs.size() << "jobs," << outcome->run->failed() << "failed in"
                                      << int(outcome->run->wallMs) << "ms";
        updateControls();
        Q_EMIT finished();
    });
    m_result->setText(tr("Starting…"));
    m_thread->start();
    updateControls();
}

} // namespace gf::ui
