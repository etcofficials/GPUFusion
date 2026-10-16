#include "ui/pages/SettingsPage.h"

#include "ui/AppContext.h"
#include "ui/widgets/Card.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QVBoxLayout>

namespace gf::ui {

namespace {

QSpinBox* makeSpin(SettingRange range, int step, const QString& suffix)
{
    auto* spin = new QSpinBox;
    spin->setRange(range.min, range.max);
    spin->setSingleStep(step);
    spin->setSuffix(suffix);
    spin->setMinimumWidth(140);
    return spin;
}

QComboBox* makeChoice(const QList<QPair<QString, QString>>& items)
{
    auto* combo = new QComboBox;
    for (const auto& [label, value] : items)
        combo->addItem(label, value);
    combo->setMinimumWidth(140);
    return combo;
}

QFormLayout* makeForm(Card* card)
{
    auto* form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignLeft);
    form->setFieldGrowthPolicy(QFormLayout::FieldsStayAtSizeHint);
    form->setHorizontalSpacing(24);
    form->setVerticalSpacing(8);
    card->body()->addLayout(form);
    return form;
}

QLabel* note(const QString& text)
{
    auto* label = new QLabel(text);
    label->setObjectName(QStringLiteral("muted"));
    label->setWordWrap(true);
    return label;
}

} // namespace

SettingsPage::SettingsPage(AppContext& context, QWidget* parent) : QWidget(parent), m_context(context)
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
    outer->addWidget(scroll, 1);

    if (const auto& loaded = m_context.loadResult(); !loaded.warnings.isEmpty()) {
        auto* banner = new QLabel(tr("Settings file %1:\n• %2").arg(describe(loaded.status), loaded.warnings.join(QStringLiteral("\n• "))));
        banner->setObjectName(QStringLiteral("warningBanner"));
        banner->setWordWrap(true);
        layout->addWidget(banner);
    }

    // Monitoring
    auto* monitoring = new Card(tr("Monitoring"));
    auto* monForm = makeForm(monitoring);
    m_refreshMs = makeSpin(ranges::refreshIntervalMs, 250, tr(" ms"));
    m_historySeconds = makeSpin(ranges::historySeconds, 60, tr(" s"));
    monForm->addRow(tr("Refresh interval"), m_refreshMs);
    monForm->addRow(tr("Graph history"), m_historySeconds);
    m_useNvml = new QCheckBox(tr("Read NVIDIA sensors through NVML (temperature, fan, performance state)"));
    monitoring->body()->addWidget(m_useNvml);
    monitoring->body()->addWidget(note(tr("Changes apply immediately. Shorter intervals cost more CPU time; changing the "
                                          "interval clears the graphs so their time axis stays accurate.")));
    layout->addWidget(monitoring);

    // Startup
    auto* startup = new Card(tr("Startup"));
    m_startMinimized = new QCheckBox(tr("Start minimized"));
    m_startMonitoring = new QCheckBox(tr("Start monitoring when GPU Fusion opens"));
    startup->body()->addWidget(m_startMinimized);
    startup->body()->addWidget(m_startMonitoring);
    startup->body()->addWidget(note(tr("GPU Fusion never registers itself to start with Windows or installs a background service.")));
    layout->addWidget(startup);

    // Appearance
    auto* appearance = new Card(tr("Appearance"));
    m_theme = makeChoice({ { tr("Dark"), QStringLiteral("dark") }, { tr("Light"), QStringLiteral("light") } });
    makeForm(appearance)->addRow(tr("Theme"), m_theme);
    layout->addWidget(appearance);

    // Logging
    auto* logging = new Card(tr("Logging"));
    auto* logForm = makeForm(logging);
    m_logLevel = makeChoice({ { tr("Debug"), QStringLiteral("debug") }, { tr("Info"), QStringLiteral("info") },
                              { tr("Warning"), QStringLiteral("warning") }, { tr("Error"), QStringLiteral("error") } });
    m_logMaxKB = makeSpin(ranges::logMaxFileKB, 256, tr(" KB"));
    m_logFiles = makeSpin(ranges::logMaxFiles, 1, QString());
    logForm->addRow(tr("Minimum level"), m_logLevel);
    logForm->addRow(tr("Maximum file size"), m_logMaxKB);
    logForm->addRow(tr("Files kept"), m_logFiles);
    logging->body()->addWidget(note(tr("Logs are written to %1. Disk use is capped at file size × files kept.")
                                        .arg(QDir::toNativeSeparators(m_context.data().logsDir()))));
    layout->addWidget(logging);

    // Benchmarks
    auto* bench = new Card(tr("Benchmarks"));
    bench->setBadge(tr("Used from Phase 7"));
    auto* benchForm = makeForm(bench);
    m_warmupRuns = makeSpin(ranges::benchmarkWarmupRuns, 1, QString());
    m_measuredRuns = makeSpin(ranges::benchmarkMeasuredRuns, 1, QString());
    m_retentionDays = makeSpin(ranges::benchmarkRetentionDays, 30, tr(" days"));
    m_retentionDays->setSpecialValueText(tr("Keep forever"));
    benchForm->addRow(tr("Warm-up runs"), m_warmupRuns);
    benchForm->addRow(tr("Measured runs"), m_measuredRuns);
    benchForm->addRow(tr("Keep results for"), m_retentionDays);
    layout->addWidget(bench);
    layout->addStretch();

    // Action bar (outside the scroll area so it is always reachable)
    auto* actions = new QHBoxLayout;
    m_status = new QLabel;
    m_status->setObjectName(QStringLiteral("muted"));
    m_status->setWordWrap(true);
    auto* defaults = new QPushButton(tr("Restore defaults"));
    m_revert = new QPushButton(tr("Revert"));
    m_save = new QPushButton(tr("Save"));
    m_save->setDefault(true);
    actions->addWidget(m_status, 1);
    actions->addWidget(defaults);
    actions->addWidget(m_revert);
    actions->addWidget(m_save);
    outer->addSpacing(10);
    outer->addLayout(actions);

    showSettings(m_context.settings());

    connect(m_save, &QPushButton::clicked, this, &SettingsPage::save);
    connect(m_revert, &QPushButton::clicked, this, [this] { showSettings(m_context.settings()); });
    connect(defaults, &QPushButton::clicked, this, [this] {
        Settings d;
        d.windowGeometry = m_context.settings().windowGeometry;
        showSettings(d);
    });

    for (QSpinBox* spin : { m_refreshMs, m_historySeconds, m_logMaxKB, m_logFiles, m_warmupRuns, m_measuredRuns, m_retentionDays })
        connect(spin, &QSpinBox::valueChanged, this, &SettingsPage::updateDirtyState);
    for (QCheckBox* box : { m_startMinimized, m_startMonitoring, m_useNvml })
        connect(box, &QCheckBox::toggled, this, &SettingsPage::updateDirtyState);
    for (QComboBox* combo : { m_theme, m_logLevel })
        connect(combo, &QComboBox::currentIndexChanged, this, &SettingsPage::updateDirtyState);
}

