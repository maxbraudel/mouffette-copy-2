#pragma once
#include <QHash>
#include <QImage>
#include <QQuickWindow>
#include <QSGTexture>
#include <memory>

// Called only during scene-graph synchronization. Weak entries never extend a
// texture's lifetime, and a window key prevents sharing across graphics devices.
inline std::shared_ptr<QSGTexture> sharedImageTexture(QQuickWindow* window, const QImage& image) {
    using Textures = QHash<qint64, std::weak_ptr<QSGTexture>>;
    thread_local QHash<QQuickWindow*, Textures> windows;
    auto& textures = windows[window];
    auto texture = textures.value(image.cacheKey()).lock();
    if (!texture) {
        // Both consumers use QSGImageNode, which maps its pixel source rect
        // through the atlas sub-rectangle. Small filmstrip images can therefore
        // share a backing texture and be batched by the scene graph.
        texture.reset(window->createTextureFromImage(image, QQuickWindow::TextureCanUseAtlas));
        textures[image.cacheKey()] = texture;
    }
    for (auto it = windows.begin(); it != windows.end();) {
        for (auto entry = it->begin(); entry != it->end();) {
            if (entry.value().expired()) entry = it->erase(entry); else ++entry;
        }
        if (it->isEmpty()) it = windows.erase(it); else ++it;
    }
    return texture;
}
