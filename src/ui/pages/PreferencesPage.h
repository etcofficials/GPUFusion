#pragma once

#include "preferences/DualGpu.h"
#include "preferences/GpuSelection.h"

#include <QPointer>
#include <QWidget>

#include <atomic>

class QButtonGroup;
class QGroupBox;
class QLabel;
class QListWidget;
class QPushButton;
class QRadioButton;
class QTableWidget;
class QThread;
class QVBoxLayout;

namespace gf::mon { struct UsageEvidence; }

namespace gf::ui {

class AppContext;

// Per-application GPU selection. Choices are named by adapter (Automatic / integrated / discrete /
// both) and mapped onto Windows' per-app preference only where the probe verification shows the
// mapping works on this PC; "Verify on this PC" launches the program and observes which GPU its
// processes actually render on. Also: what the preference does here (probe), the apps that have one,
// raw Windows values (advanced), a dual-GPU check per program, and the undoable change history.
class PreferencesPage : public QWidget {
    Q_OBJECT
public:
    explicit PreferencesPage(AppContext& context, QWidget* parent = nullptr);
    ~PreferencesPage() override;

private:
    void refreshVerification();
    void refreshApplications();
    void refreshHistory();
    void refreshEditor();
    void refreshChoices();
    void addApplication();
    void addAndSelect(const QString& path);
    void apply();
    void undo(const QString& recordId);
    void launch();
    void verifyUsage();
    void showUsageResult(const mon::UsageEvidence& evidence);
    QString selectedPath() const;
    bool isRunning(const QString& exePath) const;
    void setBusy(bool busy);
    QList<pref::SelectableGpu> selectableGpus() const;
    pref::MultiGpuSystem multiGpuSystem() const;
    pref::GpuChoice selectedChoice() const;

    AppContext& m_context;
    QLabel* m_verdict;
    QTableWidget* m_verification;
    QPushButton* m_verify;
    QListWidget* m_apps;
    QLabel* m_selected;
    QButtonGroup* m_gpuChoice;          // adapter-oriented choices
    QRadioButton* m_auto;
    QRadioButton* m_integrated;
    QRadioButton* m_discrete;
    QRadioButton* m_both;
    QLabel* m_plan;
    QGroupBox* m_advanced;              // raw Windows values
    QButtonGroup* m_choice;
    QRadioButton* m_notSet;
    QRadioButton* m_letWindows;
    QRadioButton* m_power;
    QRadioButton* m_performance;
    QPushButton* m_apply;
    QPushButton* m_verifyUsage;
    QPushButton* m_launch;
    QLabel* m_editorNote;
    QLabel* m_usageResult;
    QVBoxLayout* m_usageActions;
    QTableWidget* m_history;
    QStringList m_extraApps; // added in this session but not yet configured
    QPointer<QThread> m_usageThread;
    std::atomic<bool> m_cancelUsage{ false };
};

} // namespace gf::ui
