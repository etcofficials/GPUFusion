#include "ui/pages/PreferencesPage.h"

#include "core/Logging.h"
#include "hardware/HardwareService.h"
#include "monitoring/UsageObserver.h"
#include "platform/windows/PeImports.h"
#include "platform/windows/ProbeRunner.h"
#include "platform/windows/ProcessList.h"
#include "preferences/PreferenceService.h"
#include "ui/AppContext.h"
#include "ui/Theme.h"
#include "ui/UsageCheck.h"
#include "ui/widgets/Card.h"

#include <QButtonGroup>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QSet>
#include <QTableWidget>
#include <QThread>
#include <QUrl>
#include <QVBoxLayout>

#include <memory>

namespace gf::ui {

using namespace gf::pref;

namespace {

QLabel* note(const QString& text)
{
    auto* l = new QLabel(text);
    l->setObjectName(QStringLiteral("muted"));
    l->setWordWrap(true);
    return l;
}

QLabel* richLabel()
{
    auto* l = new QLabel;
    l->setWordWrap(true);
    l->setTextFormat(Qt::RichText);
    l->setTextInteractionFlags(Qt::TextSelectableByMouse);
    return l;
}

QTableWidget* makeTable(const QStringList& headers)
{
    auto* t = new QTableWidget(0, int(headers.size()));
    t->setHorizontalHeaderLabels(headers);
    t->verticalHeader()->hide();
    t->setEditTriggers(QAbstractItemView::NoEditTriggers);
    t->setSelectionMode(QAbstractItemView::NoSelection);
    t->setShowGrid(false);
    t->setAlternatingRowColors(true);
    t->horizontalHeader()->setStretchLastSection(true);
    t->verticalHeader()->setDefaultSectionSize(24);
    return t;
}

QString shortValue(const std::optional<QString>& v) { return v ? *v : QObject::tr("not set"); }

QColor enforcementColor(Enforcement e)
{
    const ThemeColors& c = colors();
    switch (e) {
    case Enforcement::Verified:       return c.good;
    case Enforcement::Expected:       return c.accent;
    case Enforcement::NotEnforceable: return c.warn;
    default:                          return c.textMuted;
    }
}

QColor stageColor(mon::StageState s)
{
    const ThemeColors& c = colors();
    switch (s) {
    case mon::StageState::Passed:        return c.good;
    case mon::StageState::Failed:        return c.bad;
    case mon::StageState::Inconclusive:  return c.warn;
    case mon::StageState::NotApplicable: return c.textMuted;
    }
    return c.textMuted;
}

} // namespace

PreferencesPage::PreferencesPage(AppContext& context, QWidget* parent) : QWidget(parent), m_context(context)
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

    // --- What a preference does on this PC ------------------------------------------------------
    auto* verifyCard = new Card(tr("What the Windows preference does on this PC"));
    verifyCard->body()->addWidget(note(tr("Windows lets you mark an app as \"Power saving\" or \"High performance\". Whether that moves the app "
                                          "to a different GPU depends on the hardware and drivers. GPU Fusion checks by launching its own "
                                          "harmless probe program (GPUFusionProbe.exe) under each preference and recording which GPU it gets. "
                                          "This temporarily changes the preference for the probe only, and restores it afterwards.")));
    m_verdict = new QLabel;
    m_verdict->setWordWrap(true);
    verifyCard->body()->addWidget(m_verdict);
    m_verification = makeTable({ tr("Preference set for the probe"), tr("GPU the probe's Direct3D device used"), tr("First GPU in its adapter list") });
    m_verification->setAccessibleName(tr("What each Windows preference did on this PC"));
    m_verification->setColumnWidth(0, 200);
    m_verification->setColumnWidth(1, 260);
    m_verification->setFixedHeight(4 * 24 + 30);
    verifyCard->body()->addWidget(m_verification);
    auto* verifyRow = new QHBoxLayout;
    m_verify = new QPushButton(tr("Run verification"));
    verifyRow->addWidget(m_verify);
    verifyRow->addStretch();
    verifyCard->body()->addLayout(verifyRow);
    layout->addWidget(verifyCard);

