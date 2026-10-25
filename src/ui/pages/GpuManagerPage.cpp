#include "ui/pages/GpuManagerPage.h"

#include "core/Logging.h"
#include "hardware/HardwareService.h"
#include "monitoring/UsageVerification.h"
#include "preferences/PreferenceModel.h"
#include "preferences/PreferenceService.h"
#include "profiles/AppDiscovery.h"
#include "profiles/ProfileActions.h"
#include "profiles/ProfileStore.h"
#include "ui/AppContext.h"
#include "ui/Theme.h"
#include "ui/widgets/Card.h"

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRadioButton>
#include <QScrollArea>
#include <QThread>
#include <QVBoxLayout>

#include <memory>

namespace gf::ui {

using profiles::AppProfile;
using profiles::GpuMode;
using profiles::ModeCapability;
using profiles::ModeStatus;

namespace {

constexpr GpuMode kModes[4] = { GpuMode::Automatic, GpuMode::Integrated, GpuMode::Discrete, GpuMode::Both };

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

QColor statusColor(ModeStatus s)
{
    const ThemeColors& c = colors();
    switch (s) {
    case ModeStatus::Verified:        return c.good;
    case ModeStatus::Expected:        return c.accent;
    case ModeStatus::NeedsAppSupport:
    case ModeStatus::NeedsUserAction: return c.warn;
    case ModeStatus::Unsupported:     return c.bad;
    case ModeStatus::Unknown:         return c.textMuted;
    }
    return c.textMuted;
}

QString shortValue(const std::optional<QString>& v) { return v && !v->isEmpty() ? *v : QObject::tr("not set"); }

QString stageHtml(const QString& stage)
{
    // "<name>: <state> — <detail>" as recorded in a profile
    const ThemeColors& c = colors();
    QColor dot = c.textMuted;
    if (stage.contains(QStringLiteral(": Passed")))
        dot = c.good;
    else if (stage.contains(QStringLiteral(": Failed")))
        dot = c.bad;
    else if (stage.contains(QStringLiteral(": Inconclusive")))
        dot = c.warn;
    return QStringLiteral("<span style='color:%1'>&#9679;</span> %2").arg(dot.name(), stage.toHtmlEscaped());
}

} // namespace

GpuManagerPage::GpuManagerPage(AppContext& context, QWidget* parent) : QWidget(parent), m_context(context)
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

    // --- programs ---------------------------------------------------------------------------------
    auto* listCard = new Card(tr("Programs"));
    listCard->body()->addWidget(note(tr("Any program can have a GPU profile. Pick one from the programs installed or running on this PC, or "
                                        "browse to its .exe. Each profile is saved separately in the data folder.")));
    auto* row = new QHBoxLayout;
    m_list = new QListWidget;
    m_list->setAccessibleName(tr("Program profiles"));
    m_list->setMinimumHeight(160);
    row->addWidget(m_list, 1);
    auto* buttons = new QVBoxLayout;
    auto* pick = new QPushButton(tr("Add program…"));
    pick->setToolTip(tr("Choose from installed and running programs"));
    auto* browseButton = new QPushButton(tr("Browse…"));
    m_remove = new QPushButton(tr("Remove profile"));
    m_remove->setToolTip(tr("Deletes GPU Fusion's profile. A Windows preference it stored stays until you set the mode to Automatic first."));
    buttons->addWidget(pick);
    buttons->addWidget(browseButton);
    buttons->addWidget(m_remove);
    buttons->addStretch(1);
    row->addLayout(buttons);
    listCard->body()->addLayout(row);
    layout->addWidget(listCard);

