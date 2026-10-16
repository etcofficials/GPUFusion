#pragma once

#include <QHash>
#include <QWidget>

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace gf::ui {

class AppContext;
class LiveText;
class MonitorCard;

// Live overview: one MonitorCard per physical adapter, a monitoring status line and pause control.
class DashboardPage : public QWidget {
    Q_OBJECT
public:
    explicit DashboardPage(AppContext& context, QWidget* parent = nullptr);

private:
    void rebuildCards();
    void refresh();
    void updateStatus();

    AppContext& m_context;
    LiveText* m_status;
    QPushButton* m_pause;
    QLabel* m_banner;
    QVBoxLayout* m_cards;
    QHash<quint64, MonitorCard*> m_cardByLuid;
};

} // namespace gf::ui
