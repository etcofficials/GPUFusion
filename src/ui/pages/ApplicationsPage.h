#pragma once

#include <QDateTime>
#include <QTimer>
#include <QWidget>

class QCheckBox;
class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QTableView;

namespace gf::ui {

class AppContext;
class LiveText;
class ProcessFilterModel;
class ProcessTableModel;

// Running processes with their per-process GPU footprint. Sampling runs only while this page is
// visible (it opens every process every couple of seconds).
class ApplicationsPage : public QWidget {
    Q_OBJECT
public:
    explicit ApplicationsPage(AppContext& context, QWidget* parent = nullptr);

Q_SIGNALS:
    // The user asked to open the GPU profile of the program at `exePath` (in the GPU Manager).
    void profileRequested(const QString& exePath);

protected:
    void showEvent(QShowEvent* event) override;
    void hideEvent(QHideEvent* event) override;

private:
    void onAdaptersChanged();
    void onProcesses();
    void reloadPreferences();
    void updateDetails();
    QString selectedKey() const;

    AppContext& m_context;
    ProcessTableModel* m_model;
    ProcessFilterModel* m_filter;
    QLineEdit* m_search;
    QComboBox* m_adapter;
    QCheckBox* m_showAll;
    QPushButton* m_refresh;
    LiveText* m_status;
    QLabel* m_coverage;
    QLabel* m_banner;
    QTableView* m_table;
    QLabel* m_details;
    QPushButton* m_openFolder;
    QPushButton* m_profileButton;
    QTimer m_preferenceTimer;
    QString m_detailsKey;
    QString m_profileSignature; // profile files (name, size, time) + GPUs last loaded into the model
    bool m_autoSelected = false;
    bool m_userPicked = false;
};

} // namespace gf::ui