    // --- profile editor -----------------------------------------------------------------------------
    auto* editorCard = new Card(tr("GPU for this program"));
    m_editor = new QWidget;
    auto* editor = new QVBoxLayout(m_editor);
    editor->setContentsMargins(0, 0, 0, 0);
    m_header = richLabel();
    editor->addWidget(m_header);
    m_modes = new QButtonGroup(this);
    for (int i = 0; i < 4; ++i) {
        m_modeButtons[i] = new QRadioButton(profiles::modeText(kModes[i]));
        m_modes->addButton(m_modeButtons[i], i);
        editor->addWidget(m_modeButtons[i]);
    }
    m_capability = richLabel();
    editor->addWidget(m_capability);
    auto* argsRow = new QHBoxLayout;
    argsRow->addWidget(new QLabel(tr("Launch arguments:")));
    m_arguments = new QLineEdit;
    m_arguments->setPlaceholderText(tr("optional, used when GPU Fusion starts the program"));
    argsRow->addWidget(m_arguments, 1);
    editor->addLayout(argsRow);
    m_shareChoice = new QCheckBox(tr("Tell the program which GPU to use when GPU Fusion starts it (GPU Fusion SDK)"));
    m_shareChoice->setToolTip(tr("Sets GPUFUSION_MODE, GPUFUSION_ADAPTERS and GPUFUSION_PROFILE for the program. Programs that do not "
                                 "integrate the SDK ignore them."));
    editor->addWidget(m_shareChoice);
    auto* actions = new QHBoxLayout;
    m_apply = new QPushButton(tr("Save and apply"));
    m_launch = new QPushButton(tr("Launch"));
    m_launch->setToolTip(tr("Start the program from its profile (with its arguments and GPU choice)"));
    m_verify = new QPushButton(tr("Verify on this PC"));
    m_verify->setToolTip(tr("Start the program from its profile and observe which GPU(s) its processes really use"));
    actions->addWidget(m_apply);
    actions->addWidget(m_launch);
    actions->addWidget(m_verify);
    actions->addStretch(1);
    editor->addLayout(actions);
    m_result = richLabel();
    editor->addWidget(m_result);
    editorCard->body()->addWidget(m_editor);
    layout->addWidget(editorCard);

    // --- what the modes mean ---------------------------------------------------------------------
    auto* aboutCard = new Card(tr("What GPU Fusion can and cannot do"));
    aboutCard->body()->addWidget(note(
        tr("<b>A – choose the GPU a program renders on:</b> Windows' per-app preference where it works on this PC (verified with the probe), "
           "otherwise the choice is passed to the program when GPU Fusion starts it.<br>"
           "<b>B – different GPUs for different programs:</b> one profile per program.<br>"
           "<b>C – independent work on both GPUs at once:</b> separate programs or jobs, one per GPU.<br>"
           "<b>D – one program dividing its own work between GPUs:</b> only programs built for it, e.g. with the GPU Fusion SDK.<br>"
           "<b>E – one rendering workload split across GPUs:</b> needs Direct3D 12 on every GPU and the program's own support; "
           "GPU Fusion cannot add it to a program.<br>"
           "Both GPUs showing activity does not by itself mean a program got faster. GPU Fusion never injects code into or patches programs.")));
    layout->addWidget(aboutCard);
    layout->addStretch(1);

    connect(pick, &QPushButton::clicked, this, &GpuManagerPage::pickDiscovered);
    connect(browseButton, &QPushButton::clicked, this, &GpuManagerPage::browse);
    connect(m_remove, &QPushButton::clicked, this, &GpuManagerPage::removeSelected);
    connect(m_list, &QListWidget::currentRowChanged, this, [this] { refreshEditor(); });
    connect(m_modes, &QButtonGroup::idClicked, this, [this] { refreshCapability(); });
    connect(m_apply, &QPushButton::clicked, this, [this] { applySelected(true); });
    connect(m_launch, &QPushButton::clicked, this, &GpuManagerPage::launchSelected);
    connect(m_verify, &QPushButton::clicked, this, [this] { verifySelected(20, true); });
    connect(&m_context.hardware(), &hw::HardwareService::detectionFinished, this, [this] { refreshEditor(); });
    connect(&m_context.preferences(), &pref::PreferenceService::changed, this, [this] { refreshCapability(); });

    refreshList();
}

GpuManagerPage::~GpuManagerPage()
{
    m_cancel = true;
    if (m_thread)
        m_thread->wait();
}

// ---- list -----------------------------------------------------------------------------------------