    // --- Applications and editor ------------------------------------------------------------------
    auto* appsCard = new Card(tr("Applications"));
    auto* appsRow = new QHBoxLayout;
    appsRow->setSpacing(18);
    auto* listCol = new QVBoxLayout;
    m_apps = new QListWidget;
    m_apps->setMinimumWidth(280);
    m_apps->setMinimumHeight(170);
    m_apps->setAccessibleName(tr("Applications with a graphics preference"));
    auto* addButton = new QPushButton(tr("Add application…"));
    listCol->addWidget(m_apps, 1);
    listCol->addWidget(addButton);
    appsRow->addLayout(listCol, 1);

    auto* editor = new QVBoxLayout;
    m_selected = new QLabel(tr("Select or add an application."));
    m_selected->setWordWrap(true);
    m_selected->setTextInteractionFlags(Qt::TextSelectableByMouse);
    editor->addWidget(m_selected);
    editor->addWidget(new QLabel(tr("<b>Which GPU should it use?</b>")));
    m_gpuChoice = new QButtonGroup(this);
    m_auto = new QRadioButton(tr("Automatic (Windows decides)"));
    m_integrated = new QRadioButton(tr("Integrated GPU"));
    m_discrete = new QRadioButton(tr("Discrete GPU"));
    m_both = new QRadioButton(tr("Both GPUs"));
    int gid = 0;
    for (QRadioButton* r : { m_auto, m_integrated, m_discrete, m_both }) {
        m_gpuChoice->addButton(r, gid++);
        editor->addWidget(r);
    }
    m_plan = richLabel();
    editor->addWidget(m_plan);

    m_advanced = new QGroupBox(tr("Advanced: set a raw Windows value instead"));
    m_advanced->setCheckable(true);
    m_advanced->setChecked(false);
    m_advanced->setToolTip(tr("Writes exactly the chosen Windows value, whatever it does on this PC."));
    auto* advancedLayout = new QVBoxLayout(m_advanced);
    m_choice = new QButtonGroup(this);
    m_notSet = new QRadioButton(tr("Not set (remove GPU Fusion's / Windows' choice)"));
    m_letWindows = new QRadioButton(tr("Let Windows decide"));
    m_power = new QRadioButton(tr("Power saving"));
    m_performance = new QRadioButton(tr("High performance"));
    int id = 0;
    for (QRadioButton* r : { m_notSet, m_letWindows, m_power, m_performance }) {
        m_choice->addButton(r, id++);
        advancedLayout->addWidget(r);
    }
    editor->addWidget(m_advanced);

    auto* editorButtons = new QHBoxLayout;
    m_apply = new QPushButton(tr("Apply…"));
    m_apply->setDefault(true);
    m_verifyUsage = new QPushButton(tr("Verify on this PC…"));
    m_verifyUsage->setToolTip(tr("Start the application and watch which GPU its processes actually render on"));
    m_launch = new QPushButton(tr("Launch"));
    m_launch->setToolTip(tr("Start the application normally"));
    auto* windowsSettings = new QPushButton(tr("Open Windows graphics settings"));
    editorButtons->addWidget(m_apply);
    editorButtons->addWidget(m_verifyUsage);
    editorButtons->addWidget(m_launch);
    editorButtons->addStretch();
    editorButtons->addWidget(windowsSettings);
    editor->addLayout(editorButtons);
    m_editorNote = note(QString());
    editor->addWidget(m_editorNote);
    m_usageResult = richLabel();
    editor->addWidget(m_usageResult);
    m_usageActions = new QVBoxLayout;
    editor->addLayout(m_usageActions);
    editor->addStretch();
    appsRow->addLayout(editor, 2);
    appsCard->body()->addLayout(appsRow);
    appsCard->body()->addWidget(note(tr("A preference is a request, not a guarantee: it only affects programs that let Windows pick the GPU, "
                                        "and only when they next start. Programs that choose a GPU themselves (many games and editors have a "
                                        "setting) ignore it — \"Verify on this PC\" shows where a program really renders. Changes are per user, "
                                        "need no administrator rights, and can be undone below.")));
    layout->addWidget(appsCard);

