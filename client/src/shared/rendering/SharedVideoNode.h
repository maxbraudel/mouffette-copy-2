#pragma once

#include "backend/media/ResidentVideoPlayer.h"
#include <QSGGeometryNode>
#include <QSGMaterial>
#include <QSGMaterialShader>
#include <QSGTexture>
#include <QtMultimedia/private/qvideotexturehelper_p.h>
#include <QtMultimedia/private/qhwvideobuffer_p.h>
#include <array>
#include <memory>

// This adapter is tied to the Qt Multimedia version selected by CMake. Qt's
// own video shaders retain YUV range, transfer functions and high bit depth.
namespace SharedVideoRendering {
class PlaneTexture final : public QSGTexture {
public:
    QRhiTexture* plane = nullptr;
    qint64 comparisonKey() const override { return qint64(quintptr(plane)); }
    QRhiTexture* rhiTexture() const override { return plane; }
    QSize textureSize() const override { return plane ? plane->pixelSize() : QSize(); }
    bool hasAlphaChannel() const override { return false; }
    bool hasMipmaps() const override { return false; }
};
struct Planes {
    QVideoFrameTexturesUPtr owner;
    std::array<PlaneTexture, 3> textures;
};
inline std::shared_ptr<Planes> planesFor(const QVideoFrame& frame, QRhi* rhi, QRhiResourceUpdateBatch* updates) {
    using Frames = QHash<QString, std::weak_ptr<Planes>>;
    thread_local QHash<QRhi*, Frames> contexts;
    const QString key = QString::number(ResidentVideoPlayer::frameIdentity(frame)) + QLatin1Char(':') + QString::number(frame.startTime());
    auto& entries = contexts[rhi];
    auto planes = entries.value(key).lock();
    if (!planes) {
        planes = std::make_shared<Planes>();
        QVideoFrameTexturesUPtr old;
        planes->owner = QVideoTextureHelper::createTextures(frame, *rhi, *updates, old);
        if (!planes->owner) return {};
        const int count = QVideoTextureHelper::textureDescription(frame.pixelFormat())->nplanes;
        for (int i = 0; i < count && i < 3; ++i) {
            planes->textures[i].plane = planes->owner->texture(i);
            planes->textures[i].setFiltering(QSGTexture::Linear);
        }
        entries.insert(key, planes);
    }
    for (auto context = contexts.begin(); context != contexts.end();) {
        for (auto entry = context->begin(); entry != context->end();) {
            if (entry.value().expired()) entry = context->erase(entry); else ++entry;
        }
        if (context->isEmpty()) context = contexts.erase(context); else ++context;
    }
    return planes;
}
class Material;
class Shader final : public QSGMaterialShader {
public:
    explicit Shader(const QVideoFrameFormat& format, QRhi* rhi) {
        setShaderFileName(VertexStage, QVideoTextureHelper::vertexShaderFileName(format));
        setShaderFileName(FragmentStage, QVideoTextureHelper::fragmentShaderFileName(format, rhi));
    }
    bool updateUniformData(RenderState& state, QSGMaterial* material, QSGMaterial*) override;
    void updateSampledImage(RenderState& state, int binding, QSGTexture** texture, QSGMaterial* material, QSGMaterial*) override;
};
class Material final : public QSGMaterial {
public:
    QVideoFrame frame;
    QRhi* rhi;
    std::shared_ptr<Planes> planes;
    explicit Material(QVideoFrame value, QRhi* context) : frame(std::move(value)), rhi(context) { setFlag(Blending); }
    QSGMaterialType* type() const override {
        thread_local QHash<QString, std::shared_ptr<QSGMaterialType>> types;
        const QString shader = QVideoTextureHelper::fragmentShaderFileName(frame.surfaceFormat(), rhi);
        auto& type = types[shader];
        if (!type) type = std::make_shared<QSGMaterialType>();
        return type.get();
    }
    QSGMaterialShader* createShader(QSGRendererInterface::RenderMode) const override { return new Shader(frame.surfaceFormat(), rhi); }
    int compare(const QSGMaterial* other) const override {
        const auto* value = static_cast<const Material*>(other);
        const auto a = ResidentVideoPlayer::frameIdentity(frame), b = ResidentVideoPlayer::frameIdentity(value->frame);
        if (a != b) return a < b ? -1 : 1;
        return frame.startTime() == value->frame.startTime() ? 0 : frame.startTime() < value->frame.startTime() ? -1 : 1;
    }
};
inline bool Shader::updateUniformData(RenderState& state, QSGMaterial* value, QSGMaterial*) {
    auto* material = static_cast<Material*>(value);
    QVideoTextureHelper::updateUniformData(state.uniformData(), state.rhi(), material->frame.surfaceFormat(),
        material->frame, state.combinedMatrix(), state.opacity());
    return true;
}
inline void Shader::updateSampledImage(RenderState& state, int binding, QSGTexture** texture, QSGMaterial* value, QSGMaterial*) {
    auto* material = static_cast<Material*>(value);
    if (!material->planes) material->planes = planesFor(material->frame, state.rhi(), state.resourceUpdateBatch());
    if (material->planes && binding >= 1 && binding <= 3) *texture = &material->planes->textures[size_t(binding-1)];
}
}
class SharedVideoNode final : public QSGGeometryNode {
public:
    SharedVideoNode(const QVideoFrame& frame, QRhi* rhi) {
        auto* geometry = new QSGGeometry(QSGGeometry::defaultAttributes_TexturedPoint2D(), 4);
        geometry->setDrawingMode(QSGGeometry::DrawTriangleStrip);
        setGeometry(geometry); setFlag(OwnsGeometry);
        setMaterial(new SharedVideoRendering::Material(frame, rhi)); setFlag(OwnsMaterial);
    }
    void update(const QVideoFrame& frame, const QRectF& rect) {
        auto* value = static_cast<SharedVideoRendering::Material*>(material());
        if (ResidentVideoPlayer::frameIdentity(frame) != ResidentVideoPlayer::frameIdentity(value->frame)
            || frame.startTime() != value->frame.startTime()) {
            value->frame = frame; value->planes.reset(); markDirty(DirtyMaterial);
        }
        QSGGeometry::updateTexturedRectGeometry(geometry(), rect, QRectF(0, 0, 1, 1));
        auto* vertices = geometry()->vertexDataAsTexturedPoint2D();
        for (int i = 0; i < 4; ++i) {
            float x = vertices[i].tx, y = vertices[i].ty;
            if (frame.mirrored()) x = 1-x;
            switch (frame.rotation()) {
            case QtVideo::Rotation::Clockwise90: vertices[i].tx = y; vertices[i].ty = 1-x; break;
            case QtVideo::Rotation::Clockwise180: vertices[i].tx = 1-x; vertices[i].ty = 1-y; break;
            case QtVideo::Rotation::Clockwise270: vertices[i].tx = 1-y; vertices[i].ty = x; break;
            default: vertices[i].tx = x; vertices[i].ty = y; break;
            }
        }
        markDirty(DirtyGeometry);
    }
    QVideoFrameFormat::PixelFormat pixelFormat() const { return static_cast<SharedVideoRendering::Material*>(material())->frame.pixelFormat(); }
};
