#include "ui/widgets/FrameTimeDialog.h"

#include "frames/FrameMeasure.h"

#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSpinBox>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace gf::ui {

FrameTimeDialog::FrameTimeDialog(quint32 pid, const QString& name, const QString& path, const QList<mon::MonitorTarget>& targets,
                                 const QString& framesDir, const QString& reportsDir, const QString& workDir, QWidget* parent)
    : QDialog(parent), m_pid(pid), m_name(name), m_path(path), m_framesDir(framesDir), m_reportsDir(reportsDir), m_workDir(workDir), m_targets(targets),
      m_cancel(std::make_shared<std::atomic<bool>>(false))
{
    setWindowTitle(tr("Frame times — %1").arg(name));
    resize(640, 560);
    auto* layout = new QVBoxLayout(this);
    auto* intro = new QLabel(tr("<b>%1</b> (process %2)<br>Frame times are measured from outside with Windows' graphics events: nothing is "
                                "loaded into the program and nothing about it is changed. Windows asks for administrator rights for each "
                                "measurement; no setting is changed. Keep the program rendering (not minimized) while it is measured.")
                                 .arg(name.toHtmlEscaped()).arg(pid));
    intro->setWordWrap(true);
    layout->addWidget(intro);

    auto* form = new QFormLayout;
    m_seconds = new QSpinBox;
    m_seconds->setRange(3, 600);
    m_seconds->setValue(20);
    m_seconds->setSuffix(tr(" s"));
    m_seconds->setAccessibleName(tr("Measurement length in seconds"));
    m_label = new QLineEdit;
    m_label->setPlaceholderText(tr("for example: before, after"));
    m_label->setAccessibleName(tr("Measurement label"));
    m_compare = new QComboBox;
    m_compare->setAccessibleName(tr("Compare with an earlier measurement"));
    form->addRow(tr("Length"), m_seconds);
    form->addRow(tr("Label"), m_label);
    form->addRow(tr("Compare with"), m_compare);
    layout->addLayout(form);

    auto* buttons = new QHBoxLayout;
    m_start = new QPushButton(tr("Measure"));
    m_start->setDefault(true);
    m_stop = new QPushButton(tr("Stop early"));
    m_stop->setEnabled(false);
    m_openReport = new QPushButton(tr("Open report"));
    m_openReport->setEnabled(false);
    auto* close = new QPushButton(tr("Close"));
    buttons->addWidget(m_start);
    buttons->addWidget(m_stop);
    buttons->addWidget(m_openReport);
    buttons->addStretch();
    buttons->addWidget(close);
    layout->addLayout(buttons);

    m_progress = new QProgressBar;
    m_progress->setRange(0, 100);
    m_progress->hide();
    m_progress->setAccessibleName(tr("Measurement progress"));
    m_status = new QLabel;
    m_status->setWordWrap(true);
    layout->addWidget(m_progress);
    layout->addWidget(m_status);
    m_result = new QTextBrowser;
    m_result->setAccessibleName(tr("Measurement result"));
    m_result->setOpenLinks(false);
    layout->addWidget(m_result, 1);

    connect(m_start, &QPushButton::clicked, this, &FrameTimeDialog::start);
    connect(m_stop, &QPushButton::clicked, this, [this] {
        m_cancel->store(true);
        m_stop->setEnabled(false);
    });
    connect(m_openReport, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(m_report)); });
    connect(close, &QPushButton::clicked, this, &QDialog::reject);
    reloadEarlier();
}

FrameTimeDialog::~FrameTimeDialog()
{
    m_cancel->store(true);
    if (m_worker.joinable())
        m_worker.join(); // the capture ends within a moment of the stop request
}

void FrameTimeDialog::reloadEarlier()
{
    m_compare->clear();
    m_compare->addItem(tr("Nothing (just measure)"), QString());
    m_earlier.clear();
    for (const frames::FrameSession& s : frames::loadSessions(m_framesDir)) {
        if (!s.measured() || s.processName.compare(m_name, Qt::CaseInsensitive) != 0)
            continue;
        m_earlier << s;
        m_compare->addItem(QStringLiteral("%1%2").arg(s.created.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                                                      s.label.isEmpty() ? QString() : QStringLiteral(" — %1").arg(s.label)),
                           s.id);
    }
    if (m_earlier.size() > 0)
        m_compare->setCurrentIndex(1); // the newest: the usual "before"
}

void FrameTimeDialog::start()
{
    if (m_worker.joinable())
        m_worker.join();
    m_cancel->store(false);
    m_start->setEnabled(false);
    m_stop->setEnabled(true);
    m_openReport->setEnabled(false);
    m_progress->setValue(0);
    m_progress->show();
    m_status->setText(tr("Windows will ask for administrator rights…"));
    m_result->clear();

    frames::MeasureOptions o;
    o.pid = m_pid;
    o.processName = m_name;
    o.processPath = m_path;
    o.seconds = m_seconds->value();
    o.label = m_label->text().trimmed();
    o.helperPath = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("GPUFusionFrameCapture.exe"));
    o.workDir = m_workDir;
    QPointer<FrameTimeDialog> self(this);
    auto cancel = m_cancel;
    const QList<mon::MonitorTarget> targets = m_targets;
    m_worker = std::thread([self, cancel, targets, o] {
        auto progress = [self](const QString& text, int percent) {
            QMetaObject::invokeMethod(qApp, [self, text, percent] {
                if (self) {
                    self->m_status->setText(text);
                    self->m_progress->setValue(percent);
                }
            });
        };
        const frames::FrameSession s = frames::measureFrames(targets, o, cancel.get(), progress);
        QMetaObject::invokeMethod(qApp, [self, s] {
            if (self)
                self->finished(s);
        });
    });
}

void FrameTimeDialog::finished(const frames::FrameSession& s)
{
    m_progress->hide();
    m_start->setEnabled(true);
    m_stop->setEnabled(false);
    QString error;
    if (!frames::saveSession(m_framesDir, s, &error))
        m_status->setText(tr("The measurement could not be saved: %1").arg(error));
    const frames::FrameSession* before = nullptr;
    const QString beforeId = m_compare->currentData().toString();
    for (const frames::FrameSession& e : m_earlier)
        if (e.id == beforeId)
            before = &e;
    QDir().mkpath(m_reportsDir);
    m_report = QDir(m_reportsDir).filePath(s.id + QStringLiteral(".html"));
    QFile f(m_report);
    if (f.open(QIODevice::WriteOnly)) {
        f.write(frames::reportHtml(s, before).toUtf8());
        m_openReport->setEnabled(true);
    }
    QString html;
    if (before && s.measured()) {
        const frames::FrameComparison c = frames::compareSessions(*before, s);
        html += QStringLiteral("<p><b>%1</b></p>").arg((c.comparable ? c.text : c.reason).toHtmlEscaped());
    }
    html += QStringLiteral("<ul>");
    for (const QString& line : frames::summaryLines(s))
        html += QStringLiteral("<li>%1</li>").arg(line.toHtmlEscaped());
    html += QStringLiteral("</ul>");
    m_result->setHtml(html);
    if (error.isEmpty())
        m_status->setText(s.measured() ? tr("Measured and saved (%1).").arg(s.id) : tr("Not measured."));
    reloadEarlier();
}

} // namespace gf::ui