    // --- History ---------------------------------------------------------------------------------
    auto* historyCard = new Card(tr("Change history"));
    historyCard->body()->addWidget(note(tr("Every change GPU Fusion makes is recorded with the previous value before Windows is touched. "
                                           "Undo puts the previous value back; it refuses if the setting was changed elsewhere since.")));
    m_history = makeTable({ tr("When"), tr("Application"), tr("Before → after"), tr("") });
    m_history->setAccessibleName(tr("Change history"));
    m_history->setColumnWidth(0, 130);
    m_history->setColumnWidth(1, 280);
    m_history->setColumnWidth(2, 260);
    m_history->setMinimumHeight(150);
    historyCard->body()->addWidget(m_history);
    layout->addWidget(historyCard);
    layout->addStretch();

    PreferenceService& prefs = m_context.preferences();
    connect(m_verify, &QPushButton::clicked, this, [this, &prefs] {
        const auto answer = QMessageBox::question(this, tr("Run verification"),
            tr("GPU Fusion will launch its probe program four times, each time with a different Windows graphics preference set for:\n\n%1\n\n"
               "This changes only that program's entry under HKEY_CURRENT_USER\\Software\\Microsoft\\DirectX\\UserGpuPreferences, "
               "and restores it afterwards (also after a crash, at the next start). Continue?").arg(prefs.probePath()));
        if (answer == QMessageBox::Yes)
            prefs.startVerification();
    });
    connect(&prefs, &PreferenceService::verificationStarted, this, [this] {
        setBusy(true);
        m_verdict->setText(tr("Verifying… launching the probe under each preference."));
    });
    connect(&prefs, &PreferenceService::verificationFinished, this, [this] {
        setBusy(false);
        refreshVerification();
        refreshChoices();
    });
    connect(&prefs, &PreferenceService::changed, this, [this] {
        refreshApplications();
        refreshHistory();
        refreshEditor();
    });
    connect(&m_context.hardware(), &hw::HardwareService::detectionFinished, this, &PreferencesPage::refreshChoices);
    connect(addButton, &QPushButton::clicked, this, &PreferencesPage::addApplication);
    connect(m_apps, &QListWidget::currentRowChanged, this, [this] {
        m_usageResult->clear();
        while (QLayoutItem* item = m_usageActions->takeAt(0)) {
            delete item->widget();
            delete item;
        }
        refreshEditor();
    });
    connect(m_gpuChoice, &QButtonGroup::idToggled, this, [this](int, bool on) {
        if (on)
            refreshChoices();
    });
    connect(m_advanced, &QGroupBox::toggled, this, [this](bool on) {
        for (QAbstractButton* b : m_gpuChoice->buttons())
            b->setEnabled(!on && !selectedPath().isEmpty());
        refreshChoices();
    });
    connect(m_apply, &QPushButton::clicked, this, &PreferencesPage::apply);
    connect(m_verifyUsage, &QPushButton::clicked, this, &PreferencesPage::verifyUsage);
    connect(m_launch, &QPushButton::clicked, this, &PreferencesPage::launch);
    connect(windowsSettings, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:display-advancedgraphics")));
    });

    refreshVerification();
    refreshApplications();
    refreshHistory();
    refreshEditor();
}

PreferencesPage::~PreferencesPage()
{
    m_cancelUsage = true;
    if (m_usageThread)
        m_usageThread->wait();
}

void PreferencesPage::setBusy(bool busy)
{
    // The verification thread uses the registry and the journal; no edits meanwhile.
    m_verify->setEnabled(!busy && m_context.preferences().probeAvailable());
    m_history->setEnabled(!busy);
    refreshChoices();
}

QList<SelectableGpu> PreferencesPage::selectableGpus() const
{
    const auto& d = m_context.hardware().latest();
    return d ? ui::selectableGpus(*d) : QList<SelectableGpu>{};
}

MultiGpuSystem PreferencesPage::multiGpuSystem() const
{
    MultiGpuSystem s;
    const auto& d = m_context.hardware().latest();
    if (!d)
        return s;
    QSet<quint32> vendors;
    for (const hw::Adapter& a : d->adapters) {
        if (!a.dxgi || a.isSoftware())
            continue;
        ++s.physicalGpus;
        vendors.insert(a.dxgi->vendorId);
        if (a.dxgi->d3d12.device.ok())
            ++s.d3d12Gpus;
        else
            s.gpusWithoutD3D12 << a.name();
    }
    s.sameVendor = vendors.size() == 1;
    const auto& v = m_context.preferences().lastVerification();
    s.preferenceMovesApps = v && v->restored && !v->preferenceHasNoEffect();
    return s;
}

