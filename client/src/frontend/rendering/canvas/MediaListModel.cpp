#include "frontend/rendering/canvas/MediaListModel.h"

MediaListModel::MediaListModel(QObject* parent)
    : QAbstractListModel(parent)
{
}

// ---------------------------------------------------------------------------
// QAbstractListModel interface
// ---------------------------------------------------------------------------

int MediaListModel::rowCount(const QModelIndex& parent) const
{
    if (parent.isValid())
        return 0;
    return m_rows.size();
}

QVariant MediaListModel::data(const QModelIndex& index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= static_cast<int>(m_rows.size()))
        return {};
    if (role == ModelDataRole)
        return m_rows[index.row()].data;
    return {};
}

QHash<int, QByteArray> MediaListModel::roleNames() const
{
    // Named "modelData" so that QML delegates can write:
    //   property var media: modelData
    // and have it behave identically to the old JS-array modelData.
    return { { ModelDataRole, QByteArrayLiteral("modelData") } };
}

// ---------------------------------------------------------------------------
// clearAll
// ---------------------------------------------------------------------------

void MediaListModel::clearAll()
{
    if (m_rows.isEmpty())
        return;
    beginResetModel();
    m_rows.clear();
    endResetModel();
}

// ---------------------------------------------------------------------------
// rowForId
// ---------------------------------------------------------------------------

int MediaListModel::rowForId(const QString& mediaId) const
{
    for (int i = 0, n = m_rows.size(); i < n; ++i) {
        if (m_rows[i].mediaId == mediaId)
            return i;
    }
    return -1;
}

// ---------------------------------------------------------------------------
// updateFromList — structural diff algorithm
//
// After the sort performed by pushMediaModelOnly() the order is deterministic
// (by mediaId).  We walk the new list in order and either:
//   1. In-place update a row that exists at the correct position and changed.
//   2. Move a row that exists at the wrong position.
//   3. Insert a brand-new row.
//   4. Remove rows no longer present (backward pass first).
// ---------------------------------------------------------------------------

void MediaListModel::updateFromList(const QVariantList& newList)
{
    // ── Build new row vector ──────────────────────────────────────────────
    QVector<Row> newRows;
    newRows.reserve(newList.size());
    for (const QVariant& v : newList) {
        QVariantMap map = v.toMap();
        QString mediaId = map.value(QStringLiteral("mediaId")).toString();
        newRows.append({ std::move(mediaId), std::move(map) });
    }

    // ── Pass 1: remove rows whose mediaId no longer exists ────────────────
    // Iterate backwards so indices stay valid after each remove.
    {
        QSet<QString> newIds;
        newIds.reserve(newRows.size());
        for (const Row& r : std::as_const(newRows))
            newIds.insert(r.mediaId);

        for (int i = static_cast<int>(m_rows.size()) - 1; i >= 0; --i) {
            if (!newIds.contains(m_rows[i].mediaId)) {
                beginRemoveRows(QModelIndex(), i, i);
                m_rows.remove(i);
                endRemoveRows();
            }
        }
    }

    // ── Pass 2: insert / move / update to match newRows order ────────────
    for (int newIdx = 0; newIdx < static_cast<int>(newRows.size()); ++newIdx) {
        const Row& nr = newRows[newIdx];
        int curIdx = rowForId(nr.mediaId);

        if (curIdx == -1) {
            // ── Brand-new item: insert at newIdx ─────────────────────────
            beginInsertRows(QModelIndex(), newIdx, newIdx);
            m_rows.insert(newIdx, nr);
            endInsertRows();

        } else if (curIdx == newIdx) {
            // ── Already at correct position: update data if changed ───────
            if (m_rows[newIdx].data != nr.data) {
                m_rows[newIdx].data = nr.data;
                const QModelIndex idx = index(newIdx);
                emit dataChanged(idx, idx, { ModelDataRole });
            }

        } else {
            // ── Exists but at wrong position: move then update ────────────
            // beginMoveRows destination is the row BEFORE which the item
            // should land; when moving forward (curIdx < newIdx) the
            // destination must be newIdx+1.
            const int dest = (curIdx < newIdx) ? newIdx + 1 : newIdx;
            beginMoveRows(QModelIndex(), curIdx, curIdx, QModelIndex(), dest);
            m_rows.move(curIdx, newIdx);
            endMoveRows();

            if (m_rows[newIdx].data != nr.data) {
                m_rows[newIdx].data = nr.data;
                const QModelIndex idx = index(newIdx);
                emit dataChanged(idx, idx, { ModelDataRole });
            }
        }
    }
}
