#pragma once

#include "preferences/PreferenceModel.h"

#include <QObject>
#include <QPointer>
#include <QThread>

#include <memory>
#include <optional>

namespace gf::pref {

// Owns the real registry store, the change journal (data\config\gpu-preference-changes.json) and
// the last verification result (data\reports\gpu-preference-verification.json). At start-up it
// restores a probe value left behind by an interrupted verification.
class PreferenceService : public QObject {
    Q_OBJECT
public:
    PreferenceService(const QString& configDir, const QString& reportsDir, const QString& probePath, QObject* parent = nullptr);
    ~PreferenceService() override;

    PreferenceManager& manager() { return *m_manager; }
    const ChangeJournal& journal() const { return m_journal; }
    IPreferenceStore& store() { return *m_store; }
    QString journalError() const { return m_journalError; }

    QString probePath() const { return m_probePath; }
    bool probeAvailable() const;
    const std::optional<VerificationResult>& lastVerification() const { return m_lastVerification; }
    bool isVerifying() const { return m_thread != nullptr; }
    void startVerification();

    // Executables with a stored preference (value names), sorted.
    QStringList configuredApplications() const;

Q_SIGNALS:
    void changed();                 // a preference or the journal changed
    void verificationStarted();
    void verificationFinished();

private:
    std::unique_ptr<IPreferenceStore> m_store;
    ChangeJournal m_journal;
    std::unique_ptr<PreferenceManager> m_manager;
    QString m_reportPath;
    QString m_probePath;
    QString m_journalError;
    std::optional<VerificationResult> m_lastVerification;
    QPointer<QThread> m_thread;
};

} // namespace gf::pref