GpuChoice PreferencesPage::selectedChoice() const
{
    switch (m_gpuChoice->checkedId()) {
    case 1: return GpuChoice::Integrated;
    case 2: return GpuChoice::Discrete;
    case 3: return GpuChoice::Both;
    default: return GpuChoice::Automatic;
    }
}

void PreferencesPage::refreshVerification()
{
    const PreferenceService& prefs = m_context.preferences();
    const ThemeColors& c = colors();
    const auto& v = prefs.lastVerification();
    m_verification->setRowCount(0);
    if (!prefs.probeAvailable()) {
        m_verdict->setText(tr("The probe program is missing (%1), so preferences cannot be verified.").arg(prefs.probePath()));
        m_verify->setEnabled(false);
        return;
    }
    if (!v) {
        m_verdict->setText(tr("Not verified yet on this PC. Run the verification so GPU Fusion knows which GPU choices Windows can enforce."));
        return;
    }
    for (const VerificationStep& s : v->steps) {
        const int row = m_verification->rowCount();
        m_verification->insertRow(row);
        m_verification->setItem(row, 0, new QTableWidgetItem(gpuPreferenceText(s.preference)));
        m_verification->setItem(row, 1, new QTableWidgetItem(s.ok ? s.report.d3d11Default->name : tr("Failed: %1").arg(s.error)));
        m_verification->setItem(row, 2, new QTableWidgetItem(s.report.enumOrder.isEmpty() ? QString() : s.report.enumOrder.first().name));
    }
    QString verdict;
    const QString when = QLocale().toString(v->time, QLocale::ShortFormat);
    if (!v->restored) {
        verdict = tr("<span style='color:%1'>The probe's preference could not be restored (%2). GPU Fusion will retry at the next start.</span>")
                      .arg(c.bad.name(), v->restoreError.toHtmlEscaped());
    } else if (v->preferenceHasNoEffect()) {
        const VerificationStep* ok = nullptr;
        for (const VerificationStep& s : v->steps)
            ok = ok ? ok : (s.ok ? &s : nullptr);
        verdict = tr("<b>On this PC the preference makes no difference</b> (verified %1): with every setting, the probe's Direct3D device "
                     "was created on <b>%2</b>. Windows only moves apps between GPUs when it recognises a power-saving (integrated) GPU; "
                     "here it does not treat the other GPU as one — the GPUs page shows how Windows classifies each adapter. "
                     "Setting \"Power saving\" will therefore not move apps to it.")
                      .arg(when, ok ? ok->report.d3d11Default->name.toHtmlEscaped() : QString());
    } else {
        QStringList parts;
        for (const VerificationStep& s : v->steps) {
            if (s.ok)
                parts << QStringLiteral("%1 → %2").arg(gpuPreferenceText(s.preference), s.report.d3d11Default->name).toHtmlEscaped();
        }
        verdict = tr("<b>The preference changes the GPU on this PC</b> (verified %1): %2.").arg(when, parts.join(QStringLiteral("; ")));
    }
    m_verdict->setText(verdict);
}

void PreferencesPage::refreshApplications()
{
    const QString current = selectedPath();
    QStringList apps = m_context.preferences().configuredApplications();
    for (const QString& extra : m_extraApps) {
        if (!findValueName(m_context.preferences().store(), extra))
            apps << extra;
    }
    m_apps->blockSignals(true);
    m_apps->clear();
    for (const QString& path : apps) {
        auto* item = new QListWidgetItem(QStringLiteral("%1 — %2").arg(QFileInfo(path).fileName(),
                                                                       gpuPreferenceText(m_context.preferences().manager().current(path))));
        item->setData(Qt::UserRole, path);
        item->setToolTip(path);
        m_apps->addItem(item);
        if (path.compare(current, Qt::CaseInsensitive) == 0)
            m_apps->setCurrentItem(item);
    }
    if (apps.isEmpty()) {
        auto* empty = new QListWidgetItem(tr("No application has a graphics preference yet.\nUse \"Add application…\" to choose one."));
        empty->setFlags(Qt::NoItemFlags);
        m_apps->addItem(empty);
    }
    m_apps->blockSignals(false);
}

