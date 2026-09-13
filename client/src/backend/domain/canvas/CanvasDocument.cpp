#include "backend/domain/canvas/CanvasDocument.h"

#include "backend/domain/media/CanvasMedia.h"
#include "backend/domain/media/MediaSettingsState.h"
#include "backend/files/FileManager.h"

#include <QFileInfo>
#include <QJsonArray>
#include <QUrl>

#include <algorithm>
#include <cmath>
#include <limits>

namespace {
QString secondsText(int milliseconds)
{
    return QString::number(qMax(0, milliseconds) / 1000.0, 'f', 3);
}

QJsonObject spanForIntersection(int screenId, const QRectF& screen,
                                const QRectF& media)
{
    const QRectF intersection = screen.intersected(media);
    if (intersection.isEmpty() || screen.isEmpty() || media.isEmpty()) return {};
    return {
        {QStringLiteral("screenId"), screenId},
        {QStringLiteral("normX"), (media.x() - screen.x()) / screen.width()},
        {QStringLiteral("normY"), (media.y() - screen.y()) / screen.height()},
        {QStringLiteral("normW"), media.width() / screen.width()},
        {QStringLiteral("normH"), media.height() / screen.height()},
        {QStringLiteral("spanDestNormX"), (intersection.x() - screen.x()) / screen.width()},
        {QStringLiteral("spanDestNormY"), (intersection.y() - screen.y()) / screen.height()},
        {QStringLiteral("spanDestNormW"), intersection.width() / screen.width()},
        {QStringLiteral("spanDestNormH"), intersection.height() / screen.height()},
        {QStringLiteral("spanSourceNormX"), (intersection.x() - media.x()) / media.width()},
        {QStringLiteral("spanSourceNormY"), (intersection.y() - media.y()) / media.height()},
        {QStringLiteral("spanSourceNormW"), intersection.width() / media.width()},
        {QStringLiteral("spanSourceNormH"), intersection.height() / media.height()}
    };
}
}

CanvasDocument::CanvasDocument(QObject* parent)
    : QObject(parent)
{
}

CanvasDocument::~CanvasDocument() = default;

CanvasMedia* CanvasDocument::mediaById(const QString& mediaId) const
{
    for (CanvasMedia* item : m_media) {
        if (item && item->mediaId() == mediaId) return item;
    }
    return nullptr;
}

void CanvasDocument::adoptMedia(CanvasMedia* media)
{
    if (!media) return;
    media->setParent(this);
    m_media.append(media);
    connect(media, &CanvasMedia::changed, this, [this, media]() {
        emit mediaChanged(media->mediaId());
        emit documentChanged();
    });
    emit mediaAdded(media);
    emit documentChanged();
}

CanvasMedia* CanvasDocument::addText(const QPointF& position,
                                     const QString& text)
{
    if (m_editsLocked) return nullptr;
    auto* media = new CanvasMedia(CanvasMedia::Type::Text, QSize(400, 200));
    media->setText(text.isEmpty() ? QStringLiteral("Text") : text);
    media->fitTextToContent();
    media->setPosition(position - QPointF(media->sceneRect().width() * 0.5,
                                          media->sceneRect().height() * 0.5));
    media->setZ(nextZ());
    adoptMedia(media);
    select(media->mediaId());
    return media;
}

CanvasMedia* CanvasDocument::addPreparedFile(
    const QString& sourcePath, const QSize& nativeSize, bool video,
    const QPointF& position)
{
    if (m_editsLocked || sourcePath.isEmpty()) return nullptr;
    auto* media = new CanvasMedia(video ? CanvasMedia::Type::Video
                                        : CanvasMedia::Type::Image,
                                  nativeSize.expandedTo(QSize(1, 1)));
    media->setSourcePath(sourcePath);
    media->setPosition(position);
    media->setZ(nextZ());
    if (m_fileManager) {
        const QString fileId = m_fileManager->getOrCreateFileId(sourcePath);
        media->setFileId(fileId);
        if (!fileId.isEmpty()) {
            m_fileManager->associateMediaWithFile(media->mediaId(), fileId);
            if (!m_canvasSessionId.isEmpty()) {
                m_fileManager->associateFileWithIdea(fileId, m_canvasSessionId);
            }
        }
    }
    if (video) media->initializeVideoRuntime();
    adoptMedia(media);
    select(media->mediaId());
    return media;
}

