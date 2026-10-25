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
    profiles::CapabilityInputs inputsFor(const profiles::AppProfile& p) const;
    bool saveProfile(profiles::AppProfile& p);
    void setBusy(bool busy);

    AppContext& m_context;
    QListWidget* m_list;
    QPushButton* m_remove;
    QLabel* m_header;
    QButtonGroup* m_modes;
    QRadioButton* m_modeButtons[4];
    QLabel* m_capability;
    QLineEdit* m_arguments;
    QCheckBox* m_shareChoice;
    QPushButton* m_apply;
    QPushButton* m_launch;
    QPushButton* m_verify;
    QLabel* m_result;
    QWidget* m_editor;
    QPointer<QThread> m_thread;
    std::atomic<bool> m_cancel{ false };
    bool m_loading = false;   // filling the editor from a profile: no change handlers
};

} // namespace gf::ui