QString PreferencesPage::selectedPath() const
{
    const QListWidgetItem* item = m_apps->currentItem();
    return item ? item->data(Qt::UserRole).toString() : QString();
}

bool PreferencesPage::isRunning(const QString& exePath) const
{
    mon::win::ProcessList list;
    const auto processes = list.snapshot();
    return std::any_of(processes.begin(), processes.end(), [&](const mon::win::ProcessInfo& p) {
        return QDir::toNativeSeparators(p.path).compare(QDir::toNativeSeparators(exePath), Qt::CaseInsensitive) == 0;
    });
}

void PreferencesPage::refreshEditor()
{
    const QString path = selectedPath();
    const bool has = !path.isEmpty();
    for (QAbstractButton* b : m_choice->buttons())
        b->setEnabled(has);
    for (QAbstractButton* b : m_gpuChoice->buttons())
        b->setEnabled(has && !m_advanced->isChecked());
    m_advanced->setEnabled(has);
    m_launch->setEnabled(has && QFileInfo::exists(path));
    if (!has) {
        m_selected->setText(tr("Select or add an application."));
        m_editorNote->clear();
        refreshChoices();
        return;
    }
    PreferenceManager& manager = m_context.preferences().manager();
    const GpuPreference current = manager.current(path);
    m_selected->setText(tr("<b>%1</b><br>%2<br>Stored value: <code>%3</code>")
                            .arg(QFileInfo(path).fileName().toHtmlEscaped(), QDir::toNativeSeparators(path).toHtmlEscaped(),
                                 shortValue(manager.rawValue(path)).toHtmlEscaped()));
    QAbstractButton* button = current == GpuPreference::LetWindowsDecide ? m_letWindows
                            : current == GpuPreference::PowerSaving      ? m_power
                            : current == GpuPreference::HighPerformance  ? m_performance
                                                                         : m_notSet;
    button->setChecked(true);

    // Show the stored value as an adapter choice where it maps to one on this PC.
    const auto& v = m_context.preferences().lastVerification();
    const QList<SelectableGpu> gpus = selectableGpus();
    QRadioButton* adapterButton = m_auto;
    if (current == GpuPreference::PowerSaving || current == GpuPreference::HighPerformance) {
        const GpuChoice c = current == GpuPreference::PowerSaving ? GpuChoice::Integrated : GpuChoice::Discrete;
        if (planChoice(c, gpus, v).write == current)
            adapterButton = c == GpuChoice::Integrated ? m_integrated : m_discrete;
        else
            adapterButton = nullptr; // stored value that does not mean an adapter here: shown in Advanced
    }
    if (adapterButton) {
        m_advanced->setChecked(false);
        adapterButton->setChecked(true);
    } else {
        m_advanced->setChecked(true);
    }
    refreshChoices();
}

void PreferencesPage::refreshChoices()
{
    const QList<SelectableGpu> gpus = selectableGpus();
    const auto& v = m_context.preferences().lastVerification();
    const auto integrated = gpuFor(GpuChoice::Integrated, gpus);
    const auto discrete = gpuFor(GpuChoice::Discrete, gpus);
    const ChoicePlan intPlan = planChoice(GpuChoice::Integrated, gpus, v);
    const ChoicePlan disPlan = planChoice(GpuChoice::Discrete, gpus, v);
    m_integrated->setText(integrated ? tr("Integrated GPU: %1 — %2").arg(integrated->name, enforcementText(intPlan.enforcement))
                                     : tr("Integrated GPU (none detected)"));
    m_discrete->setText(discrete ? tr("Discrete GPU: %1 — %2").arg(discrete->name, enforcementText(disPlan.enforcement))
                                 : tr("Discrete GPU (none detected)"));
    m_both->setText(tr("Both GPUs — not possible as a setting (see the check below)"));

    const QString path = selectedPath();
    const bool busy = m_context.preferences().isVerifying() || m_usageThread;
    m_verifyUsage->setEnabled(!path.isEmpty() && QFileInfo::exists(path) && !busy && m_context.hardware().latest().has_value());
    if (path.isEmpty()) {
        m_plan->clear();
        m_apply->setEnabled(false);
        return;
    }
    if (m_advanced->isChecked()) {
        m_plan->setText(tr("<span style='color:%1'>Advanced: the chosen Windows value is written as is.</span>").arg(colors().textMuted.name()));
        m_apply->setEnabled(!busy);
        return;
    }
    const ChoicePlan plan = planChoice(selectedChoice(), gpus, v);
    QStringList html{ QStringLiteral("<span style='color:%1'>&#9679;</span> %2").arg(enforcementColor(plan.enforcement).name(), plan.summary.toHtmlEscaped()) };
    for (const QString& r : plan.reasons)
        html << QStringLiteral("<span style='color:%1'>• %2</span>").arg(colors().textMuted.name(), r.toHtmlEscaped());
    if (plan.choice == GpuChoice::Both) {
        QString error;
        const auto imports = win::readImports(path, &error);
        const DualGpuAssessment a = assessDualGpu(QFileInfo(path).fileName(), imports, error, multiGpuSystem());
        html << QStringLiteral("<b>%1</b>").arg(a.verdict.toHtmlEscaped());
        for (const QString& e : a.evidence)
            html << QStringLiteral("<span style='color:%1'>%2</span>").arg(colors().textMuted.name(), e.toHtmlEscaped());
        for (const QString& c : a.cases)
            html << QStringLiteral("• %1").arg(c.toHtmlEscaped());
    }
    m_plan->setText(html.join(QStringLiteral("<br>")));
    m_apply->setEnabled(plan.write.has_value() && !busy);
    m_apply->setToolTip(plan.write ? QString() : tr("Nothing would be stored: %1").arg(plan.summary));
}

