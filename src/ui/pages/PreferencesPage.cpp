#include "ui/pages/PreferencesPage.h"

#include "platform/windows/ProbeRunner.h"
#include "platform/windows/ProcessList.h"
#include "preferences/PreferenceService.h"
#include "ui/AppContext.h"
#include "ui/Theme.h"
#include "ui/widgets/Card.h"

#include <QButtonGroup>
#include <QDesktopServices>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QTableWidget>
#include <QUrl>
#include <QVBoxLayout>

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
    m_choice = new QButtonGroup(this);
    m_notSet = new QRadioButton(tr("Not set (remove GPU Fusion's / Windows' choice)"));
    m_letWindows = new QRadioButton(tr("Let Windows decide"));
    m_power = new QRadioButton(tr("Power saving"));
    m_performance = new QRadioButton(tr("High performance"));
    int id = 0;
    for (QRadioButton* r : { m_notSet, m_letWindows, m_power, m_performance }) {
        m_choice->addButton(r, id++);
        editor->addWidget(r);
    }
    auto* editorButtons = new QHBoxLayout;
    m_apply = new QPushButton(tr("Apply…"));
    m_apply->setDefault(true);
    m_launch = new QPushButton(tr("Launch"));
    m_launch->setToolTip(tr("Start the application normally, then check the Applications page to see which GPU it really uses"));
    auto* windowsSettings = new QPushButton(tr("Open Windows graphics settings"));
    editorButtons->addWidget(m_apply);
    editorButtons->addWidget(m_launch);
    editorButtons->addStretch();
    editorButtons->addWidget(windowsSettings);
    editor->addLayout(editorButtons);
    m_editorNote = note(QString());
    editor->addWidget(m_editorNote);
    editor->addStretch();
    appsRow->addLayout(editor, 2);
    appsCard->body()->addLayout(appsRow);
    appsCard->body()->addWidget(note(tr("A preference is a request, not a guarantee: it only affects programs that let Windows pick the GPU, "
                                        "and only when they next start. Programs that choose a GPU themselves (many games and editors have a "
                                        "setting) ignore it. Changes are per user, need no administrator rights, and can be undone below.")));
    layout->addWidget(appsCard);

    // --- History ---------------------------------------------------------------------------------
    auto* historyCard = new Card(tr("Change history"));
    historyCard->body()->addWidget(note(tr("Every change GPU Fusion makes is recorded with the previous value before Windows is touched. "
                                           "Undo puts the previous value back; it refuses if the setting was changed elsewhere since.")));
    m_history = makeTable({ tr("When"), tr("Application"), tr("Before → after"), tr("") });
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
    });
    connect(&prefs, &PreferenceService::changed, this, [this] {
        refreshApplications();
        refreshHistory();
        refreshEditor();
    });
    connect(addButton, &QPushButton::clicked, this, &PreferencesPage::addApplication);
    connect(m_apps, &QListWidget::currentRowChanged, this, &PreferencesPage::refreshEditor);
    connect(m_apply, &QPushButton::clicked, this, &PreferencesPage::apply);
    connect(m_launch, &QPushButton::clicked, this, &PreferencesPage::launch);
    connect(windowsSettings, &QPushButton::clicked, this, [] {
        QDesktopServices::openUrl(QUrl(QStringLiteral("ms-settings:display-advancedgraphics")));
    });

    refreshVerification();
    refreshApplications();
    refreshHistory();
    refreshEditor();
}

void PreferencesPage::setBusy(bool busy)
{
    // The verification thread uses the registry and the journal; no edits meanwhile.
    m_verify->setEnabled(!busy && m_context.preferences().probeAvailable());
    m_apply->setEnabled(!busy && !selectedPath().isEmpty());
    m_history->setEnabled(!busy);
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
        m_verdict->setText(tr("Not verified yet on this PC."));
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
                     "here it does not treat the other GPU as one — for example because its driver predates WDDM 2.0. "
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
    m_apply->setEnabled(has && !m_context.preferences().isVerifying());
    m_launch->setEnabled(has && QFileInfo::exists(path));
    if (!has) {
        m_selected->setText(tr("Select or add an application."));
        m_editorNote->clear();
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
}

void PreferencesPage::addApplication()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose an application"), QString(), tr("Programs (*.exe)"));
    if (path.isEmpty())
        return;
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
    const int choice = m_choice->checkedId();
    const GpuPreference wanted = choice == 1 ? GpuPreference::LetWindowsDecide : choice == 2 ? GpuPreference::PowerSaving
                               : choice == 3 ? GpuPreference::HighPerformance : GpuPreference::NotSet;
    PreferenceManager& manager = m_context.preferences().manager();
    const std::optional<QString> before = manager.rawValue(path);
    const QString afterText = withGpuPreference(before.value_or(QString()), wanted);
    const std::optional<QString> after = afterText.isEmpty() ? std::nullopt : std::optional<QString>(afterText);
    if (before == after) {
        m_editorNote->setText(tr("Already set to %1; nothing to change.").arg(gpuPreferenceText(wanted)));
        return;
    }
    QString message = tr("GPU Fusion will change one per-user Windows setting:\n\n"
                         "Key: HKEY_CURRENT_USER\\Software\\Microsoft\\DirectX\\UserGpuPreferences\nValue: %1\nFrom: %2\nTo: %3\n\n"
                         "The previous value is recorded first, so you can undo this in Change history.")
                          .arg(QDir::toNativeSeparators(path), shortValue(before), after ? *after : tr("(value removed)"));
    const auto& v = m_context.preferences().lastVerification();
    if (v && v->restored && v->preferenceHasNoEffect() && (wanted == GpuPreference::PowerSaving || wanted == GpuPreference::HighPerformance))
        message += tr("\n\nNote: verification on this PC showed that this setting does not change which GPU programs use.");
    if (QMessageBox::question(this, tr("Change graphics preference"), message) != QMessageBox::Yes)
        return;

    const ChangeResult r = manager.apply(path, wanted);
    if (!r.ok) {
        QMessageBox::warning(this, tr("Change graphics preference"), r.error);
        return;
    }
    m_editorNote->setText(isRunning(path)
        ? tr("Saved. %1 is running — Windows applies the preference when it next starts, so restart it.").arg(QFileInfo(path).fileName())
        : tr("Saved. It applies the next time %1 starts. Use Launch, then the Applications page, to see which GPU it actually uses.")
              .arg(QFileInfo(path).fileName()));
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
