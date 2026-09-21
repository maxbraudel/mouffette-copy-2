#pragma once

#include "backend/domain/profile/ClientProfileCache.h"
#include <QPointer>
#include <QQuickImageProvider>

class ProfilePictureProvider final : public QQuickImageProvider
{
public:
    explicit ProfilePictureProvider(ClientProfileCache* cache)
        : QQuickImageProvider(QQuickImageProvider::Image), m_cache(cache) {}
    QImage requestImage(const QString& id, QSize* size, const QSize& requestedSize) override
    {
        // Images are small and requested synchronously on the QML thread.
        QImage image = m_cache ? m_cache->image(id)
                              : QImage(QStringLiteral(":/icons/default-profile-picture.jpg"));
        if (size) *size = image.size();
        if (requestedSize.isValid()) image = image.scaled(requestedSize, Qt::KeepAspectRatio,
                                                        Qt::SmoothTransformation);
        return image;
    }
private:
    QPointer<ClientProfileCache> m_cache;
};