void PreferencesPage::addApplication()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose an application"), QString(), tr("Programs (*.exe)"));
    if (!path.isEmpty())
        addAndSelect(path);
}

void PreferencesPage::addAndSelect(const QString& path)
{
    QString why;
    if (!isConfigurableExecutable(path, &why)) {
        QMessageBox::warning(this, tr("Add application"), why);
        return;
    }
    const QString native = QDir::toNativeSeparators(path);
    if (!m_extraApps.contains(native, Qt::CaseInsensitive))
        m_extraApps << native;
    refreshApplications();
    for (int i = 0; i < m_apps->count(); ++i) {
        if (m_apps->item(i)->data(Qt::UserRole).toString().compare(native, Qt::CaseInsensitive) == 0)
            m_apps->setCurrentRow(i);
    }
}

void PreferencesPage::apply()
{
    const QString path = selectedPath();
    if (path.isEmpty())
        return;
    GpuPreference wanted = GpuPreference::NotSet;
    QString what;
    if (m_advanced->isChecked()) {
        const int choice = m_choice->checkedId();
        wanted = choice == 1 ? GpuPreference::LetWindowsDecide : choice == 2 ? GpuPreference::PowerSaving
               : choice == 3 ? GpuPreference::HighPerformance : GpuPreference::NotSet;
        what = gpuPreferenceText(wanted);
    } else {
        const ChoicePlan plan = planChoice(selectedChoice(), selectableGpus(), m_context.preferences().lastVerification());
        if (!plan.write) {
            m_editorNote->setText(plan.summary);
            return;
        }
        wanted = *plan.write;
        what = plan.target ? tr("%1 (stored as \"%2\")").arg(plan.target->name, gpuPreferenceText(wanted)) : gpuPreferenceText(wanted);
    }
    PreferenceManager& manager = m_context.preferences().manager();
    const std::optional<QString> before = manager.rawValue(path);
    const QString afterText = withGpuPreference(before.value_or(QString()), wanted);
    const std::optional<QString> after = afterText.isEmpty() ? std::nullopt : std::optional<QString>(afterText);
    if (before == after) {
        m_editorNote->setText(tr("Already set to %1; nothing to change.").arg(what));
        return;
    }
    QString message = tr("GPU Fusion will change one per-user Windows setting:\n\n"
                         "Key: HKEY_CURRENT_USER\\Software\\Microsoft\\DirectX\\UserGpuPreferences\nValue: %1\nFrom: %2\nTo: %3\n\n"
                         "The previous value is recorded first, so you can undo this in Change history.")
                          .arg(QDir::toNativeSeparators(path), shortValue(before), after ? *after : tr("(value removed)"));
    const auto& v = m_context.preferences().lastVerification();
    if (m_advanced->isChecked() && v && v->restored && v->preferenceHasNoEffect() &&
        (wanted == GpuPreference::PowerSaving || wanted == GpuPreference::HighPerformance))
        message += tr("\n\nNote: verification on this PC showed that this setting does not change which GPU programs use.");
    if (QMessageBox::question(this, tr("Change graphics preference"), message) != QMessageBox::Yes)
        return;

    const ChangeResult r = manager.apply(path, wanted);
    if (!r.ok) {
        QMessageBox::warning(this, tr("Change graphics preference"), r.error);
        return;
    }
    // Read back what Windows now has, rather than assuming the write did what was asked.
    const std::optional<QString> stored = manager.rawValue(path);
    const bool matches = stored == after;
    QString saved = matches ? tr("Saved and read back: %1.").arg(shortValue(stored))
                            : tr("Saved, but reading it back gave %1 instead of %2.").arg(shortValue(stored), shortValue(after));
    saved += u' ';
    saved += isRunning(path)
        ? tr("%1 is running — Windows applies the preference when it next starts, so restart it, then use \"Verify on this PC\".")
              .arg(QFileInfo(path).fileName())
        : tr("It applies the next time %1 starts. Saving does not prove which GPU it will use: \"Verify on this PC\" checks that.")
              .arg(QFileInfo(path).fileName());
    m_editorNote->setText(saved);
    Q_EMIT m_context.preferences().changed();
}

