#ifndef MEDIALISTMODEL_H
#define MEDIALISTMODEL_H

#include <QAbstractListModel>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

// ---------------------------------------------------------------------------
// MediaListModel
// ---------------------------------------------------------------------------
// A QAbstractListModel that wraps the canvas media list and exposes each row
// as a single "modelData" role (QVariantMap) to QML.
//
// The key contract:
//   • The model object IDENTITY never changes — it is created once and handed
//     to the QML Repeater via a context property.  The Repeater therefore
//     never destroys its delegates on routine geometry/state updates.
//
//   • updateFromList() performs a structural diff:
//       – rows whose data changed  → dataChanged() signal (in-place update)
//       – rows added to the scene  → beginInsertRows / endInsertRows
//       – rows removed from scene  → beginRemoveRows / endRemoveRows
//       – rows that moved position → beginMoveRows / endMoveRows
//
// Because the QML delegate item is kept alive across normal updates, the
// VideoOutput → QVideoSink pipeline is never disrupted and video never
// flickers after a move or resize gesture.
// ---------------------------------------------------------------------------
class MediaListModel : public QAbstractListModel
{
    Q_OBJECT
public:
    explicit MediaListModel(QObject* parent = nullptr);
    ~MediaListModel() override = default;

    // QAbstractListModel
    int     rowCount(const QModelIndex& parent = QModelIndex()) const override;
    QVariant data(const QModelIndex& index, int role = Qt::DisplayRole) const override;
    QHash<int, QByteArray> roleNames() const override;

    // Diff-update the model from a freshly-built QVariantList.
    // Each element must be a QVariantMap containing at least "mediaId".
    void updateFromList(const QVariantList& newList);

    // Reset to empty (e.g. on scene teardown).
    void clearAll();

    // Role enum
    enum Roles {
        ModelDataRole = Qt::UserRole + 1
    };

private:
    struct Row {
        QString    mediaId;
        QVariantMap data;
    };

    QVector<Row> m_rows;

    // Returns the current row index for a given mediaId, or -1.
    int rowForId(const QString& mediaId) const;
};

#endif // MEDIALISTMODEL_H