bool CanvasDocument::removeMedia(const QString& mediaId)
{
    if (m_editsLocked) return false;
    for (qsizetype i = 0; i < m_media.size(); ++i) {
        CanvasMedia* media = m_media.at(i);
        if (!media || media->mediaId() != mediaId) continue;
        const bool selected = media->selected();
        emit mediaAboutToBeRemoved(media);
        m_media.removeAt(i);
        if (m_fileManager && !media->isText()) {
            m_fileManager->removeMediaAssociation(mediaId);
        }
        media->deleteLater();
        emit mediaRemoved(mediaId);
        if (selected) emit selectionChanged();
        emit documentChanged();
        return true;
    }
    return false;
}

void CanvasDocument::clear()
{
    const QList<CanvasMedia*> previous = m_media;
    m_media.clear();
    for (CanvasMedia* media : previous) {
        if (!media) continue;
        emit mediaAboutToBeRemoved(media);
        emit mediaRemoved(media->mediaId());
        if (m_fileManager && !media->isText()) {
            m_fileManager->removeMediaAssociation(media->mediaId());
        }
        media->deleteLater();
    }
    emit selectionChanged();
    emit documentChanged();
}

void CanvasDocument::moveForward(const QString& mediaId)
{
    if (m_editsLocked) return;
    CanvasMedia* target = mediaById(mediaId);
    if (!target) return;
    qreal nearest = std::numeric_limits<qreal>::max();
    for (CanvasMedia* item : m_media) {
        if (item != target && item->z() > target->z()) {
            nearest = std::min(nearest, item->z());
        }
    }
    target->setZ(nearest == std::numeric_limits<qreal>::max()
                     ? nextZ() : nearest + 0.5);
}

void CanvasDocument::moveBackward(const QString& mediaId)
{
    if (m_editsLocked) return;
    CanvasMedia* target = mediaById(mediaId);
    if (!target) return;
    qreal nearest = -std::numeric_limits<qreal>::max();
    for (CanvasMedia* item : m_media) {
        if (item != target && item->z() < target->z()) {
            nearest = std::max(nearest, item->z());
        }
    }
    target->setZ(nearest == -std::numeric_limits<qreal>::max()
                     ? 1.0 : std::max<qreal>(1.0, nearest - 0.5));
}

QStringList CanvasDocument::selectedMediaIds() const
{
    QStringList ids;
    for (CanvasMedia* item : m_media) {
        if (item && item->selected()) ids.append(item->mediaId());
    }
    return ids;
}

CanvasMedia* CanvasDocument::selectedMedia() const
{
    for (CanvasMedia* item : m_media) {
        if (item && item->selected()) return item;
    }
    return nullptr;
}

void CanvasDocument::select(const QString& mediaId, bool additive)
{
    if (m_editsLocked) return;
    CanvasMedia* target = mediaById(mediaId);
    if (!target) return;
    bool changed = false;
    for (CanvasMedia* item : m_media) {
        const bool selected = item == target || (additive && item->selected());
        if (item->selected() != selected) {
            item->setSelected(selected);
            changed = true;
        }
    }
    if (changed) emit selectionChanged();
}

void CanvasDocument::clearSelection()
{
    bool changed = false;
    for (CanvasMedia* item : m_media) {
        if (item && item->selected()) {
            item->setSelected(false);
            changed = true;
        }
    }
    if (changed) emit selectionChanged();
}

void CanvasDocument::setScreens(const QList<ScreenInfo>& screens)
{
    m_screens = screens;
    rebuildScreenRects();
    emit screensChanged();
    emit documentChanged();
}

void CanvasDocument::rebuildScreenRects()
{
    m_screenRects.clear();
    if (m_screens.isEmpty()) return;
    int minX = std::numeric_limits<int>::max();
    int minY = std::numeric_limits<int>::max();
    for (const ScreenInfo& screen : m_screens) {
        minX = std::min(minX, screen.x);
        minY = std::min(minY, screen.y);
    }
    for (const ScreenInfo& screen : m_screens) {
        m_screenRects.insert(screen.id,
            QRectF(screen.x - minX, screen.y - minY,
                   screen.width, screen.height));
    }
}

