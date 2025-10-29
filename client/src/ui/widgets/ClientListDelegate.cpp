#include "ClientListDelegate.h"
#include "AppColors.h"
#include <QPainter>
#include <QStyleOptionViewItem>
#include <QModelIndex>
#include <QAbstractItemModel>
#include <QStyle>
#include <QPen>

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
    
    // Draw the item with the base delegate
    QStyledItemDelegate::paint(painter, opt, index);
    
    if (!index.isValid()) {
        return;
    }
    
    const QAbstractItemModel* model = index.model();
    if (!model) {
        return;
    }
    
    // Draw a 1px separator line at the top of items after the first
    if (index.row() > 0) {
        painter->save();
        painter->setRenderHint(QPainter::Antialiasing, false);
        
        QColor c = AppColors::getCurrentColor(AppColors::gAppBorderColorSource);
        painter->setPen(QPen(c, 1));
        
        const QRect r = option.rect;
        const int y = r.top();
        painter->drawLine(r.left(), y, r.right(), y);
        
        painter->restore();
    }
}
