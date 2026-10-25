#pragma once

#include "ui/widgets/Card.h"

#include <QPointer>
#include <QThread>

#include <atomic>

class QComboBox;
class QLabel;
class QLineEdit;
class QPushButton;
class QSpinBox;

namespace gf::ui {

class AppContext;

// "Your own jobs on several GPUs" (capability class C): a batch of independent jobs, one per input
// file, spread over the chosen GPUs by GPU Fusion's dispatcher — with a command template for any
// program that can be told which GPU to use, or the built-in H.264 decoder. Run once, or compare
// against each GPU alone (and the fastest GPU running as many jobs at once) with output checks.
class JobBatchPanel : public Card {
    Q_OBJECT
public:
    explicit JobBatchPanel(AppContext& context, QWidget* parent = nullptr);
    ~JobBatchPanel() override;

    // For tests: fill the form and start without dialogs.
    void configure(const QString& command, const QString& inputs, const QString& outputFolder, const QString& pattern,
                   const QString& gpus, int compareRuns);
    void start();
    bool isRunning() const { return m_thread != nullptr; }
    QString resultText() const;

Q_SIGNALS:
    void finished();

private:
    void browseInputs();
    void updateControls();

    AppContext& m_context;
    QComboBox* m_kind;
    QLineEdit* m_command;
    QLineEdit* m_inputs;
    QLineEdit* m_output;
    QLineEdit* m_pattern;
    QComboBox* m_gpus;
    QSpinBox* m_compare;
    QPushButton* m_run;
    QPushButton* m_cancelButton;
    QLabel* m_result;
    QPointer<QThread> m_thread;
    std::atomic<bool> m_cancel{ false };
};

} // namespace gf::ui