void SettingsPage::showSettings(const Settings& s)
{
    m_refreshMs->setValue(s.refreshIntervalMs);
    m_historySeconds->setValue(s.historySeconds);
    m_useNvml->setChecked(s.useNvml);
    m_startMinimized->setChecked(s.startMinimized);
    m_startMonitoring->setChecked(s.startMonitoringOnLaunch);
    m_theme->setCurrentIndex(std::max(0, m_theme->findData(s.theme)));
    m_logLevel->setCurrentIndex(std::max(0, m_logLevel->findData(s.logLevel)));
    m_logMaxKB->setValue(s.logMaxFileKB);
    m_logFiles->setValue(s.logMaxFiles);
    m_warmupRuns->setValue(s.benchmarkWarmupRuns);
    m_measuredRuns->setValue(s.benchmarkMeasuredRuns);
    m_retentionDays->setValue(s.benchmarkRetentionDays);
    updateDirtyState();
}

Settings SettingsPage::collect() const
{
    Settings s = m_context.settings(); // keeps non-editable state such as window geometry
    s.refreshIntervalMs = m_refreshMs->value();
    s.historySeconds = m_historySeconds->value();
    s.useNvml = m_useNvml->isChecked();
    s.startMinimized = m_startMinimized->isChecked();
    s.startMonitoringOnLaunch = m_startMonitoring->isChecked();
    s.theme = m_theme->currentData().toString();
    s.logLevel = m_logLevel->currentData().toString();
    s.logMaxFileKB = m_logMaxKB->value();
    s.logMaxFiles = m_logFiles->value();
    s.benchmarkWarmupRuns = m_warmupRuns->value();
    s.benchmarkMeasuredRuns = m_measuredRuns->value();
    s.benchmarkRetentionDays = m_retentionDays->value();
    return s;
}

void SettingsPage::updateDirtyState()
{
    const bool changed = !(collect() == m_context.settings());
    const bool onDisk = QFileInfo::exists(m_context.store().filePath());
    const bool readOnly = m_context.store().isReadOnly();
    m_save->setEnabled((changed || !onDisk) && !readOnly);
    m_revert->setEnabled(changed);
    if (readOnly)
        m_status->setText(tr("Saving is disabled for this settings file — see the warning above."));
    else if (changed)
        m_status->setText(tr("Unsaved changes"));
    else if (!onDisk)
        m_status->setText(tr("Defaults in use; not saved yet"));
    else
        m_status->setText(tr("Saved to %1").arg(QDir::toNativeSeparators(m_context.store().filePath())));
}

void SettingsPage::save()
{
    QString error;
    if (m_context.updateSettings(collect(), &error))
        updateDirtyState();
    else
        m_status->setText(tr("Could not save: %1").arg(error));
}

} // namespace gf::ui
