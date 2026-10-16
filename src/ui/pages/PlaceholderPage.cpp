#include "ui/pages/PlaceholderPage.h"

#include "ui/widgets/Card.h"

#include <QLabel>
#include <QVBoxLayout>

namespace gf::ui {

PlaceholderPage::PlaceholderPage(int phase, const QString& summary, const QStringList& planned, QWidget* parent)
    : QWidget(parent)
{
    auto* layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);

    auto* card = new Card(tr("Not implemented yet"));
    card->setBadge(tr("Planned — Phase %1").arg(phase));

    auto* text = new QLabel(summary);
    text->setWordWrap(true);
    card->body()->addWidget(text);

    if (!planned.isEmpty()) {
        QString items;
        for (const QString& p : planned)
            items += QStringLiteral("<li style='margin-bottom:4px'>%1</li>").arg(p.toHtmlEscaped());
        auto* list = new QLabel(QStringLiteral("<ul style='margin-left:-20px'>%1</ul>").arg(items));
        list->setObjectName(QStringLiteral("muted"));
        list->setWordWrap(true);
        list->setTextFormat(Qt::RichText);
        card->body()->addWidget(list);
    }

    layout->addWidget(card);
    layout->addStretch();
}

} // namespace gf::ui
