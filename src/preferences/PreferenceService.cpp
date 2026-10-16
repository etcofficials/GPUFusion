#include "preferences/PreferenceService.h"

#include "core/Logging.h"
#include "platform/windows/ProbeRunner.h"
#include "platform/windows/RegistryPreferenceStore.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonDocument>
#include <QSaveFile>

namespace gf::pref {

PreferenceService::PreferenceService(const QString& configDir, const QString& reportsDir, const QString& probePath, QObject* parent)
    : QObject(parent),
      m_store(std::make_unique<win::RegistryPreferenceStore>()),
      m_journal(QDir(configDir).filePath(QStringLiteral("gpu-preference-changes.json"))),
      m_reportPath(QDir(reportsDir).filePath(QStringLiteral("gpu-preference-verification.json"))),
      m_probePath(QDir::toNativeSeparators(probePath))
{
    if (!m_journal.load(&m_journalError))
        qCWarning(lcPrefs).noquote() << "Preference journal could not be read:" << m_journalError;
    m_manager = std::make_unique<PreferenceManager>(*m_store, m_journal);

    QString error;
    if (recoverPending(*m_store, m_journal, &error))
        qCInfo(lcPrefs) << "Recovered from an interrupted preference verification";
    else if (!error.isEmpty())
        qCWarning(lcPrefs).noquote() << "Could not restore the probe preference from an interrupted verification:" << error;

    QFile report(m_reportPath);
    if (report.open(QIODevice::ReadOnly))
        m_lastVerification = VerificationResult::fromJson(QJsonDocument::fromJson(report.readAll()).object());
}

PreferenceService::~PreferenceService()
{
    if (m_thread)
        m_thread->wait(); // the verification restores the probe value before it finishes
}

bool PreferenceService::probeAvailable() const
{
    return QFileInfo::exists(m_probePath);
}

QStringList PreferenceService::configuredApplications() const
{
    QStringList names = m_store->names();
    names.sort(Qt::CaseInsensitive);
    return names;
}

void PreferenceService::startVerification()
{
    if (m_thread)
        return;
    Q_EMIT verificationStarted();
    QThread* thread = QThread::create([this] {
        VerificationResult result = verifyPreferences(*m_store, m_journal, m_probePath,
                                                      [](const QString& p, QString* e) { return win::runProbe(p, e); });
        QMetaObject::invokeMethod(this, [this, result = std::move(result)] {
            m_lastVerification = result;
            QDir().mkpath(QFileInfo(m_reportPath).absolutePath());
            QSaveFile f(m_reportPath);
            if (f.open(QIODevice::WriteOnly)) {
                f.write(QJsonDocument(result.toJson()).toJson(QJsonDocument::Indented));
                f.commit();
            }
            Q_EMIT verificationFinished();
            Q_EMIT changed();
        }, Qt::QueuedConnection);
    });
    thread->setObjectName(QStringLiteral("gpufusion-verify"));
    connect(thread, &QThread::finished, thread, &QObject::deleteLater);
    connect(thread, &QThread::finished, this, [this] { m_thread = nullptr; });
    m_thread = thread;
    thread->start();
}

} // namespace gf::pref