void CanvasDocument::setCamera(qreal scale, qreal panX, qreal panY)
{
    if (!std::isfinite(scale) || scale <= 0.0001
        || !std::isfinite(panX) || !std::isfinite(panY)) return;
    if (qFuzzyCompare(1.0 + m_cameraScale, 1.0 + scale)
        && qFuzzyCompare(1.0 + m_cameraPanX, 1.0 + panX)
        && qFuzzyCompare(1.0 + m_cameraPanY, 1.0 + panY)) return;
    m_cameraScale = scale;
    m_cameraPanX = panX;
    m_cameraPanY = panY;
    emit cameraChanged();
}

void CanvasDocument::resetCamera()
{
    setCamera(1.0, 0.0, 0.0);
}

void CanvasDocument::setRemoteCursor(bool visible,
                                     const QPointF& scenePosition)
{
    if (m_remoteCursorVisible == visible
        && m_remoteCursorPosition == scenePosition) return;
    m_remoteCursorVisible = visible;
    m_remoteCursorPosition = scenePosition;
    emit remoteCursorChanged();
}

bool CanvasDocument::mapRemoteCursor(int globalX, int globalY,
                                     QPointF* scenePosition) const
{
    for (const ScreenInfo& screen : m_screens) {
        const QRect remote(screen.x, screen.y, screen.width, screen.height);
        if (!remote.adjusted(-1, -1, 1, 1).contains(globalX, globalY)) continue;
        const QRectF local = m_screenRects.value(screen.id);
        if (local.isEmpty()) return false;
        const qreal rx = screen.width > 1
            ? qBound(0.0, (globalX - screen.x) / qreal(screen.width - 1), 1.0)
            : 0.0;
        const qreal ry = screen.height > 1
            ? qBound(0.0, (globalY - screen.y) / qreal(screen.height - 1), 1.0)
            : 0.0;
        if (scenePosition) {
            *scenePosition = {local.x() + rx * local.width(),
                              local.y() + ry * local.height()};
        }
        return true;
    }
    return false;
}

void CanvasDocument::setEditsLocked(bool locked)
{
    if (m_editsLocked == locked) return;
    m_editsLocked = locked;
    emit editsLockedChanged();
}

void CanvasDocument::setContentAvailable(bool available)
{
    if (m_contentAvailable == available) return;
    m_contentAvailable = available;
    emit contentAvailabilityChanged();
}