void GpuManagerPage::refreshList()
{
    const QString current = m_list->currentItem() ? m_list->currentItem()->data(Qt::UserRole).toString() : QString();
    QSignalBlocker block(m_list);
    m_list->clear();
    if (m_context.profiles()) {
        for (const AppProfile& p : m_context.profiles()->list()) {
            QString line = QStringLiteral("%1 — %2").arg(p.name, profiles::modeText(p.mode));
            if (p.lastVerification && p.lastVerification->mode == p.mode)
                line += p.lastVerification->confirmed ? tr("  (verified)")
                      : p.mode == GpuMode::Automatic && p.lastVerification->outcome == QLatin1StringView("observed") ? tr("  (checked)")
                                                                                                                       : tr("  (last check: %1)").arg(p.lastVerification->outcome);
            auto* item = new QListWidgetItem(line);
            item->setData(Qt::UserRole, p.id);
            item->setToolTip(p.exePath);
            m_list->addItem(item);
            if (p.id == current)
                m_list->setCurrentItem(item);
        }
    }
    if (!m_list->currentItem() && m_list->count() > 0)
        m_list->setCurrentRow(0);
    refreshEditor();
}

std::optional<AppProfile> GpuManagerPage::selected() const
{
    if (!m_context.profiles() || !m_list->currentItem())
        return std::nullopt;
    return m_context.profiles()->find(m_list->currentItem()->data(Qt::UserRole).toString());
}

bool GpuManagerPage::saveProfile(AppProfile& p)
{
    QString error;
    if (!m_context.profiles() || !m_context.profiles()->save(p, &error)) {
        QMessageBox::warning(this, tr("GPU profile"), tr("The profile could not be saved: %1").arg(error));
        return false;
    }
    return true;
}

bool GpuManagerPage::addProgram(const QString& exePath, const QString& name)
{
    if (!m_context.profiles())
        return false;
    QString why;
    if (!pref::isConfigurableExecutable(exePath, &why)) {
        m_result->setText(why.toHtmlEscaped());
        return false;
    }
    AppProfile p = m_context.profiles()->find(exePath).value_or(profiles::newProfile(exePath, name));
    if (!m_context.profiles()->find(p.id) && !saveProfile(p))
        return false;
    refreshList();
    for (int i = 0; i < m_list->count(); ++i) {
        if (m_list->item(i)->data(Qt::UserRole).toString() == p.id)
            m_list->setCurrentRow(i);
    }
    return true;
}

