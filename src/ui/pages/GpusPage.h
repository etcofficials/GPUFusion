#pragma once

#include <QWidget>

class QLabel;
class QPushButton;
class QVBoxLayout;

namespace gf::hw { struct DetectionResult; }

namespace gf::ui {

class AppContext;

class GpusPage : public QWidget {
    Q_OBJECT
public:
    explicit GpusPage(AppContext& context, QWidget* parent = nullptr);

private:
    void showDetecting();
    void showResult(const hw::DetectionResult& result);
    void clearCards();

    AppContext& m_context;
    QLabel* m_summary;
    QPushButton* m_refresh;
    QVBoxLayout* m_cards;
};

} // namespace gf::ui