void PreferencesPage::undo(const QString& recordId)
{
    PreferenceManager& manager = m_context.preferences().manager();
    ChangeResult r = manager.rollback(recordId);
    if (!r.ok && r.error.contains(QStringLiteral("outside GPU Fusion"))) {
        if (QMessageBox::question(this, tr("Undo change"), tr("%1\n\nUndo anyway?").arg(r.error)) != QMessageBox::Yes)
            return;
        r = manager.rollback(recordId, true);
    }
    if (!r.ok)
        QMessageBox::warning(this, tr("Undo change"), r.error);
    Q_EMIT m_context.preferences().changed();
}

void PreferencesPage::launch()
{
    const QString path = selectedPath();
    QString error;
    if (!win::launchDetached(path, &error))
        QMessageBox::warning(this, tr("Launch"), tr("Could not start %1: %2").arg(path, error));
    else
        m_editorNote->setText(tr("Started %1. Open the Applications page to see which GPU it uses.").arg(QFileInfo(path).fileName()));
}

void PreferencesPage::verifyUsage()
{
    const QString path = selectedPath();
    const auto& detection = m_context.hardware().latest();
    if (path.isEmpty() || !detection || m_usageThread)
        return;
    const bool running = isRunning(path);
    QString message = tr("GPU Fusion will start:\n\n%1\n\nand watch for 20 seconds which GPU its processes (and the processes it starts) use. "
                         "Use the program normally meanwhile — for example open its main window or a 3D view — so that it does GPU work.")
                          .arg(QDir::toNativeSeparators(path));
    if (running)
        message += tr("\n\n%1 is already running. Windows applies a preference only when a program starts: close every instance first "
                      "for a meaningful result.").arg(QFileInfo(path).fileName());
    if (QMessageBox::question(this, tr("Verify on this PC"), message) != QMessageBox::Yes)
        return;

    const QList<mon::MonitorTarget> targets = mon::targetsFrom(*detection);
    mon::ObserveOptions options;
    options.exe = path;
    options.seconds = 20;
    m_cancelUsage = false;
    auto evidence = std::make_shared<mon::UsageEvidence>();
    QPointer<PreferencesPage> self(this);
    m_usageThread = QThread::create([this, self, targets, options, evidence] {
        *evidence = mon::observeUsage(targets, options, &m_cancelUsage, [self](int pass, int passes, int processes) {
            QMetaObject::invokeMethod(self, [self, pass, passes, processes] {
                if (self)
                    self->m_usageResult->setText(tr("Watching… %1 of %2 s, %3 process(es) of the program seen.").arg(pass).arg(passes).arg(processes));
            }, Qt::QueuedConnection);
        });
    });
    connect(m_usageThread, &QThread::finished, this, [this, evidence] {
        m_usageThread->deleteLater();
        m_usageThread = nullptr;
        showUsageResult(*evidence);
        refreshChoices();
    });
    m_usageResult->setText(tr("Starting %1…").arg(QFileInfo(path).fileName()));
    m_usageThread->start();
    refreshChoices();
}

