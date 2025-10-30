#ifndef CLIENTLISTDELEGATE_H
#define CLIENTLISTDELEGATE_H

#include <QStyledItemDelegate>

/**
 * @brief Custom delegate for client list that draws separators between items
 * 
 * This lightweight delegate draws a 1px separator line between list items
 * (no line above first, none below last). It also suppresses hover/selection
 * effects for placeholder items with no flags.
 */
class ClientListSeparatorDelegate : public QStyledItemDelegate {
    Q_OBJECT

public:
    using QStyledItemDelegate::QStyledItemDelegate;
    
    void paint(QPainter* painter, const QStyleOptionViewItem& option, 
               const QModelIndex& index) const override;
};

#endif // CLIENTLISTDELEGATE_H
