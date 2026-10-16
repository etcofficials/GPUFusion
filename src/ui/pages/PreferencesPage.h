#pragma once

#include <QWidget>

class QButtonGroup;
class QLabel;
class QListWidget;
class QPushButton;
class QRadioButton;
class QTableWidget;

namespace gf::ui {

class AppContext;

// Windows per-app GPU preference: what it does on this PC (verified with a probe), the apps that
// have one, an editor (confirm -> journal -> write; undo from history), and launching an app to
// check where it really runs.
class PreferencesPage : public QWidget {
    Q_OBJECT
public:
    explicit PreferencesPage(AppContext& context, QWidget* parent = nullptr);

private:
    void refreshVerification();
    void refreshApplications();
    void refreshHistory();
    void refreshEditor();
    void addApplication();
    void apply();
    void undo(const QString& recordId);
    void launch();
    QString selectedPath() const;
    bool isRunning(const QString& exePath) const;
    void setBusy(bool busy);

    AppContext& m_context;
    QLabel* m_verdict;
    QTableWidget* m_verification;
    QPushButton* m_verify;
    QListWidget* m_apps;
    QLabel* m_selected;
    QButtonGroup* m_choice;
    QRadioButton* m_notSet;
    QRadioButton* m_letWindows;
    QRadioButton* m_power;
    QRadioButton* m_performance;
    QPushButton* m_apply;
    QPushButton* m_launch;
    QLabel* m_editorNote;
    QTableWidget* m_history;
    QStringList m_extraApps; // added in this session but not yet configured
};

} // namespace gf::ui