void PreferencesPage::showUsageResult(const mon::UsageEvidence& observed)
{
    const QString path = selectedPath();
    const auto& detection = m_context.hardware().latest();
    if (!detection)
        return;
    bool noAdapter = false;
    const mon::UsageVerdict v = evaluateForApp(observed, m_context.preferences().manager(), *detection, &noAdapter);
    qCInfo(lcPrefs).noquote() << "Usage verification of" << QDir::toNativeSeparators(observed.targetExe) << ":" << v.headline;

    const ThemeColors& c = colors();
    QStringList html{ QStringLiteral("<b>%1</b>").arg(v.headline.toHtmlEscaped()) };
    if (noAdapter)
        html << QStringLiteral("<span style='color:%1'>The stored value (%2) does not ask for a specific GPU, so there is no adapter to "
                               "confirm — this shows where the program actually ran.</span>")
                    .arg(c.textMuted.name(), shortValue(m_context.preferences().manager().rawValue(observed.targetExe)).toHtmlEscaped());
    for (const mon::UsageStage& s : v.stages)
        html << QStringLiteral("<span style='color:%1'>&#9679;</span> %2: <b>%3</b>%4")
                    .arg(stageColor(s.state).name(), s.name.toHtmlEscaped(), mon::stageStateText(s.state),
                         s.detail.isEmpty() ? QString() : QStringLiteral(" — %1").arg(s.detail.toHtmlEscaped()));
    for (const QString& d : v.details)
        html << QStringLiteral("<span style='color:%1'>%2</span>").arg(c.warn.name(), d.toHtmlEscaped());
    m_usageResult->setText(html.join(QStringLiteral("<br>")));

    while (QLayoutItem* item = m_usageActions->takeAt(0)) {
        delete item->widget();
        delete item;
    }
    for (const QString& exe : v.renderingExecutables) {
        if (QFileInfo(exe).fileName().compare(QFileInfo(path).fileName(), Qt::CaseInsensitive) == 0 || !QFileInfo(exe).isAbsolute())
            continue;
        auto* b = new QPushButton(tr("Configure %1 instead (it does the rendering)").arg(QFileInfo(exe).fileName()));
        b->setToolTip(exe);
        connect(b, &QPushButton::clicked, this, [this, exe] { addAndSelect(exe); });
        m_usageActions->addWidget(b);
    }
}

void PreferencesPage::refreshHistory()
{
    const auto& records = m_context.preferences().journal().records();
    m_history->setRowCount(0);
    for (int i = int(records.size()) - 1; i >= 0; --i) { // newest first
        const ChangeRecord& r = records[i];
        const int row = m_history->rowCount();
        m_history->insertRow(row);
        m_history->setItem(row, 0, new QTableWidgetItem(QLocale().toString(r.time, QLocale::ShortFormat)));
        auto* app = new QTableWidgetItem(QFileInfo(r.valueName).fileName());
        app->setToolTip(r.valueName);
        m_history->setItem(row, 1, app);
        m_history->setItem(row, 2, new QTableWidgetItem(QStringLiteral("%1 → %2").arg(shortValue(r.before), shortValue(r.after))));
        if (r.rolledBack || r.origin != QLatin1StringView("user")) {
            const QString state_text = r.rolledBack ? (r.origin.contains(QLatin1StringView("failed")) ? tr("Failed") : tr("Undone"))
                                     : r.origin.startsWith(QLatin1StringView("undo")) ? tr("Undo of an earlier change")
                                                                                       : r.origin;
            auto* state = new QTableWidgetItem(state_text);
            state->setForeground(colors().textMuted);
            m_history->setItem(row, 3, state);
        } else {
            auto* button = new QPushButton(tr("Undo"));
            const QString id = r.id;
            connect(button, &QPushButton::clicked, this, [this, id] { undo(id); });
            m_history->setCellWidget(row, 3, button);
        }
    }
    if (records.isEmpty()) {
        m_history->insertRow(0);
        auto* empty = new QTableWidgetItem(tr("No changes made by GPU Fusion yet."));
        empty->setForeground(colors().textMuted);
        m_history->setItem(0, 0, empty);
        m_history->setSpan(0, 0, 1, 4);
    }
}

} // namespace gf::ui
