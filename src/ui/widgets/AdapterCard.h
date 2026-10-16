#pragma once

#include "hardware/AdapterModel.h"
#include "ui/widgets/Card.h"

class QGridLayout;

namespace gf::ui {

// Everything known about one adapter: identity, driver, memory, Direct3D and video capabilities,
// outputs, classification and findings. Sections reflow to one column on narrow windows.
class AdapterCard : public Card {
    Q_OBJECT
public:
    explicit AdapterCard(const hw::Adapter& adapter, QWidget* parent = nullptr);

    // Status shown as the first badge; exposed for tests and the dashboard.
    static QString statusText(const hw::Adapter& adapter, Tone* tone, QString* explanation);

protected:
    void resizeEvent(QResizeEvent* event) override;

private:
    QWidget* section(const QString& title, const QList<QPair<QString, QString>>& rows);
    void reflow();

    QGridLayout* m_sections;
    QList<QWidget*> m_sectionWidgets;
    int m_columns = 0;
};

} // namespace gf::ui