QJsonObject CanvasDocument::serializeSceneState() const
{
    QJsonObject root{{QStringLiteral("renderSchemaVersion"), 2}};
    if (!m_canvasSessionId.isEmpty()
        && m_canvasSessionId != QLatin1String("default")) {
        root.insert(QStringLiteral("canvasSessionId"), m_canvasSessionId);
    }
    QJsonArray screens;
    for (const ScreenInfo& screen : m_screens) screens.append(screen.toJson());
    root.insert(QStringLiteral("screens"), screens);

    QJsonArray serializedMedia;
    QList<CanvasMedia*> ordered = m_media;
    std::sort(ordered.begin(), ordered.end(), [](CanvasMedia* a, CanvasMedia* b) {
        return a && b ? a->z() < b->z() : a < b;
    });
    for (CanvasMedia* media : ordered) {
        if (!media) continue;
        const QRectF bounds = media->sceneRect().normalized();
        const MediaSettingsState& settings = media->settings();
        QJsonObject item{
            {QStringLiteral("mediaId"), media->mediaId()},
            {QStringLiteral("fileId"), media->fileId()},
            {QStringLiteral("fileName"), QFileInfo(media->sourcePath()).fileName()},
            {QStringLiteral("type"), media->typeName()},
            {QStringLiteral("x"), bounds.x()},
            {QStringLiteral("y"), bounds.y()},
            {QStringLiteral("width"), bounds.width()},
            {QStringLiteral("height"), bounds.height()},
            {QStringLiteral("baseWidth"), media->baseSize().width()},
            {QStringLiteral("baseHeight"), media->baseSize().height()},
            {QStringLiteral("visible"), media->contentVisible()},
            {QStringLiteral("z"), media->z()},
            {QStringLiteral("autoDisplay"), settings.displayAutomatically},
            {QStringLiteral("autoDisplayDelayMs"),
                 MediaSettingsSerialization::delayMilliseconds(
                     settings.displayDelayEnabled, settings.displayDelayText)},
            {QStringLiteral("autoHide"), settings.hideDelayEnabled},
            {QStringLiteral("autoHideDelayMs"),
                 MediaSettingsSerialization::delayMilliseconds(
                     settings.hideDelayEnabled, settings.hideDelayText)},
            {QStringLiteral("hideWhenVideoEnds"), settings.hideWhenVideoEnds},
            {QStringLiteral("fadeInSeconds"),
                 MediaSettingsSerialization::durationSeconds(
                     settings.fadeInEnabled, settings.fadeInText)},
            {QStringLiteral("fadeOutSeconds"),
                 MediaSettingsSerialization::durationSeconds(
                     settings.fadeOutEnabled, settings.fadeOutText)},
            {QStringLiteral("contentOpacity"), media->contentOpacity()}
        };
        QJsonArray spans;
        for (auto it = m_screenRects.cbegin(); it != m_screenRects.cend(); ++it) {
            const QJsonObject span = spanForIntersection(it.key(), it.value(), bounds);
            if (!span.isEmpty()) spans.append(span);
        }
        if (!spans.isEmpty()) item.insert(QStringLiteral("spans"), spans);

        if (media->isText()) {
            item.insert(QStringLiteral("text"), media->text());
            item.insert(QStringLiteral("fontFamily"), media->fontFamily());
            item.insert(QStringLiteral("fontSize"), media->fontPixelSize());
            item.insert(QStringLiteral("fontPixelSize"), media->fontPixelSize());
            item.insert(QStringLiteral("fontWeight"), media->fontWeight());
            item.insert(QStringLiteral("fontBold"), media->fontWeight() >= 600);
            item.insert(QStringLiteral("fontItalic"), media->italic());
            item.insert(QStringLiteral("fontUnderline"), media->underline());
            item.insert(QStringLiteral("fontUppercase"), media->uppercase());
            item.insert(QStringLiteral("textColor"), media->textColor().name(QColor::HexArgb));
            item.insert(QStringLiteral("textBorderWidthPercent"), media->outlineWidthPercent());
            item.insert(QStringLiteral("textOutlineWidthPx"),
                        media->outlineWidthPercent() * media->fontPixelSize() / 100.0);
            item.insert(QStringLiteral("textBorderColor"), media->outlineColor().name(QColor::HexArgb));
            item.insert(QStringLiteral("textHighlightEnabled"), media->highlightEnabled());
            item.insert(QStringLiteral("textHighlightColor"), media->highlightColor().name(QColor::HexArgb));
            item.insert(QStringLiteral("textFitToTextEnabled"), media->fitToTextEnabled());
            item.insert(QStringLiteral("uniformScale"), media->scale());
            item.insert(QStringLiteral("horizontalAlignment"), media->horizontalAlignment());
            item.insert(QStringLiteral("verticalAlignment"), media->verticalAlignment());
        } else if (media->isVideo()) {
            item.insert(QStringLiteral("autoPlay"), settings.playAutomatically);
            item.insert(QStringLiteral("autoPlayDelayMs"),
                        MediaSettingsSerialization::delayMilliseconds(
                            settings.playDelayEnabled, settings.playDelayText));
            item.insert(QStringLiteral("autoPause"), settings.pauseDelayEnabled);
            item.insert(QStringLiteral("autoPauseDelayMs"),
                        MediaSettingsSerialization::delayMilliseconds(
                            settings.pauseDelayEnabled, settings.pauseDelayText));
            item.insert(QStringLiteral("muted"), media->muted());
            item.insert(QStringLiteral("volume"), media->volume());
            item.insert(QStringLiteral("continuousLoop"), media->repeatEnabled());
            item.insert(QStringLiteral("repeatEnabled"), settings.repeatEnabled);
            item.insert(QStringLiteral("repeatCount"),
                        qMax(1, settings.repeatCountText.toInt()));
            item.insert(QStringLiteral("autoUnmute"), settings.unmuteAutomatically);
            item.insert(QStringLiteral("autoUnmuteDelayMs"),
                        MediaSettingsSerialization::delayMilliseconds(
                            settings.unmuteDelayEnabled, settings.unmuteDelayText));
            item.insert(QStringLiteral("autoMute"), settings.muteDelayEnabled);
            item.insert(QStringLiteral("autoMuteDelayMs"),
                        MediaSettingsSerialization::delayMilliseconds(
                            settings.muteDelayEnabled, settings.muteDelayText));
            item.insert(QStringLiteral("muteWhenVideoEnds"), settings.muteWhenVideoEnds);
            item.insert(QStringLiteral("audioFadeInSeconds"),
                        MediaSettingsSerialization::durationSeconds(
                            settings.audioFadeInEnabled, settings.audioFadeInText));
            item.insert(QStringLiteral("audioFadeOutSeconds"),
                        MediaSettingsSerialization::durationSeconds(
                            settings.audioFadeOutEnabled, settings.audioFadeOutText));
            item.insert(QStringLiteral("startPositionMs"),
                        static_cast<double>(media->positionMs()));
        }
        serializedMedia.append(item);
    }
    root.insert(QStringLiteral("media"), serializedMedia);
    return root;
}

