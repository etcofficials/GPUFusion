#include "ui/pages/SettingsPage.h"

#include "ui/AppContext.h"
#include "ui/widgets/Card.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDir>
#include <QFileInfo>
#include <QFileDialog>
#include <QFormLayout>
#include <QLineEdit>

#include "profiles/ProfileStore.h"
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
    auto* benchForm = makeForm(bench);
    m_warmupRuns = makeSpin(ranges::benchmarkWarmupRuns, 1, QString());
    m_measuredRuns = makeSpin(ranges::benchmarkMeasuredRuns, 1, QString());
    m_retentionDays = makeSpin(ranges::benchmarkRetentionDays, 30, tr(" days"));
    m_retentionDays->setSpecialValueText(tr("Keep forever"));
    benchForm->addRow(tr("Warm-up runs"), m_warmupRuns);
    benchForm->addRow(tr("Measured runs"), m_measuredRuns);
    benchForm->addRow(tr("Keep results for"), m_retentionDays);
    layout->addWidget(bench);

    // GPU profiles: automatic backup
    auto* profilesCard = new Card(tr("GPU profiles"));
    auto* profForm = makeForm(profilesCard);
    auto* folderRow = new QHBoxLayout;
    m_backupFolder = new QLineEdit;
    m_backupFolder->setPlaceholderText(tr("off — choose a folder, ideally on another drive"));
    m_backupFolder->setMinimumWidth(360);
    m_backupFolder->setAccessibleName(tr("Profile backup folder"));
    auto* browse = new QPushButton(tr("Browse…"));
    browse->setAccessibleName(tr("Choose the profile backup folder"));
    folderRow->addWidget(m_backupFolder, 1);
    folderRow->addWidget(browse);
    profForm->addRow(tr("Automatic backup to"), folderRow);
    m_backupsKept = makeSpin(ranges::profileBackupsKept, 1, tr(" backups"));
    m_backupsKept->setAccessibleName(tr("Profile backups kept"));
    profForm->addRow(tr("Keep the newest"), m_backupsKept);
    m_backupNote = note(QString());
    profilesCard->body()->addWidget(m_backupNote);
    layout->addWidget(profilesCard);
    connect(browse, &QPushButton::clicked, this, [this] {
        const QString dir = QFileDialog::getExistingDirectory(this, tr("Folder for automatic profile backups"), m_backupFolder->text());
        if (!dir.isEmpty())
            m_backupFolder->setText(QDir::toNativeSeparators(dir));
    });
    connect(m_backupFolder, &QLineEdit::textChanged, this, [this] { updateBackupNote(); updateDirtyState(); });
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

    for (QSpinBox* spin : { m_refreshMs, m_historySeconds, m_logMaxKB, m_logFiles, m_warmupRuns, m_measuredRuns, m_retentionDays, m_backupsKept })
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
    m_backupFolder->setText(s.profileBackupFolder);
    m_backupsKept->setValue(s.profileBackupsKept);
    updateBackupNote();
    updateDirtyState();
}

void SettingsPage::updateBackupNote()
{
    const QString folder = m_backupFolder->text().trimmed();
    QString text;
    if (folder.isEmpty())
        text = tr("Off. When a folder is set, GPU Fusion saves all GPU profiles there after every change (an export file, the same "
                  "format as GPU Manager → Export…), keeps the newest ones and deletes nothing else in that folder.");
    else if (QDir::isRelativePath(folder))
        text = tr("Enter a full path, e.g. D:\\Backups\\GPU Fusion.");
    else if (profiles::onSameVolume(folder, m_context.data().root))
        text = tr("Warning: this folder is on the same drive as GPU Fusion's data (%1). A backup there does not survive that drive "
                  "failing; choose a folder on another drive.").arg(QDir::toNativeSeparators(m_context.data().root));
    else
        text = tr("Profiles are saved there after every change. Restore with GPU Manager → Import….");
    m_backupNote->setText(text);
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
    s.profileBackupFolder = QDir::toNativeSeparators(m_backupFolder->text().trimmed());
    s.profileBackupsKept = m_backupsKept->value();
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
    if (!m_context.updateSettings(collect(), &error)) {
        m_status->setText(tr("Could not save: %1").arg(error));
        return;
    }
    updateDirtyState();
    // A newly chosen backup folder gets a first backup now, not only at the next profile change.
    const Settings& s = m_context.settings();
    if (!s.profileBackupFolder.isEmpty() && m_context.profiles()) {
        const profiles::BackupResult r = profiles::backupStore(*m_context.profiles(), s.profileBackupFolder, s.profileBackupsKept);
        m_status->setText(!r.error.isEmpty() ? tr("Saved, but the profile backup failed: %1").arg(r.error)
                          : r.written       ? tr("Saved. Profiles backed up to %1.").arg(r.path)
                                            : tr("Saved. The newest backup already holds the current profiles."));
    }
}

} // namespace gf::ui
