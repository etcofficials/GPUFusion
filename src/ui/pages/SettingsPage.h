#pragma once

#include "core/Settings.h"

#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QPushButton;
class QSpinBox;

namespace gf::ui {

class AppContext;

class SettingsPage : public QWidget {
    Q_OBJECT
public:
    explicit SettingsPage(AppContext& context, QWidget* parent = nullptr);

private:
    void showSettings(const Settings& s);
    Settings collect() const;
    void updateDirtyState();
    void save();

    AppContext& m_context;

    QSpinBox* m_refreshMs;
    QSpinBox* m_historySeconds;
    QCheckBox* m_useNvml;
    QCheckBox* m_startMinimized;
    QCheckBox* m_startMonitoring;
    QComboBox* m_theme;
    QComboBox* m_logLevel;
    QSpinBox* m_logMaxKB;
    QSpinBox* m_logFiles;
    QSpinBox* m_warmupRuns;
    QSpinBox* m_measuredRuns;
    QSpinBox* m_retentionDays;

    QPushButton* m_save;
    QPushButton* m_revert;
    QLabel* m_status;
};

} // namespace gf::ui