QJsonObject CanvasDocument::serializeProjectState() const
{
    QJsonObject root = serializeSceneState();
    root.remove(QStringLiteral("canvasSessionId"));
    QJsonArray media = root.value(QStringLiteral("media")).toArray();
    for (qsizetype index = 0; index < media.size(); ++index) {
        QJsonObject item = media.at(index).toObject();
        CanvasMedia* source = mediaById(item.value(QStringLiteral("mediaId")).toString());
        if (source) {
            item.insert(QStringLiteral("projectMediaSettings"),
                        MediaSettingsSerialization::toProjectJson(source->settings()));
        }
        media.replace(index, item);
    }
    root.insert(QStringLiteral("media"), media);
    root.insert(QStringLiteral("viewport"), QJsonObject{
        {QStringLiteral("m11"), m_cameraScale},
        {QStringLiteral("m12"), 0.0},
        {QStringLiteral("m21"), 0.0},
        {QStringLiteral("m22"), m_cameraScale},
        {QStringLiteral("dx"), m_cameraPanX},
        {QStringLiteral("dy"), m_cameraPanY},
        {QStringLiteral("centerX"), 0.0},
        {QStringLiteral("centerY"), 0.0}
    });
    return root;
}

bool CanvasDocument::restoreProjectState(
    const QJsonObject& state,
    const QHash<QString, QString>& sourcePathByMediaId,
    QStringList* skippedMediaIds)
{
    if (!m_media.isEmpty()
        || state.value(QStringLiteral("renderSchemaVersion")).toInt(-1) != 2) {
        return false;
    }
    QList<ScreenInfo> restoredScreens;
    for (const QJsonValue& value : state.value(QStringLiteral("screens")).toArray()) {
        if (value.isObject()) restoredScreens.append(ScreenInfo::fromJson(value.toObject()));
    }
    if (!restoredScreens.isEmpty()) setScreens(restoredScreens);

    const auto skip = [skippedMediaIds](const QString& id) {
        if (skippedMediaIds && !id.isEmpty() && !skippedMediaIds->contains(id)) {
            skippedMediaIds->append(id);
        }
    };
    for (const QJsonValue& value : state.value(QStringLiteral("media")).toArray()) {
        const QJsonObject source = value.toObject();
        const QString id = source.value(QStringLiteral("mediaId")).toString().trimmed();
        const QString type = source.value(QStringLiteral("type")).toString().toLower();
        if (id.isEmpty() || (type != QLatin1String("text")
            && type != QLatin1String("image") && type != QLatin1String("video"))) {
            skip(id);
            continue;
        }
        const QSize base(qMax(1, source.value(QStringLiteral("baseWidth")).toInt(
                                  qRound(source.value(QStringLiteral("width")).toDouble(1.0)))),
                         qMax(1, source.value(QStringLiteral("baseHeight")).toInt(
                                  qRound(source.value(QStringLiteral("height")).toDouble(1.0)))));
        CanvasMedia* media = nullptr;
        if (type == QLatin1String("text")) {
            media = new CanvasMedia(CanvasMedia::Type::Text, base);
            // Restore atomically. Fit mode is re-enabled only after all text
            // metrics, alignment and persisted geometry have been applied.
            media->setFitToTextEnabled(false);
            media->setText(source.value(QStringLiteral("text")).toString(QStringLiteral("Text")));
        } else {
            const QString path = sourcePathByMediaId.value(id);
            if (path.isEmpty() || !QFileInfo::exists(path)) {
                skip(id);
                continue;
            }
            media = new CanvasMedia(type == QLatin1String("video")
                                        ? CanvasMedia::Type::Video
                                        : CanvasMedia::Type::Image, base);
            media->setSourcePath(path);
            if (m_fileManager) {
                media->setFileId(m_fileManager->getOrCreateFileId(path));
            }
            if (media->isVideo()) media->initializeVideoRuntime();
        }
        media->restoreMediaId(id);
        if (m_fileManager && !media->isText() && !media->fileId().isEmpty()) {
            m_fileManager->associateMediaWithFile(media->mediaId(), media->fileId());
            if (!m_canvasSessionId.isEmpty()) {
                m_fileManager->associateFileWithIdea(media->fileId(), m_canvasSessionId);
            }
        }
        media->setBaseSize(base);
        media->setPosition({source.value(QStringLiteral("x")).toDouble(),
                            source.value(QStringLiteral("y")).toDouble()});
        qreal scale = source.value(QStringLiteral("width")).toDouble(base.width())
            / qMax(1, base.width());
        if (!std::isfinite(scale) || scale <= 0.0) scale = 1.0;
        media->setScale(scale);
        media->setZ(source.value(QStringLiteral("z")).toDouble(nextZ()));
        media->setContentVisible(source.value(QStringLiteral("visible")).toBool(true));
        media->setContentOpacity(std::clamp(
            source.value(QStringLiteral("contentOpacity")).toDouble(1.0), 0.0, 1.0));

        MediaSettingsState settings;
        settings.displayAutomatically = source.value(QStringLiteral("autoDisplay")).toBool(false);
        const int displayDelay = source.value(QStringLiteral("autoDisplayDelayMs")).toInt();
        settings.displayDelayEnabled = displayDelay > 0;
        settings.displayDelayText = secondsText(displayDelay);
        settings.hideDelayEnabled = source.value(QStringLiteral("autoHide")).toBool(false);
        settings.hideDelayText = secondsText(source.value(QStringLiteral("autoHideDelayMs")).toInt());
        settings.hideWhenVideoEnds = source.value(QStringLiteral("hideWhenVideoEnds")).toBool(false);
        settings.fadeInEnabled = source.value(QStringLiteral("fadeInSeconds")).toDouble() > 0.0;
        settings.fadeInText = QString::number(source.value(QStringLiteral("fadeInSeconds")).toDouble());
        settings.fadeOutEnabled = source.value(QStringLiteral("fadeOutSeconds")).toDouble() > 0.0;
        settings.fadeOutText = QString::number(source.value(QStringLiteral("fadeOutSeconds")).toDouble());
        settings.opacityOverrideEnabled = true;
        settings.opacityText = QString::number(qRound(media->contentOpacity() * 100.0));
        if (media->isVideo()) {
            settings.playAutomatically = source.value(QStringLiteral("autoPlay")).toBool(false);
            const int playDelay = source.value(QStringLiteral("autoPlayDelayMs")).toInt();
            settings.playDelayEnabled = playDelay > 0;
            settings.playDelayText = secondsText(playDelay);
            settings.pauseDelayEnabled = source.value(QStringLiteral("autoPause")).toBool(false);
            settings.pauseDelayText = secondsText(source.value(QStringLiteral("autoPauseDelayMs")).toInt());
            settings.repeatEnabled = source.value(QStringLiteral("repeatEnabled")).toBool(false);
            settings.repeatCountText = QString::number(qMax(1, source.value(QStringLiteral("repeatCount")).toInt(1)));
            settings.unmuteAutomatically = source.value(QStringLiteral("autoUnmute")).toBool(false);
            const int unmuteDelay = source.value(QStringLiteral("autoUnmuteDelayMs")).toInt();
            settings.unmuteDelayEnabled = unmuteDelay > 0;
            settings.unmuteDelayText = secondsText(unmuteDelay);
            settings.muteDelayEnabled = source.value(QStringLiteral("autoMute")).toBool(false);
            settings.muteDelayText = secondsText(source.value(QStringLiteral("autoMuteDelayMs")).toInt());
            settings.muteWhenVideoEnds = source.value(QStringLiteral("muteWhenVideoEnds")).toBool(false);
        }
        MediaSettingsSerialization::fromProjectJson(
            source.value(QStringLiteral("projectMediaSettings")).toObject(), &settings);
        media->setSettings(settings);

        if (media->isText()) {
            media->setFontFamily(source.value(QStringLiteral("fontFamily")).toString(QStringLiteral("Impact")));
            media->setFontPixelSize(qMax(1, source.value(QStringLiteral("fontPixelSize")).toInt(64)));
            media->setFontWeight(source.value(QStringLiteral("fontWeight")).toInt(400));
            media->setItalic(source.value(QStringLiteral("fontItalic")).toBool(false));
            media->setUnderline(source.value(QStringLiteral("fontUnderline")).toBool(false));
            media->setUppercase(source.value(QStringLiteral("fontUppercase")).toBool(false));
            media->setTextColor(QColor(source.value(QStringLiteral("textColor")).toString(QStringLiteral("#FFFFFFFF"))));
            media->setOutlineWidthPercent(source.value(QStringLiteral("textBorderWidthPercent")).toDouble());
            media->setOutlineColor(QColor(source.value(QStringLiteral("textBorderColor")).toString(QStringLiteral("#FF000000"))));
            media->setHighlightEnabled(source.value(QStringLiteral("textHighlightEnabled")).toBool(false));
            media->setHighlightColor(QColor(source.value(QStringLiteral("textHighlightColor")).toString(QStringLiteral("#80FFFF00"))));
            media->setHorizontalAlignment(source.value(QStringLiteral("horizontalAlignment")).toString());
            media->setVerticalAlignment(source.value(QStringLiteral("verticalAlignment")).toString());
            media->setFitToTextEnabled(source.value(QStringLiteral("textFitToTextEnabled")).toBool(true));
            // Persisted project geometry remains authoritative on restore.
            // Fit mode resumes for the next text/style edit, but loading must
            // never silently move or resize an existing project element.
            media->setBaseSize(base);
            media->setPosition({source.value(QStringLiteral("x")).toDouble(),
                                source.value(QStringLiteral("y")).toDouble()});
        } else if (media->isVideo()) {
            media->setMuted(source.value(QStringLiteral("muted")).toBool(false));
            media->setVolume(source.value(QStringLiteral("volume")).toDouble(1.0));
            media->setRepeatEnabled(source.value(QStringLiteral("continuousLoop")).toBool(false));
            media->setPositionMs(qMax<qint64>(0, qRound64(
                source.value(QStringLiteral("startPositionMs")).toDouble())));
        }
        media->setUploadNotUploaded();
        adoptMedia(media);
    }
    const QJsonObject viewport = state.value(QStringLiteral("viewport")).toObject();
    if (!viewport.isEmpty()) {
        setCamera(viewport.value(QStringLiteral("m11")).toDouble(1.0),
                  viewport.value(QStringLiteral("dx")).toDouble(),
                  viewport.value(QStringLiteral("dy")).toDouble());
    }
    clearSelection();
    return true;
}

qreal CanvasDocument::nextZ() const
{
    qreal z = 1.0;
    for (CanvasMedia* media : m_media) {
        if (media) z = std::max(z, media->z() + 1.0);
    }
    return z;
}
