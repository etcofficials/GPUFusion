#include "ui/pages/GamesPage.h"

#include "hardware/HardwareService.h"
#include "preferences/PreferenceService.h"
#include "profiles/CompatEvidence.h"
#include "profiles/KnownOptions.h"
#include "profiles/ProfileStore.h"
#include "ui/AppContext.h"
#include "ui/widgets/Card.h"

#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QSpinBox>
#include <QTextBrowser>
#include <QUrl>
#include <QVBoxLayout>

namespace gf::ui {

namespace {
QLabel* muted(const QString& text)
{
    auto* l = new QLabel(text);
    l->setObjectName(QStringLiteral("muted"));
    l->setWordWrap(true);
    return l;
}
const QString kVariant = QStringLiteral("Intel GPU (game check)");
} // namespace

GamesPage::GamesPage(AppContext& context, QWidget* parent)
    : QWidget(parent), m_context(context), m_cancel(std::make_shared<std::atomic<bool>>(false))
{
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(0, 0, 0, 0);
    auto* scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    auto* content = new QWidget;
    auto* layout = new QVBoxLayout(content);
    scroll->setWidget(content);
    outer->addWidget(scroll);

    auto* card = new Card(tr("Check a game"));
    card->body()->addWidget(muted(tr("Start your game (any program that renders) and play or let it run, then check it here. For the chosen time GPU "
                                     "Fusion watches the game and every other program: which use the game's GPU, whether each could move to the "
                                     "other GPU and how, and what the CPU and GPU loads indicate. GPU use and CPU load are estimates from Windows' "
                                     "counters; frame times are measured (optional, through the Windows administrator prompt). Nothing is started, "
                                     "closed or changed by a check.")));
    auto* form = new QFormLayout;
    auto* gameRow = new QHBoxLayout;
    m_game = new QComboBox;
    m_game->setAccessibleName(tr("Game to check"));
    m_game->setMinimumWidth(320);
    m_refresh = new QPushButton(tr("Refresh list"));
    gameRow->addWidget(m_game, 1);
    gameRow->addWidget(m_refresh);
    form->addRow(tr("Game"), gameRow);
    m_seconds = new QSpinBox;
    m_seconds->setRange(5, 600);
    m_seconds->setValue(20);
    m_seconds->setSuffix(tr(" s"));
    m_seconds->setAccessibleName(tr("Check length in seconds"));
    form->addRow(tr("Length"), m_seconds);
    m_frames = new QCheckBox(tr("Also measure its frame times (asks for administrator rights)"));
    form->addRow(QString(), m_frames);
    m_label = new QLineEdit;
    m_label->setPlaceholderText(tr("for example: before, after"));
    m_label->setAccessibleName(tr("Check label"));
    form->addRow(tr("Label"), m_label);
    m_compare = new QComboBox;
    m_compare->setAccessibleName(tr("Compare with an earlier check"));
    form->addRow(tr("Compare with"), m_compare);
    card->body()->addLayout(form);
    auto* buttons = new QHBoxLayout;
    m_start = new QPushButton(tr("Check"));
    m_start->setDefault(true);
    m_stop = new QPushButton(tr("Stop"));
    m_stop->setEnabled(false);
    m_openReport = new QPushButton(tr("Open report"));
    m_openReport->setEnabled(false);
    buttons->addWidget(m_start);
    buttons->addWidget(m_stop);
    buttons->addWidget(m_openReport);
    buttons->addStretch();
    card->body()->addLayout(buttons);
    m_progress = new QProgressBar;
    m_progress->setAccessibleName(tr("Check progress"));
    m_progress->hide();
    m_status = new QLabel;
    m_status->setWordWrap(true);
    card->body()->addWidget(m_progress);
    card->body()->addWidget(m_status);
    layout->addWidget(card);

    auto* results = new Card(tr("Result"));
    m_result = new QTextBrowser;
    m_result->setAccessibleName(tr("Check result"));
    m_result->setMinimumHeight(260);
    m_result->setOpenLinks(false);
    results->body()->addWidget(m_result);
    layout->addWidget(results);

    auto* actions = new Card(tr("Move a competing program to the other GPU"));
    actions->body()->addWidget(muted(tr("For a program that competes for the game's GPU and has a known way to run on the Intel GPU, GPU Fusion can "
                                        "create a profile variant that starts it there. It takes effect when you next start the program from GPU "
                                        "Fusion (GPU Manager → Launch). GPU Fusion does not close or restart the program, and does not change "
                                        "Windows settings or your own profiles. Undo removes the variants created here.")));
    auto* actionRow = new QHBoxLayout;
    m_competitor = new QComboBox;
    m_competitor->setAccessibleName(tr("Competing program"));
    m_prepare = new QPushButton(tr("Create an Intel-GPU variant"));
    m_undo = new QPushButton(tr("Undo variants created here"));
    actionRow->addWidget(m_competitor, 1);
    actionRow->addWidget(m_prepare);
    actionRow->addWidget(m_undo);
    actions->body()->addLayout(actionRow);
    layout->addWidget(actions);
    layout->addStretch();

    connect(m_refresh, &QPushButton::clicked, this, &GamesPage::refreshGames);
    connect(m_game, &QComboBox::currentIndexChanged, this, &GamesPage::reloadEarlier);
    connect(m_start, &QPushButton::clicked, this, &GamesPage::start);
    connect(m_stop, &QPushButton::clicked, this, [this] {
        m_cancel->store(true);
        m_stop->setEnabled(false);
    });
    connect(m_openReport, &QPushButton::clicked, this, [this] { QDesktopServices::openUrl(QUrl::fromLocalFile(m_report)); });
    connect(m_prepare, &QPushButton::clicked, this, &GamesPage::prepareProfile);
    connect(m_undo, &QPushButton::clicked, this, &GamesPage::undoProfiles);
    m_game->addItem(tr("Click “Refresh list” to find programs using a GPU"), 0u);
    updateActions();
}

GamesPage::~GamesPage()
{
    m_cancel->store(true);
    if (m_worker.joinable())
        m_worker.join();
}

QString GamesPage::gamesDir() const { return QDir(m_context.data().root).filePath(QStringLiteral("games")); }
QString GamesPage::journalFile() const { return QDir(m_context.data().configDir()).filePath(QStringLiteral("game-check-variants.json")); }

std::function<compat::Assessment(const QString&)> GamesPage::assessor()
{
    const auto detection = m_context.hardware().latest();
    std::optional<bool> reaches;
    if (const auto& v = m_context.preferences().lastVerification(); v && v->preferenceHasNoEffect())
        reaches = false;
    const compat::SystemFacts system = detection ? compat::systemFacts(*detection, reaches) : compat::SystemFacts{};
    const QList<compat::Entry> entries = compat::loadEntries(QDir(m_context.data().configDir()).filePath(QStringLiteral("compatibility.json")));
    const QList<profiles::AppProfile> list = m_context.profiles() ? m_context.profiles()->list() : QList<profiles::AppProfile>{};
    return [system, entries, list](const QString& exe) { return compat::assess(compat::examine(exe), system, entries, profiles::evidenceFor(exe, list)); };
}

void GamesPage::refreshGames()
{
    const auto detection = m_context.hardware().latest();
    if (!detection || m_worker.joinable())
        return;
    m_refresh->setEnabled(false);
    m_status->setText(tr("Looking for programs that use a GPU…"));
    QPointer<GamesPage> self(this);
    const QList<mon::MonitorTarget> targets = mon::targetsFrom(*detection);
    std::thread([self, targets] {
        const QList<games::ProcessGpu> list = games::sampleGpuUse(targets, 1000);
        QMetaObject::invokeMethod(qApp, [self, list] {
            if (!self)
                return;
            self->m_game->clear();
            for (const games::ProcessGpu& g : list)
                if (g.meanPercent >= 0.5 && !g.path.isEmpty())
                    self->m_game->addItem(QStringLiteral("%1 (process %2) — %3 % of %4").arg(g.name).arg(g.pid).arg(g.meanPercent, 0, 'f', 0).arg(g.adapter),
                                          QVariantList{ g.pid, g.name, g.path });
            if (self->m_game->count() == 0)
                self->m_game->addItem(tr("No program is using a GPU right now"), QVariant());
            self->m_status->clear();
            self->m_refresh->setEnabled(true);
            self->updateActions();
        });
    }).detach();
}

void GamesPage::reloadEarlier()
{
    m_compare->clear();
    m_compare->addItem(tr("Nothing (just check)"), QString());
    m_earlier.clear();
    const QVariantList d = m_game->currentData().toList();
    const QString name = d.value(1).toString();
    for (const games::GameCheck& c : games::loadChecks(gamesDir())) {
        if (name.isEmpty() || c.name.compare(name, Qt::CaseInsensitive) != 0 || !c.error.isEmpty())
            continue;
        m_earlier << c;
        m_compare->addItem(QStringLiteral("%1%2").arg(c.created.toString(QStringLiteral("yyyy-MM-dd HH:mm:ss")),
                                                      c.label.isEmpty() ? QString() : QStringLiteral(" — %1").arg(c.label)),
                           c.id);
    }
    if (!m_earlier.isEmpty())
        m_compare->setCurrentIndex(1);
    updateActions();
}

void GamesPage::updateActions()
{
    const bool running = m_worker.joinable();
    m_start->setEnabled(!running && m_game->currentData().toList().size() == 3);
    m_refresh->setEnabled(!running);
    m_prepare->setEnabled(!running && m_competitor->currentData().toString().size() > 0 && m_context.profiles());
    QFile f(journalFile());
    m_undo->setEnabled(!running && f.exists() && m_context.profiles());
}

void GamesPage::start()
{
    const QVariantList d = m_game->currentData().toList();
    const auto detection = m_context.hardware().latest();
    if (d.size() != 3 || !detection)
        return;
    if (m_worker.joinable())
        m_worker.join();
    m_cancel->store(false);
    games::CheckOptions o;
    o.pid = d[0].toUInt();
    o.name = d[1].toString();
    o.path = d[2].toString();
    o.seconds = m_seconds->value();
    o.label = m_label->text().trimmed();
    o.measureFrames = m_frames->isChecked();
    o.helperPath = QDir(QCoreApplication::applicationDirPath()).filePath(QStringLiteral("GPUFusionFrameCapture.exe"));
    o.workDir = m_context.data().tempDir();
    m_progress->setValue(0);
    m_progress->show();
    m_stop->setEnabled(true);
    m_status->setText(o.measureFrames ? tr("Windows will ask for administrator rights for the frame-time capture…") : tr("Checking…"));
    QPointer<GamesPage> self(this);
    auto cancel = m_cancel;
    const QList<mon::MonitorTarget> targets = mon::targetsFrom(*detection);
    const auto assess = assessor();
    m_worker = std::thread([self, cancel, targets, o, assess] {
        auto progress = [self](const QString& text, int percent) {
            QMetaObject::invokeMethod(qApp, [self, text, percent] {
                if (self) {
                    self->m_status->setText(text);
                    self->m_progress->setValue(percent);
                }
            });
        };
        const games::GameCheck c = games::runCheck(targets, o, assess, cancel.get(), progress);
        QMetaObject::invokeMethod(qApp, [self, c] {
            if (self)
                self->finished(c);
        });
    });
    updateActions();
}

void GamesPage::finished(const games::GameCheck& c)
{
    if (m_worker.joinable())
        m_worker.join();
    m_progress->hide();
    m_stop->setEnabled(false);
    QString error;
    games::saveCheck(gamesDir(), c, &error);
    const games::GameCheck* before = nullptr;
    for (const games::GameCheck& e : m_earlier)
        if (e.id == m_compare->currentData().toString())
            before = &e;
    QDir().mkpath(m_context.data().reportsDir());
    m_report = QDir(m_context.data().reportsDir()).filePath(c.id + QStringLiteral(".html"));
    QFile f(m_report);
    if (f.open(QIODevice::WriteOnly))
        f.write(games::reportHtml(c, before).toUtf8());
    m_openReport->setEnabled(true);
    QString html;
    if (before) {
        html += QStringLiteral("<p><b>Before / after</b></p><ul>");
        for (const QString& l : games::compareChecks(*before, c))
            html += QStringLiteral("<li>%1</li>").arg(l.toHtmlEscaped());
        html += QStringLiteral("</ul>");
    }
    html += QStringLiteral("<ul>");
    for (const QString& l : games::summaryLines(c))
        html += QStringLiteral("<li>%1</li>").arg(l.toHtmlEscaped());
    html += QStringLiteral("</ul>");
    m_result->setHtml(html);
    m_status->setText(error.isEmpty() ? tr("Checked and saved (%1).").arg(c.id) : tr("The check could not be saved: %1").arg(error));
    m_last = c;
    m_competitor->clear();
    for (const games::Competitor& k : c.competitors)
        if (k.movable() && !k.use.path.isEmpty())
            m_competitor->addItem(QStringLiteral("%1 — %2 % of %3 (%4)").arg(k.use.name).arg(k.use.meanPercent, 0, 'f', 0).arg(k.use.adapter, compat::statusText(k.move.status)),
                                  k.use.path);
    if (m_competitor->count() == 0)
        m_competitor->addItem(tr("No competing program with a known way to move it"), QString());
    reloadEarlier();
}

void GamesPage::prepareProfile()
{
    profiles::ProfileStore* store = m_context.profiles();
    const QString exe = m_competitor->currentData().toString();
    if (!store || exe.isEmpty())
        return;
    const QList<profiles::KnownGpuOption> options = profiles::loadKnownOptions(QDir(m_context.data().configDir()).filePath(QStringLiteral("gpu-options.json")));
    const auto option = profiles::suggestedOptionFor(exe, options);
    if (QMessageBox::question(this, tr("Create an Intel-GPU variant"),
                              tr("Create a profile variant “%1” for %2 that starts it on the Intel GPU%3?\n\nNothing is started or closed now; your own "
                                 "profiles and Windows settings are not changed.")
                                  .arg(kVariant, QDir::toNativeSeparators(exe), option ? tr(" with “%1”").arg(option->arguments) : QString()))
        != QMessageBox::Yes)
        return;
    profiles::AppProfile p = profiles::newProfile(exe);
    p.variant = kVariant;
    p.mode = profiles::GpuMode::Integrated;
    if (option)
        p.arguments = profiles::withKnownOption(QString(), *option);
    p.id = profiles::profileIdFor(exe, kVariant);
    QString error;
    if (!store->save(p, &error)) {
        m_status->setText(tr("The variant could not be saved: %1").arg(error));
        return;
    }
    // Journal: what was created here, so Undo removes exactly that.
    QJsonArray journal;
    QFile f(journalFile());
    if (f.open(QIODevice::ReadOnly)) {
        journal = QJsonDocument::fromJson(f.readAll()).array();
        f.close();
    }
    journal << QJsonObject{ { QStringLiteral("profileId"), p.id }, { QStringLiteral("exe"), exe }, { QStringLiteral("created"), QDateTime::currentDateTime().toString(Qt::ISODate) } };
    if (f.open(QIODevice::WriteOnly))
        f.write(QJsonDocument(journal).toJson());
    m_status->setText(tr("Created the variant “%1” for %2. Start it from the GPU Manager (Launch) when you are ready; then check the game again to "
                         "see whether it helped.").arg(kVariant, QFileInfo(exe).fileName()));
    updateActions();
}

void GamesPage::undoProfiles()
{
    profiles::ProfileStore* store = m_context.profiles();
    QFile f(journalFile());
    if (!store || !f.open(QIODevice::ReadOnly))
        return;
    const QJsonArray journal = QJsonDocument::fromJson(f.readAll()).array();
    f.close();
    int removed = 0;
    QStringList kept;
    for (const QJsonValue& v : journal) {
        const QString id = v.toObject().value(QLatin1StringView("profileId")).toString();
        QString error;
        // Only variants GPU Fusion created here (their variant name says so).
        bool ours = false;
        for (const profiles::AppProfile& p : store->list())
            ours = ours || (p.id == id && p.variant == kVariant);
        if (ours && store->remove(id, &error))
            ++removed;
        else if (ours)
            kept << error;
    }
    QFile::remove(journalFile());
    m_status->setText(kept.isEmpty() ? tr("Removed %n variant(s) created here.", nullptr, removed) : tr("Removed %1; could not remove: %2").arg(removed).arg(kept.join(u' ')));
    updateActions();
}

} // namespace gf::ui
