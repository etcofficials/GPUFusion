#pragma once

#include "profiles/AppProfile.h"

#include <QPointer>
#include <QThread>
#include <QWidget>

#include <atomic>

class QButtonGroup;
class QCheckBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QRadioButton;

namespace gf::mon { struct UsageVerdict; }

namespace gf::ui {

class AppContext;

// The generic GPU manager: one saved profile per program (any executable — picked from the installed
// and running programs or browsed to), a GPU mode per profile (Automatic / Integrated / Discrete /
// Both) with what that mode can achieve for this program on this PC, and apply / launch / verify.
// GPU Fusion applies the strongest justified generic mechanism and verifies real GPU use; it never
// injects into or patches the program.
class GpuManagerPage : public QWidget {
    Q_OBJECT
public:
    // What to do on Launch / Verify when the program is already running: ask (default), ask it to close
    // (gracefully, never forced), or go on.
    enum class RunningPolicy { Ask, Close, Continue };

    explicit GpuManagerPage(AppContext& context, QWidget* parent = nullptr);
    ~GpuManagerPage() override;

    // For tests and the command line: adds (or selects) a profile for `exePath` without dialogs.
    bool addProgram(const QString& exePath, const QString& name = {});
    void selectMode(profiles::GpuMode mode);
    void applySelected(bool confirm = true);
    void verifySelected(int seconds = 20, bool confirm = true);
    bool isVerifying() const { return m_thread != nullptr; }
    QString resultText() const;
    QString capabilityText() const;
    void setRunningPolicy(RunningPolicy p) { m_runningPolicy = p; }
    // For tests: the known-option suggestion shown for the selected program ("" if none).
    QString knownOptionText() const;
    void useKnownOption();
    void setArguments(const QString& arguments);
    // Writes a shortcut for the selected (saved) profile to `path` (.lnk). For tests and the button.
    bool createShortcut(const QString& path);
    // A variant of the selected program (own mode and arguments), selected afterwards.
    bool addVariant(const QString& variantName);
    // Profile backup / transfer (the buttons ask for the file; tests call these directly).
    bool exportTo(const QString& path);
    bool importFrom(const QString& path, bool replace);

Q_SIGNALS:
    void verificationFinished();

private:
    void refreshList();
    void refreshEditor();
    void refreshCapability();
    void pickDiscovered();
    void browse();
    void removeSelected();
    void launchSelected();
    void showVerdict(const profiles::AppProfile& p, const mon::UsageVerdict& v);
    void showLastVerification(const profiles::AppProfile& p);
    std::optional<profiles::AppProfile> selected() const;
    profiles::GpuMode chosenMode() const;
    profiles::CapabilityInputs inputsFor(const profiles::AppProfile& p);
    void refreshKnownOption(const profiles::AppProfile& p);
    bool saveProfile(profiles::AppProfile& p);
    void setBusy(bool busy);
    bool handleRunning(const QString& exe, const QString& action); // false: do not go on

    AppContext& m_context;
    QListWidget* m_list;
    QPushButton* m_remove;
    QPushButton* m_addVariant;
    QLabel* m_header;
    QButtonGroup* m_modes;
    QRadioButton* m_modeButtons[4];
    QLabel* m_capability;
    QLineEdit* m_arguments;
    QLabel* m_knownOption;
    QPushButton* m_useKnownOption;
    QCheckBox* m_shareChoice;
    QPushButton* m_apply;
    QPushButton* m_launch;
    QPushButton* m_verify;
    QPushButton* m_shortcut;
    QLabel* m_result;
    QWidget* m_editor;
    QPointer<QThread> m_thread;
    std::atomic<bool> m_cancel{ false };
    bool m_loading = false;   // filling the editor from a profile: no change handlers
    RunningPolicy m_runningPolicy = RunningPolicy::Ask;
    QString m_inputsExe;      // capability inputs cached per executable (imports are read from disk)
    std::optional<profiles::CapabilityInputs> m_inputs;
};

} // namespace gf::ui
