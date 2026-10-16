#include "ui/widgets/Card.h"

#include <QGridLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QVBoxLayout>

namespace gf::ui {

Card::Card(const QString& title, QWidget* parent) : QFrame(parent)
{
    setObjectName(QStringLiteral("card"));
    auto* outer = new QVBoxLayout(this);
    outer->setContentsMargins(16, 14, 16, 16);
    outer->setSpacing(10);

    m_header = new QHBoxLayout;
    m_header->setSpacing(6);
    auto* titleLabel = new QLabel(title);
    titleLabel->setObjectName(QStringLiteral("cardTitle"));
    titleLabel->setWordWrap(true);
    m_header->addWidget(titleLabel, 1);
    outer->addLayout(m_header);

    m_body = new QVBoxLayout;
    m_body->setSpacing(8);
    outer->addLayout(m_body);
}

QLabel* Card::addBadge(const QString& text, Tone tone, const QString& toolTip)
{
    auto* badge = new QLabel(text);
    switch (tone) {
    case Tone::Neutral: badge->setObjectName(QStringLiteral("badgeNeutral")); break;
    case Tone::Accent:  badge->setObjectName(QStringLiteral("badgeAccent")); break;
    case Tone::Good:    badge->setObjectName(QStringLiteral("badgeGood")); break;
    case Tone::Warning: badge->setObjectName(QStringLiteral("badge")); break;
    case Tone::Bad:     badge->setObjectName(QStringLiteral("badgeBad")); break;
    }
    badge->setToolTip(toolTip);
    badge->setAccessibleName(toolTip.isEmpty() ? text : QStringLiteral("%1: %2").arg(text, toolTip));
    m_header->addWidget(badge, 0, Qt::AlignTop);
    return badge;
}

InfoGrid::InfoGrid(QWidget* parent) : QWidget(parent)
{
    auto* grid = new QGridLayout(this);
    grid->setContentsMargins(0, 0, 0, 0);
    grid->setHorizontalSpacing(18);
    grid->setVerticalSpacing(6);
    grid->setColumnStretch(1, 1);
}

void InfoGrid::addRow(const QString& label, const QString& value)
{
    auto* grid = static_cast<QGridLayout*>(layout());
    const int row = grid->rowCount();
    auto* key = new QLabel(label);
    key->setObjectName(QStringLiteral("muted"));
    key->setAlignment(Qt::AlignLeft | Qt::AlignTop);
    auto* val = new QLabel(value);
    val->setWordWrap(true);
    val->setTextInteractionFlags(Qt::TextSelectableByMouse);
    grid->addWidget(key, row, 0);
    grid->addWidget(val, row, 1);
}

void InfoGrid::clear()
{
    while (QLayoutItem* item = layout()->takeAt(0)) {
        delete item->widget();
        delete item;
    }
}

} // namespace gf::ui
