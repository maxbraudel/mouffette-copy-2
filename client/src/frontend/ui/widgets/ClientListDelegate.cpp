#include "ClientListDelegate.h"
#include "frontend/ui/theme/AppColors.h"
#include <QPainter>
#include <QApplication>
#include <QStyleOptionViewItem>
#include <QModelIndex>
#include <QAbstractItemModel>
#include <QStyle>
#include <QPen>

namespace {
struct BadgeColors {
    QColor foreground;
    QColor background;
};

BadgeColors badgeColors(const QString& status, const QPalette& palette)
{
    if (status == QStringLiteral("Available") || status == QStringLiteral("Connected")
        || status == QStringLiteral("Live")) {
        return {AppColors::gStatusConnectedText, AppColors::gStatusConnectedBg};
    }
    if (status == QStringLiteral("Connecting") || status == QStringLiteral("Reconnecting")
        || status == QStringLiteral("Degraded")) {
        return {AppColors::gStatusWarningText, AppColors::gStatusWarningBg};
    }
    if (status == QStringLiteral("Disconnecting") || status == QStringLiteral("In use")
        || status == QStringLiteral("Unavailable")) {
        return {AppColors::gStatusErrorText, AppColors::gStatusErrorBg};
    }

    QColor foreground = palette.color(QPalette::Disabled, QPalette::Text);
    QColor background = foreground;
    background.setAlpha(28);
    return {foreground, background};
}

void drawSeparator(QPainter* painter,
                   const QStyleOptionViewItem& option,
                   const QModelIndex& index)
{
    if (!index.isValid() || index.row() <= 0) {
        return;
    }
    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, false);
    const QColor color = AppColors::getCurrentColor(AppColors::gAppBorderColorSource);
    painter->setPen(QPen(color, 1));
    painter->drawLine(option.rect.left(), option.rect.top(),
                      option.rect.right(), option.rect.top());
    painter->restore();
}
}

void ClientListSeparatorDelegate::paint(QPainter* painter, 
                                         const QStyleOptionViewItem& option, 
                                         const QModelIndex& index) const
{
    QStyleOptionViewItem opt(option);
    
    // Supprimer l'effet hover / sélection pour l'item message "no clients" (flags vides)
    if (index.isValid()) {
        Qt::ItemFlags f = index.model() ? index.model()->flags(index) : Qt::NoItemFlags;
        if (f == Qt::NoItemFlags) {
            opt.state &= ~QStyle::State_MouseOver;
            opt.state &= ~QStyle::State_Selected;
            opt.state &= ~QStyle::State_HasFocus;
        }
    }
    
    const bool isClientRow = index.data(ClientListRoles::IsClientRow).toBool();
    if (!isClientRow) {
        QStyledItemDelegate::paint(painter, opt, index);
        drawSeparator(painter, option, index);
        return;
    }

    // Let the active style draw hover/selection/background, then render a
    // stable two-line row and a real status badge ourselves.
    QStyleOptionViewItem backgroundOption(opt);
    backgroundOption.text.clear();
    QStyle* style = opt.widget ? opt.widget->style() : QApplication::style();
    style->drawControl(QStyle::CE_ItemViewItem,
                       &backgroundOption, painter, opt.widget);

    const QString primary = index.data(ClientListRoles::PrimaryText).toString();
    const QString status = index.data(ClientListRoles::AvailabilityStatus).toString();
    const QString secondary = index.data(ClientListRoles::SecondaryText).toString();

    const QRect content = option.rect.adjusted(12, 7, -12, -7);
    QFont primaryFont = option.font;
    primaryFont.setWeight(QFont::DemiBold);
    QFont badgeFont = option.font;
    badgeFont.setPointSizeF(qMax(8.0, option.font.pointSizeF() - 1.0));
    badgeFont.setWeight(QFont::DemiBold);
    QFont secondaryFont = option.font;
    secondaryFont.setPointSizeF(qMax(8.0, option.font.pointSizeF() - 1.0));

    const QFontMetrics badgeMetrics(badgeFont);
    const int badgeHeight = qMax(22, badgeMetrics.height() + 6);
    const int badgeWidth = badgeMetrics.horizontalAdvance(status) + 18;
    const QRect badgeRect(content.right() - badgeWidth + 1,
                          content.top(), badgeWidth, badgeHeight);
    const QRect primaryRect(content.left(), content.top(),
                            qMax(0, badgeRect.left() - content.left() - 10),
                            badgeHeight);

    painter->save();
    painter->setRenderHint(QPainter::Antialiasing, true);
    const BadgeColors colors = badgeColors(status, option.palette);
    painter->setPen(Qt::NoPen);
    painter->setBrush(colors.background);
    painter->drawRoundedRect(badgeRect, badgeHeight / 2.0, badgeHeight / 2.0);
    painter->setPen(colors.foreground);
    painter->setFont(badgeFont);
    painter->drawText(badgeRect, Qt::AlignCenter, status);

    painter->setFont(primaryFont);
    painter->setPen(option.palette.color(QPalette::Text));
    const QFontMetrics primaryMetrics(primaryFont);
    painter->drawText(primaryRect, Qt::AlignLeft | Qt::AlignVCenter,
                      primaryMetrics.elidedText(primary, Qt::ElideRight,
                                                primaryRect.width()));

    if (!secondary.isEmpty()) {
        const QRect secondaryRect(content.left(), badgeRect.bottom() + 5,
                                  content.width(),
                                  qMax(0, content.bottom() - badgeRect.bottom() - 4));
        painter->setFont(secondaryFont);
        painter->setPen(AppColors::gTextSecondary);
        const QFontMetrics secondaryMetrics(secondaryFont);
        painter->drawText(secondaryRect, Qt::AlignLeft | Qt::AlignVCenter,
                          secondaryMetrics.elidedText(secondary, Qt::ElideRight,
                                                      secondaryRect.width()));
    }
    painter->restore();
    drawSeparator(painter, option, index);
}

QSize ClientListSeparatorDelegate::sizeHint(const QStyleOptionViewItem& option,
                                             const QModelIndex& index) const
{
    if (!index.data(ClientListRoles::IsClientRow).toBool()) {
        return QStyledItemDelegate::sizeHint(option, index);
    }
    const bool hasSecondary = !index.data(ClientListRoles::SecondaryText).toString().isEmpty();
    QSize result = QStyledItemDelegate::sizeHint(option, index);
    result.setHeight(hasSecondary ? 66 : 48);
    return result;
}
