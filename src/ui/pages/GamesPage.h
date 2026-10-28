#pragma once

#include "games/GameCheck.h"

#include <QWidget>

#include <atomic>
#include <memory>
#include <thread>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QProgressBar;
class QPushButton;
class QSpinBox;
class QTextBrowser;

namespace gf::ui {

class AppContext;

// Games: watch a running game (any program that renders) and the programs around it; see what
// competes for its GPU, what could move to the other GPU and how; measure frame times; compare
// before and after. GPU Fusion starts, closes and restarts nothing here; the only change it can make
// is a profile *variant* for a competitor (start it on the Intel GPU next time), recorded so it can be
// undone.
class GamesPage : public QWidget {
    Q_OBJECT
public:
    explicit GamesPage(AppContext& context, QWidget* parent = nullptr);
    ~GamesPage() override;

private:
    void refreshGames();
    void reloadEarlier();
    void start();
    void finished(const games::GameCheck& c);
    void prepareProfile();
    void undoProfiles();
    void updateActions();
    QString gamesDir() const;
    QString journalFile() const;
    std::function<compat::Assessment(const QString&)> assessor();

    AppContext& m_context;
    QComboBox* m_game;
    QPushButton* m_refresh;
    QSpinBox* m_seconds;
    QCheckBox* m_frames;
    QLineEdit* m_label;
    QComboBox* m_compare;
    QPushButton* m_start;
    QPushButton* m_stop;
    QProgressBar* m_progress;
    QLabel* m_status;
    QTextBrowser* m_result;
    QComboBox* m_competitor;
    QPushButton* m_prepare;
    QPushButton* m_undo;
    QPushButton* m_openReport;
    QString m_report;
    std::optional<games::GameCheck> m_last;
    QList<games::GameCheck> m_earlier;
    std::thread m_worker;
    std::shared_ptr<std::atomic<bool>> m_cancel;
};

} // namespace gf::ui