void GpuManagerPage::pickDiscovered()
{
    QDialog dialog(this);
    dialog.setWindowTitle(tr("Add program"));
    dialog.resize(640, 480);
    auto* v = new QVBoxLayout(&dialog);
    v->addWidget(note(tr("Programs found in the Start menu, the installed-programs list and running processes. Nothing is started or "
                         "changed by this list.")));
    auto* filter = new QLineEdit;
    filter->setPlaceholderText(tr("Filter"));
    filter->setClearButtonEnabled(true);
    v->addWidget(filter);
    auto* list = new QListWidget;
    v->addWidget(list, 1);
    auto* box = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    v->addWidget(box);

    QApplication::setOverrideCursor(Qt::WaitCursor);
    const QList<profiles::DiscoveredApp> apps = profiles::discoverApps();
    QApplication::restoreOverrideCursor();
    for (const profiles::DiscoveredApp& a : apps) {
        auto* item = new QListWidgetItem(QStringLiteral("%1   (%2)").arg(a.name, a.source));
        item->setToolTip(a.exePath);
        item->setData(Qt::UserRole, a.exePath);
        item->setData(Qt::UserRole + 1, a.name);
        list->addItem(item);
    }
    list->sortItems();
    connect(filter, &QLineEdit::textChanged, list, [list](const QString& text) {
        for (int i = 0; i < list->count(); ++i) {
            QListWidgetItem* item = list->item(i);
            item->setHidden(!text.isEmpty() && !item->text().contains(text, Qt::CaseInsensitive) &&
                            !item->toolTip().contains(text, Qt::CaseInsensitive));
        }
    });
    connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
    connect(box, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(box, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    if (dialog.exec() != QDialog::Accepted || !list->currentItem())
        return;
    addProgram(list->currentItem()->data(Qt::UserRole).toString(), list->currentItem()->data(Qt::UserRole + 1).toString());
}

void GpuManagerPage::browse()
{
    const QString path = QFileDialog::getOpenFileName(this, tr("Choose a program"), QString(), tr("Programs (*.exe)"));
    if (!path.isEmpty())
        addProgram(QDir::toNativeSeparators(path));
}

void GpuManagerPage::removeSelected()
{
    const auto p = selected();
    if (!p)
        return;
    QString message = tr("Remove GPU Fusion's profile for %1?").arg(p->name);
    const std::optional<QString> stored = m_context.preferences().manager().rawValue(profiles::resolveCurrentExecutable(p->exePath));
    if (stored && pref::parseGpuPreference(*stored) != pref::GpuPreference::NotSet)
        message += tr("\n\nThe Windows preference it stored (%1) is kept. Set the mode to Automatic and apply first to remove it.").arg(*stored);
    if (QMessageBox::question(this, tr("Remove profile"), message) != QMessageBox::Yes)
        return;
    QString error;
    if (!m_context.profiles()->remove(p->id, &error))
        QMessageBox::warning(this, tr("Remove profile"), error);
    refreshList();
}

// ---- editor ---------------------------------------------------------------------------------------

profiles::CapabilityInputs GpuManagerPage::inputsFor(const AppProfile& p) const
{
    const auto& d = m_context.hardware().latest();
    if (!d)
        return {};
    return profiles::capabilityInputs(p.exePath, *d, m_context.preferences().lastVerification());
}

GpuMode GpuManagerPage::chosenMode() const
{
    const int id = m_modes->checkedId();
    return id >= 0 && id < 4 ? kModes[id] : GpuMode::Automatic;
}

void GpuManagerPage::selectMode(GpuMode mode)
{
    for (int i = 0; i < 4; ++i) {
        if (kModes[i] == mode)
            m_modeButtons[i]->setChecked(true);
    }
    refreshCapability();
}

void GpuManagerPage::refreshEditor()
{
    const auto p = selected();
    m_editor->setEnabled(p.has_value());
    m_remove->setEnabled(p.has_value() && !m_thread);
    if (!p) {
        m_header->setText(m_context.profiles() ? tr("Add a program to choose its GPU.") : tr("Profiles are not available."));
        m_capability->clear();
        m_result->clear();
        return;
    }
    m_loading = true;
    const QString current = profiles::resolveCurrentExecutable(p->exePath);
    QString header = QStringLiteral("<b>%1</b><br><span style='color:%2'>%3</span>")
                         .arg(p->name.toHtmlEscaped(), colors().textMuted.name(), p->exePath.toHtmlEscaped());
    if (!QFileInfo::exists(current))
        header += QStringLiteral("<br><span style='color:%1'>%2</span>").arg(colors().bad.name(), tr("The program is no longer there.").toHtmlEscaped());
    else if (current != p->exePath)
        header += QStringLiteral("<br><span style='color:%1'>%2</span>")
                      .arg(colors().textMuted.name(), tr("A newer version is installed; changes apply to %1").arg(current).toHtmlEscaped());
    m_header->setText(header);
    m_arguments->setText(p->arguments);
    m_shareChoice->setChecked(p->shareGpuChoiceWithApp);
    m_loading = false;
    selectMode(p->mode);
    showLastVerification(*p);
    setBusy(m_thread != nullptr);
}

void GpuManagerPage::refreshCapability()
{
    const auto p = selected();
    if (!p || m_loading)
        return;
    const profiles::CapabilityInputs in = inputsFor(*p);
    const QList<ModeCapability> caps = profiles::assessAllModes(*p, in);
    for (int i = 0; i < 4 && i < caps.size(); ++i)
        m_modeButtons[i]->setText(tr("%1 — %2").arg(profiles::modeText(kModes[i]), profiles::modeStatusText(caps[i].status)));
    const ModeCapability& c = caps.value(m_modes->checkedId() < 0 ? 0 : m_modes->checkedId());
    const ThemeColors& t = colors();
    QStringList html{ QStringLiteral("<span style='color:%1'>&#9679;</span> <b>%2</b> %3")
                          .arg(statusColor(c.status).name(), profiles::modeStatusText(c.status).toHtmlEscaped(), c.summary.toHtmlEscaped()) };
    if (!c.capabilityClass.isEmpty())
        html << QStringLiteral("<span style='color:%1'>Capability class %2. How: %3</span>")
                    .arg(t.textMuted.name(), c.capabilityClass, (c.mechanism.isEmpty() ? tr("nothing to apply") : c.mechanism).toHtmlEscaped());
    for (const QString& r : c.reasons)
        html << QStringLiteral("<span style='color:%1'>• %2</span>").arg(t.textMuted.name(), r.toHtmlEscaped());
    const std::optional<QString> stored = m_context.preferences().manager().rawValue(profiles::resolveCurrentExecutable(p->exePath));
    html << QStringLiteral("<span style='color:%1'>Windows value now: %2</span>").arg(t.textMuted.name(), shortValue(stored).toHtmlEscaped());
    m_capability->setText(html.join(QStringLiteral("<br>")));
}

QString GpuManagerPage::capabilityText() const { return m_capability->text(); }
QString GpuManagerPage::resultText() const { return m_result->text(); }

void GpuManagerPage::setBusy(bool busy)
{
    const auto p = selected();
    const bool exists = p && QFileInfo::exists(profiles::resolveCurrentExecutable(p->exePath));
    m_apply->setEnabled(!busy && exists);
    m_launch->setEnabled(!busy && exists);
    m_verify->setEnabled(!busy && exists && m_context.hardware().latest().has_value());
    m_remove->setEnabled(!busy && p.has_value());
    m_list->setEnabled(!busy);
}

// ---- actions --------------------------------------------------------------------------------------

void GpuManagerPage::applySelected(bool confirm)
{
    auto p = selected();
    if (!p)
        return;
    p->mode = chosenMode();
    p->arguments = m_arguments->text().trimmed();
    p->shareGpuChoiceWithApp = m_shareChoice->isChecked();
    const profiles::CapabilityInputs in = inputsFor(*p);
    const ModeCapability cap = profiles::assessMode(*p, p->mode, in);
    pref::PreferenceManager& manager = m_context.preferences().manager();
    const QString exe = profiles::resolveCurrentExecutable(p->exePath);
    const std::optional<QString> before = manager.rawValue(exe);
    const QString afterText = pref::withGpuPreference(before.value_or(QString()), cap.write.value_or(pref::GpuPreference::NotSet));
    const std::optional<QString> after = afterText.isEmpty() ? std::nullopt : std::optional<QString>(afterText);
    if (confirm && before != after) {
        const QString message = tr("Mode: %1\n\nGPU Fusion will change one per-user Windows setting:\n\n"
                                   "Key: HKEY_CURRENT_USER\\Software\\Microsoft\\DirectX\\UserGpuPreferences\nValue: %2\nFrom: %3\nTo: %4\n\n"
                                   "The previous value is recorded first, so you can undo this on the GPU Preferences page.")
                                    .arg(profiles::modeText(p->mode), exe, shortValue(before), after ? *after : tr("(value removed)"));
        if (QMessageBox::question(this, tr("Apply GPU profile"), message) != QMessageBox::Yes)
            return;
    }
    const profiles::ApplyResult r = profiles::applyProfile(*p, manager, in);
    if (!r.ok) {
        m_result->setText(QStringLiteral("<span style='color:%1'>%2</span>").arg(colors().bad.name(), r.message.toHtmlEscaped()));
        return;
    }
    if (!saveProfile(*p))
        return;
    Q_EMIT m_context.preferences().changed();
    refreshList();
    m_result->setText(tr("%1<br>Saved. A Windows preference takes effect when the program next starts; \"Verify on this PC\" checks which "
                         "GPU it really uses.").arg(r.message.toHtmlEscaped()));
}

void GpuManagerPage::launchSelected()
{
    const auto p = selected();
    const auto& d = m_context.hardware().latest();
    if (!p || !d)
        return;
    QString error;
    const auto pid = profiles::launchProfile(*p, profiles::selectableGpus(*d), &error);
    m_result->setText(pid ? tr("Started %1 (process %2) in mode %3.").arg(p->name.toHtmlEscaped()).arg(*pid).arg(profiles::modeText(p->mode))
                          : tr("Could not start %1: %2").arg(p->name, error).toHtmlEscaped());
}

void GpuManagerPage::verifySelected(int seconds, bool confirm)
{
    auto p = selected();
    const auto& d = m_context.hardware().latest();
    if (!p || !d || m_thread)
        return;
    if (p->mode != chosenMode() || p->arguments != m_arguments->text().trimmed() || p->shareGpuChoiceWithApp != m_shareChoice->isChecked()) {
        m_result->setText(tr("Save and apply the changed settings first, so the program is checked the way it will run."));
        return;
    }
    if (confirm) {
        const QString message = tr("GPU Fusion will start:\n\n%1 %2\n\nin mode %3 and watch for %4 seconds which GPU(s) its processes (and "
                                   "the processes it starts) use. Use the program normally meanwhile so that it does GPU work. Close any "
                                   "running instance first for a meaningful result.")
                                    .arg(profiles::resolveCurrentExecutable(p->exePath), p->arguments, profiles::modeText(p->mode)).arg(seconds);
        if (QMessageBox::question(this, tr("Verify on this PC"), message) != QMessageBox::Yes)
            return;
    }
    const hw::DetectionResult detection = *d;
    const std::optional<QString> stored = m_context.preferences().manager().rawValue(profiles::resolveCurrentExecutable(p->exePath));
    auto profile = std::make_shared<AppProfile>(*p);
    auto verdict = std::make_shared<mon::UsageVerdict>();
    m_cancel = false;
    QPointer<GpuManagerPage> self(this);
    m_thread = QThread::create([this, self, profile, verdict, detection, stored, seconds] {
        *verdict = profiles::verifyProfile(*profile, detection, stored, seconds, &m_cancel, [self](int pass, int passes, int processes) {
            QMetaObject::invokeMethod(self, [self, pass, passes, processes] {
                if (self)
                    self->m_result->setText(tr("Watching… %1 of %2 s, %3 process(es) of the program seen.").arg(pass).arg(passes).arg(processes));
            }, Qt::QueuedConnection);
        });
    });
    connect(m_thread, &QThread::finished, this, [this, profile, verdict] {
        m_thread->deleteLater();
        m_thread = nullptr;
        qCInfo(lcPrefs).noquote() << "Profile verification of" << profile->name << profiles::modeKey(profile->mode) << ":" << verdict->headline;
        // Keep the user's latest saved settings; only record the verification.
        AppProfile latest = m_context.profiles()->find(profile->id).value_or(*profile);
        latest.lastVerification = profile->lastVerification;
        saveProfile(latest);
        refreshList();
        showVerdict(latest, *verdict);
        Q_EMIT verificationFinished();
    });
    m_result->setText(tr("Starting %1…").arg(p->name.toHtmlEscaped()));
    setBusy(true);
    m_thread->start();
}

void GpuManagerPage::showVerdict(const AppProfile& p, const mon::UsageVerdict& v)
{
    const ThemeColors& c = colors();
    QStringList html{ QStringLiteral("<b>%1</b>").arg(v.headline.toHtmlEscaped()) };
    if (p.lastVerification)
        for (const QString& s : p.lastVerification->stages)
            html << stageHtml(s);
    for (const QString& d : v.details)
        html << QStringLiteral("<span style='color:%1'>%2</span>").arg(c.warn.name(), d.toHtmlEscaped());
    for (const QString& exe : v.renderingExecutables) {
        if (QFileInfo(exe).fileName().compare(QFileInfo(p.exePath).fileName(), Qt::CaseInsensitive) != 0)
            html << QStringLiteral("<span style='color:%1'>%2</span>")
                        .arg(c.textMuted.name(), tr("GPU work came from %1 — add a profile for it if it should use another GPU.").arg(exe).toHtmlEscaped());
    }
    m_result->setText(html.join(QStringLiteral("<br>")));
}

void GpuManagerPage::showLastVerification(const AppProfile& p)
{
    if (!p.lastVerification) {
        m_result->setText(QStringLiteral("<span style='color:%1'>%2</span>")
                              .arg(colors().textMuted.name(), tr("Not verified yet on this PC.").toHtmlEscaped()));
        return;
    }
    const profiles::ProfileVerification& v = *p.lastVerification;
    QStringList html{ QStringLiteral("<b>%1</b> <span style='color:%2'>(%3, mode %4)</span>")
                          .arg(v.headline.toHtmlEscaped(), colors().textMuted.name(), v.time.toString(QStringLiteral("yyyy-MM-dd HH:mm")),
                               profiles::modeText(v.mode)) };
    for (const QString& s : v.stages)
        html << stageHtml(s);
    m_result->setText(html.join(QStringLiteral("<br>")));
}

} // namespace gf::ui
