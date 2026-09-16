#include "backend/domain/media/CanvasMedia.h"
#include "backend/domain/media/TextRenderState.h"
#include "backend/media/MediaResidencyManager.h"
#include "backend/media/MediaBackendBootstrap.h"
#include "shared/rendering/MediaFrameSource.h"

#include <QAudioOutput>
#include <QAudioDevice>
#include <QFileInfo>
#include <QFutureWatcher>
#include <QUrl>
#include <QUuid>
#include <QVideoFrame>
#include <QVideoSink>

#include <algorithm>
#include <cmath>

namespace {
QString uploadStateName(CanvasMedia::UploadState state)
{
    switch (state) {
    case CanvasMedia::UploadState::Uploading: return QStringLiteral("uploading");
    case CanvasMedia::UploadState::Uploaded: return QStringLiteral("uploaded");
    case CanvasMedia::UploadState::NotUploaded: return QStringLiteral("not-uploaded");
    }
    return QStringLiteral("not-uploaded");
}
}

CanvasMedia::CanvasMedia(Type type, const QSize& baseSize, QObject* parent)
    : QObject(parent)
    , m_type(type)
    , m_mediaId(QUuid::createUuid().toString(QUuid::WithoutBraces))
    , m_baseSize(baseSize.expandedTo(QSize(1, 1)))
{
    m_residencyOwnerId = QStringLiteral("canvas:") + QUuid::createUuid().toString(QUuid::WithoutBraces);
    if (!isText()) {
        m_residentFrameSource = new RemoteVideoFrameSource(this);
        connect(&MediaResidencyManager::instance(), &MediaResidencyManager::ownerChanged,
                this, [this](const QString& owner) {
            if (owner == m_residencyOwnerId) refreshResidency();
        });
    }
}

CanvasMedia::~CanvasMedia()
{
    if (m_player) m_player->stop();
    retireResidency();
}

void CanvasMedia::retireResidency()
{
    if (isText() || m_residencyRetired) return;
    m_residencyRetired = true;
    if (m_player) m_player->clearAsset();
    if (m_residentFrameSource) m_residentFrameSource->clear();
    if (m_residencyAcquired) MediaResidencyManager::instance().release(m_residencyOwnerId);
    m_residencyAcquired = false;
}

QString CanvasMedia::typeName() const
{
    switch (m_type) {
    case Type::Image: return QStringLiteral("image");
    case Type::Video: return QStringLiteral("video");
    case Type::Text: return QStringLiteral("text");
    }
    return QStringLiteral("image");
}

void CanvasMedia::restoreMediaId(const QString& id)
{
    if (!id.trimmed().isEmpty() && m_mediaId != id.trimmed()) {
        m_mediaId = id.trimmed();
        notifyChanged();
    }
}

void CanvasMedia::setFileId(const QString& id)
{
    if (m_fileId == id) return;
    m_fileId = id;
    const QFileInfo source(m_sourcePath);
    m_sourceSizeBytes = source.isFile() ? source.size() : -1;
    notifyChanged();
}

void CanvasMedia::setSourcePath(const QString& path, const QString& expectedSha256)
{
    if (m_sourcePath == path && m_expectedSha256 == expectedSha256) return;
    m_sourcePath = path;
    m_expectedSha256 = expectedSha256;
    m_identityPublished = false;
    m_sourceInvalidationReported = false;
    m_fileId = expectedSha256;
    m_pendingPositionMs = -1;
    const QFileInfo source(path);
    m_sourceSizeBytes = source.isFile() ? source.size() : -1;
    if (!isText()) {
        if (m_residencyAcquired) MediaResidencyManager::instance().release(m_residencyOwnerId);
        m_residencyAcquired = false;
        if (!path.isEmpty()) requestResidency();
        refreshResidency();
    }
    notifyChanged();
}

void CanvasMedia::requestResidency()
{
    const QString path = m_sourcePath;
    const QString expected = m_expectedSha256;
    // Defer even a shared-cache hit until the document has adopted the node
    // and installed identity/invalidation observers.
    QMetaObject::invokeMethod(this, [this, path, expected]() {
        if (m_residencyRetired || m_sourcePath != path || m_expectedSha256 != expected) return;
        m_residencyAcquired = true;
        MediaResidencyManager::instance().acquire(m_residencyOwnerId, path, expected);
        refreshResidency();
    }, Qt::QueuedConnection);
}

bool CanvasMedia::residencyReady() const
{
    return isText() || MediaResidencyManager::instance().ready(m_residencyOwnerId);
}

QString CanvasMedia::residencyState() const
{
    return isText() ? QStringLiteral("ready") : MediaResidencyManager::instance().state(m_residencyOwnerId);
}

double CanvasMedia::residencyProgress() const
{
    return isText() ? 1.0 : MediaResidencyManager::instance().progress(m_residencyOwnerId);
}

QString CanvasMedia::residencyError() const
{
    return isText() ? QString() : MediaResidencyManager::instance().errorString(m_residencyOwnerId);
}

void CanvasMedia::refreshResidency()
{
    if (isText() || m_residencyRetired) return;
    auto& manager = MediaResidencyManager::instance();
    const QString digest = manager.sha256(m_residencyOwnerId);
    const bool mismatched = !m_expectedSha256.isEmpty() && !digest.isEmpty()
        && digest != m_expectedSha256;
    if (mismatched && !m_sourceInvalidationReported) {
        m_sourceInvalidationReported = true;
        emit sourceInvalidated(manager.errorString(m_residencyOwnerId));
    }
    if (!digest.isEmpty() && !mismatched && !m_identityPublished) {
        m_identityPublished = true;
        if (digest != m_fileId) setFileId(digest);
        emit identityReady(digest);
    }
    const auto asset = manager.asset(m_residencyOwnerId);
    if (manager.ready(m_residencyOwnerId) && asset) {
        if (isVideo()) {
            initializeVideoRuntime();
            // Publish a playable asset only after audio discovery completes,
            // so the first Play never starts silently and changes clocks later.
            if (m_audioOutput && m_player->asset() != asset) m_player->setAsset(asset);
        } else if (m_residentFrameSource) {
            m_residentFrameSource->setFrame(asset->image);
        }
    } else {
        if (m_player && m_player->asset()) m_player->clearAsset();
        if (m_residentFrameSource) m_residentFrameSource->clear();
        m_hasRenderedFrame = false;
        m_firstFramePrimed = false;
    }
    emit residencyChanged();
    emit runtimeStateChanged();
}

QString CanvasMedia::displayName() const
{
    if (isText()) return m_text.simplified().left(48).isEmpty()
        ? QStringLiteral("Text") : m_text.simplified().left(48);
    const QString name = QFileInfo(m_sourcePath).fileName();
    return name.isEmpty() ? typeName() : name;
}

void CanvasMedia::setBaseSize(const QSize& size)
{
    const QSize safe = size.expandedTo(QSize(1, 1));
    if (m_baseSize == safe) return;
    m_baseSize = safe;
    notifyChanged();
}

void CanvasMedia::setPosition(const QPointF& position)
{
    if (m_position == position) return;
    m_position = position;
    notifyChanged();
}

void CanvasMedia::setScale(qreal scale)
{
    if (!std::isfinite(scale) || scale <= 0.0001) return;
    if (qFuzzyCompare(m_scale, scale)) return;
    m_scale = scale;
    notifyChanged();
}

void CanvasMedia::setZ(qreal z)
{
    if (!std::isfinite(z) || qFuzzyCompare(m_z, z)) return;
    m_z = z;
    notifyChanged();
}

QRectF CanvasMedia::sceneRect() const
{
    return {m_position,
            QSizeF(m_baseSize.width() * m_scale,
                   m_baseSize.height() * m_scale)};
}

void CanvasMedia::setSelected(bool selected)
{
    if (m_selected == selected) return;
    m_selected = selected;
    notifyChanged();
}

void CanvasMedia::setContentVisible(bool visible)
{
    if (m_contentVisible == visible) return;
    m_contentVisible = visible;
    notifyChanged();
}

void CanvasMedia::setContentOpacity(qreal opacity)
{
    opacity = std::clamp<qreal>(opacity, 0.0, 1.0);
    if (qFuzzyCompare(1.0 + m_contentOpacity, 1.0 + opacity)) return;
    m_contentOpacity = opacity;
    notifyChanged();
}

void CanvasMedia::setAnimatedDisplayOpacity(qreal opacity)
{
    opacity = std::clamp<qreal>(opacity, 0.0, 1.0);
    if (qFuzzyCompare(1.0 + m_animatedDisplayOpacity, 1.0 + opacity)) return;
    m_animatedDisplayOpacity = opacity;
    notifyChanged();
}

void CanvasMedia::setUploadNotUploaded()
{
    if (m_uploadState == UploadState::NotUploaded && m_uploadProgress == 0) return;
    m_uploadState = UploadState::NotUploaded;
    m_uploadProgress = 0;
    emit uploadStateChanged();
    notifyChanged();
}

void CanvasMedia::setUploadUploading(int progress)
{
    progress = std::clamp(progress, 0, 100);
    if (m_uploadState == UploadState::Uploading && m_uploadProgress == progress) return;
    m_uploadState = UploadState::Uploading;
    m_uploadProgress = progress;
    emit uploadStateChanged();
    notifyChanged();
}

void CanvasMedia::setUploadUploaded()
{
    if (m_uploadState == UploadState::Uploaded && m_uploadProgress == 100) return;
    m_uploadState = UploadState::Uploaded;
    m_uploadProgress = 100;
    emit uploadStateChanged();
    notifyChanged();
}

void CanvasMedia::setSettings(const MediaSettingsState& settings)
{
    m_settings = settings;
    qreal opacity = 1.0;
    if (settings.opacityOverrideEnabled) {
        bool ok = false;
        const qreal percent = settings.opacityText.trimmed().toDouble(&ok);
        if (ok && std::isfinite(percent)) opacity = percent / 100.0;
    }
    setContentOpacity(opacity);

    if (isVideo()) {
        qreal volume = 1.0;
        if (settings.volumeOverrideEnabled) {
            bool ok = false;
            const qreal percent = settings.volumeText.trimmed().toDouble(&ok);
            if (ok && std::isfinite(percent)) volume = percent / 100.0;
        }
        setVolume(volume);
    }
    notifyChanged();
}

#define CANVAS_MEDIA_SETTER(TypeName, Method, Field) \
void CanvasMedia::Method(TypeName value) { if (Field == value) return; Field = value; notifyChanged(); }

CANVAS_MEDIA_SETTER(bool, setUnderline, m_underline)
CANVAS_MEDIA_SETTER(bool, setHighlightEnabled, m_highlightEnabled)

#undef CANVAS_MEDIA_SETTER

int CanvasMedia::renderedFontWeight() const
{
    return m_fontWeightOverride ? m_fontWeight : 400;
}

QColor CanvasMedia::renderedTextColor() const
{
    return m_textColorOverride ? m_textColor : QColor(Qt::white);
}

qreal CanvasMedia::renderedOutlineWidthPercent() const
{
    return m_outlineWidthOverride ? m_outlineWidthPercent : 0.0;
}

QColor CanvasMedia::renderedOutlineColor() const
{
    return m_outlineColorOverride ? m_outlineColor : QColor(Qt::black);
}

void CanvasMedia::setFontWeightOverrideEnabled(bool enabled)
{
    if (m_fontWeightOverride == enabled) return;
    m_fontWeightOverride = enabled;
    notifyTextMetricsChanged();
}

void CanvasMedia::setTextColorOverrideEnabled(bool enabled)
{
    if (m_textColorOverride == enabled) return;
    m_textColorOverride = enabled;
    notifyChanged();
}

void CanvasMedia::setOutlineWidthOverrideEnabled(bool enabled)
{
    if (m_outlineWidthOverride == enabled) return;
    m_outlineWidthOverride = enabled;
    notifyTextMetricsChanged();
}

void CanvasMedia::setOutlineColorOverrideEnabled(bool enabled)
{
    if (m_outlineColorOverride == enabled) return;
    m_outlineColorOverride = enabled;
    notifyChanged();
}

void CanvasMedia::setText(const QString& text)
{
    if (m_text == text) return;
    m_text = text;
    notifyTextMetricsChanged();
}

void CanvasMedia::setFontFamily(const QString& family)
{
    if (m_fontFamily == family) return;
    m_fontFamily = family;
    notifyTextMetricsChanged();
}

void CanvasMedia::setItalic(bool italic)
{
    if (m_italic == italic) return;
    m_italic = italic;
    notifyTextMetricsChanged();
}

void CanvasMedia::setUppercase(bool uppercase)
{
    if (m_uppercase == uppercase) return;
    m_uppercase = uppercase;
    notifyTextMetricsChanged();
}

void CanvasMedia::setFitToTextEnabled(bool enabled)
{
    if (m_fitToText == enabled) return;
    m_fitToText = enabled;
    if (enabled) updateFitToTextGeometry();
    notifyChanged();
}

void CanvasMedia::fitTextToContent()
{
    if (updateFitToTextGeometry()) notifyChanged();
}

void CanvasMedia::setFontPixelSize(int size)
{
    size = qBound(1, size, 2048);
    if (m_fontPixelSize == size) return;
    m_fontPixelSize = size;
    notifyTextMetricsChanged();
}

void CanvasMedia::setFontWeight(int weight)
{
    weight = qBound(1, weight, 1000);
    if (m_fontWeight == weight) return;
    m_fontWeight = weight;
    notifyTextMetricsChanged();
}

void CanvasMedia::setTextColor(const QColor& color)
{
    if (!color.isValid() || color == m_textColor) return;
    m_textColor = color;
    notifyChanged();
}

void CanvasMedia::setHighlightColor(const QColor& color)
{
    if (!color.isValid() || color == m_highlightColor) return;
    m_highlightColor = color;
    notifyChanged();
}

void CanvasMedia::setOutlineWidthPercent(qreal width)
{
    if (!std::isfinite(width)) return;
    width = qBound<qreal>(0.0, width, 100.0);
    if (qFuzzyCompare(1.0 + m_outlineWidthPercent, 1.0 + width)) return;
    m_outlineWidthPercent = width;
    notifyTextMetricsChanged();
}

void CanvasMedia::notifyTextMetricsChanged()
{
    if (m_fitToText) updateFitToTextGeometry();
    notifyChanged();
}

bool CanvasMedia::updateFitToTextGeometry()
{
    if (!isText() || !m_fitToText) return false;

    TextRenderState state;
    state.text = m_text;
    state.fontFamily = m_fontFamily;
    state.fontPixelSize = m_fontPixelSize;
    state.fontWeight = renderedFontWeight();
    state.italic = m_italic;
    state.underline = m_underline;
    state.uppercase = m_uppercase;
    state.fitToTextEnabled = true;
    state.outlineWidthPercent = renderedOutlineWidthPercent();
    state.outlineWidthPixels = TextRenderMetrics::outlinePixels(
        state.outlineWidthPercent, m_fontPixelSize);

    QSize fitted = TextRenderMetrics::fittedTextSize(state);
    if (qAbs(fitted.width() - m_baseSize.width()) <= 1
        && qAbs(fitted.height() - m_baseSize.height()) <= 1) {
        fitted = m_baseSize;
    }
    if (fitted == m_baseSize) return false;

    const qreal anchorX = m_horizontalAlignment == QLatin1String("left")
        ? 0.0 : (m_horizontalAlignment == QLatin1String("right") ? 1.0 : 0.5);
    const qreal anchorY = m_verticalAlignment == QLatin1String("top")
        ? 0.0 : (m_verticalAlignment == QLatin1String("bottom") ? 1.0 : 0.5);
    const QPointF anchorBefore(
        m_position.x() + m_baseSize.width() * m_scale * anchorX,
        m_position.y() + m_baseSize.height() * m_scale * anchorY);
    m_baseSize = fitted;
    m_position = QPointF(
        anchorBefore.x() - m_baseSize.width() * m_scale * anchorX,
        anchorBefore.y() - m_baseSize.height() * m_scale * anchorY);
    return true;
}

void CanvasMedia::setOutlineColor(const QColor& color)
{
    if (!color.isValid() || color == m_outlineColor) return;
    m_outlineColor = color;
    notifyChanged();
}

void CanvasMedia::setHorizontalAlignment(const QString& alignment)
{
    const QString value = alignment == QLatin1String("left")
        || alignment == QLatin1String("right") ? alignment
                                                : QStringLiteral("center");
    if (m_horizontalAlignment == value) return;
    m_horizontalAlignment = value;
    notifyChanged();
}

void CanvasMedia::setVerticalAlignment(const QString& alignment)
{
    const QString value = alignment == QLatin1String("top")
        || alignment == QLatin1String("bottom") ? alignment
                                                 : QStringLiteral("center");
    if (m_verticalAlignment == value) return;
    m_verticalAlignment = value;
    notifyChanged();
}

void CanvasMedia::initializeVideoRuntime()
{
    if (!isVideo() || m_player) return;
    m_player = new ResidentVideoPlayer(this);
    connect(m_player, &ResidentVideoPlayer::playbackStateChanged,
            this, &CanvasMedia::runtimeStateChanged);
    connect(m_player, &ResidentVideoPlayer::positionChanged,
            this, &CanvasMedia::runtimeStateChanged);
    connect(m_player, &ResidentVideoPlayer::positionChanged, this,
            [this](qint64 position) { enforcePlaybackEnd(position); });
    connect(m_player, &ResidentVideoPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
        if ((status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia)
            && m_pendingPositionMs >= 0) {
            const qint64 target = m_pendingPositionMs;
            m_pendingPositionMs = -1;
            m_player->setPosition(target);
            emit runtimeStateChanged();
        }
        if (status == QMediaPlayer::EndOfMedia) {
            // The multimedia backend still finalizes its stopped state while
            // delivering EndOfMedia. Restart after that transition has settled.
            QMetaObject::invokeMethod(this, [this]() {
                if (m_player->mediaStatus() == QMediaPlayer::EndOfMedia)
                    enforcePlaybackEnd(m_player->duration(), true);
            }, Qt::QueuedConnection);
        }
    });
    connect(m_player, &ResidentVideoPlayer::durationChanged,
            this, &CanvasMedia::runtimeStateChanged);
    connect(m_player, &ResidentVideoPlayer::errorChanged,
            this, &CanvasMedia::runtimeStateChanged);
    updateVideoLoops();
    auto* audio = new QFutureWatcher<QAudioDevice>(this);
    connect(audio, &QFutureWatcher<QAudioDevice>::finished, this, [this, audio] {
        const QAudioDevice device = audio->result();
        audio->deleteLater();
        if (m_residencyRetired) return;
        // QObjects stay on the GUI thread. The potentially slow enumeration
        // above returns only the thread-safe, implicitly shared device value.
        // The first QVideoSink also initializes Qt's platform backend. Create
        // it only after discovery has warmed that backend off the GUI thread.
        m_videoSink = new QVideoSink(this);
        m_player->setVideoOutput(m_videoSink);
        connect(m_videoSink, &QVideoSink::videoFrameChanged, this,
                [this](const QVideoFrame& frame) {
            if (frame.isValid()) {
                m_hasRenderedFrame = true;
                m_firstFramePrimed = true;
                emit runtimeStateChanged();
            }
        });
        m_audioOutput = new QAudioOutput(device, this);
        m_audioOutput->setMuted(m_muted);
        m_audioOutput->setVolume(m_volume);
        m_player->setAudioOutput(m_audioOutput);
        refreshResidency();
    });
    audio->setFuture(MediaBackendBootstrap::defaultAudioOutput());
}

bool CanvasMedia::isPlaying() const
{
    return m_player
        && m_player->playbackState() == QMediaPlayer::PlayingState;
}

bool CanvasMedia::muted() const
{
    return m_audioOutput ? m_audioOutput->isMuted() : m_muted;
}

void CanvasMedia::setMuted(bool muted)
{
    if (this->muted() == muted) return;
    m_muted = muted;
    if (m_audioOutput) m_audioOutput->setMuted(muted);
    emit audioStateChanged();
    emit runtimeStateChanged();
}

qreal CanvasMedia::volume() const
{
    return m_audioOutput ? m_audioOutput->volume() : m_volume;
}

void CanvasMedia::setVolume(qreal volume)
{
    if (!std::isfinite(volume)) return;
    const qreal normalized = std::clamp<qreal>(volume, 0.0, 1.0);
    if (qFuzzyCompare(this->volume(), normalized)) return;
    m_volume = normalized;
    if (m_audioOutput) m_audioOutput->setVolume(normalized);
    emit audioStateChanged();
    emit runtimeStateChanged();
}

void CanvasMedia::setRepeatEnabled(bool enabled)
{
    if (m_repeatEnabled == enabled) return;
    m_repeatEnabled = enabled;
    updateVideoLoops();
    emit runtimeStateChanged();
}

bool CanvasMedia::setPlaybackRange(qint64 startMs, qint64 endMs)
{
    constexpr qint64 maximum = 7LL * 24 * 60 * 60 * 1000;
    if (!isVideo() || startMs < -1 || endMs < -1
        || startMs > maximum || endMs > maximum
        || (endMs >= 0 && endMs <= qMax<qint64>(0, startMs))) return false;
    if (m_startMarkerMs == startMs && m_endMarkerMs == endMs) return true;
    m_startMarkerMs = startMs;
    m_endMarkerMs = endMs;
    updateVideoLoops();
    notifyChanged();
    emit runtimeStateChanged();
    return true;
}

bool CanvasMedia::canPlaceStart() const
{
    return m_player && m_player->duration() > 0
        && positionMs() < playbackEndMs();
}

bool CanvasMedia::canPlaceEnd() const
{
    return m_player && m_player->duration() > 0
        && positionMs() > playbackStartMs();
}

qint64 CanvasMedia::playbackStartMs() const
{
    const qint64 start = qMax<qint64>(0, m_startMarkerMs);
    return m_player && m_player->duration() > 0
        ? qMin(start, m_player->duration() - 1) : start;
}

qint64 CanvasMedia::playbackEndMs() const
{
    const qint64 duration = m_player ? m_player->duration() : 0;
    return m_endMarkerMs >= 0 ? (duration > 0 ? qMin(m_endMarkerMs, duration)
                                                           : m_endMarkerMs)
                             : duration;
}

void CanvasMedia::updateVideoLoops()
{
    if (m_player) m_player->setLoops(m_repeatEnabled
        && m_startMarkerMs < 0 && m_endMarkerMs < 0
        ? QMediaPlayer::Infinite : QMediaPlayer::Once);
}

bool CanvasMedia::repeatAvailable() const
{
    return m_repeatEnabled || (m_scenePlayback && m_repeatRemaining > 0);
}

void CanvasMedia::beginScenePlayback()
{
    if (!m_player) return;
    m_player->pause();
    m_scenePlayback = true;
    m_repeatRemaining = m_settings.repeatEnabled
        ? qMax(1, m_settings.repeatCountText.toInt()) : 0;
    setPositionMs(playbackStartMs());
}

void CanvasMedia::endScenePlayback()
{
    m_scenePlayback = false;
    m_repeatRemaining = 0;
}

void CanvasMedia::enforcePlaybackEnd(qint64 position, bool atEnd)
{
    if (!m_player || m_handlingPlaybackEnd || (!isPlaying() && !atEnd)) return;
    const qint64 end = playbackEndMs();
    if (end <= 0 || position < end
        || (atEnd && m_player->mediaStatus() != QMediaPlayer::EndOfMedia)) return;
    if (m_repeatEnabled && m_startMarkerMs < 0 && m_endMarkerMs < 0) return;
    // Natural EOF has its own deferred status handler. Seeking during the last
    // position notification would let the backend stop our newly started loop.
    if (!atEnd && end >= m_player->duration()) return;
    // Paused scrubbing remains unrestricted; only active playback consumes a range.
    m_handlingPlaybackEnd = true;
    if (repeatAvailable()) {
        if (!m_repeatEnabled) --m_repeatRemaining;
        setPositionMs(playbackStartMs());
        m_player->play();
    } else if (m_endMarkerMs >= 0) {
        m_player->pause();
        m_player->setPosition(end);
    }
    m_handlingPlaybackEnd = false;
}

void CanvasMedia::togglePlayPause()
{
    if (!m_player) return;
    if (isPlaying()) {
        m_player->pause();
    } else {
        if (positionMs() < playbackStartMs() || positionMs() >= playbackEndMs())
            setPositionMs(playbackStartMs());
        m_player->play();
    }
}

void CanvasMedia::stopToBeginning()
{
    if (!m_player) return;
    m_player->pause();
    setPositionMs(playbackStartMs());
}

void CanvasMedia::seekToRatio(qreal ratio)
{
    if (!m_player || m_player->duration() <= 0) return;
    ratio = std::clamp<qreal>(ratio, 0.0, 1.0);
    setPositionMs(qRound64(ratio * m_player->duration()));
}

void CanvasMedia::setPositionMs(qint64 positionMs)
{
    if (!m_player) return;
    const qint64 target = qMax<qint64>(0, positionMs);
    // A restored or pasted video can still be loading. Preserve the requested
    // preview position until the decoder is ready to accept the seek.
    if (m_player->duration() <= 0 || m_player->mediaStatus() == QMediaPlayer::LoadingMedia
        || m_player->mediaStatus() == QMediaPlayer::NoMedia) {
        m_pendingPositionMs = target;
    } else {
        m_pendingPositionMs = -1;
        m_player->setPosition(target);
    }
    emit runtimeStateChanged();
}

qint64 CanvasMedia::positionMs() const
{
    return m_pendingPositionMs >= 0 ? m_pendingPositionMs : (m_player ? m_player->position() : 0);
}

QVariantMap CanvasMedia::toModelMap(qreal unit) const
{
    const qreal safeUnit = unit > 0.0001 ? unit : 1.0;
    QVariantMap map{
        {QStringLiteral("mediaId"), m_mediaId},
        {QStringLiteral("canvasMedia"), true},
        {QStringLiteral("mediaType"), typeName()},
        {QStringLiteral("x"), m_position.x() * safeUnit},
        {QStringLiteral("y"), m_position.y() * safeUnit},
        {QStringLiteral("width"), m_baseSize.width() * safeUnit},
        {QStringLiteral("height"), m_baseSize.height() * safeUnit},
        {QStringLiteral("scale"), m_scale},
        {QStringLiteral("z"), m_z},
        {QStringLiteral("selected"), m_selected},
        {QStringLiteral("residencyReady"), residencyReady()},
        {QStringLiteral("residencyState"), residencyState()},
        {QStringLiteral("residencyProgress"), residencyProgress()},
        {QStringLiteral("residencyError"), residencyError()},
        {QStringLiteral("residentFrameSource"), QVariant::fromValue<QObject*>(m_residentFrameSource)},
        {QStringLiteral("sourceSizeBytes"), m_sourceSizeBytes},
        {QStringLiteral("sourcePath"), m_sourcePath},
        {QStringLiteral("sourceUrl"), m_sourcePath.isEmpty()
             ? QString() : QUrl::fromLocalFile(m_sourcePath).toString()},
        {QStringLiteral("uploadState"), uploadStateName(m_uploadState)},
        {QStringLiteral("uploadProgress"), m_uploadProgress},
        {QStringLiteral("displayName"), displayName()},
        {QStringLiteral("contentVisible"), m_contentVisible},
        {QStringLiteral("contentOpacity"), m_contentOpacity},
        {QStringLiteral("animatedDisplayOpacity"), m_animatedDisplayOpacity},
        {QStringLiteral("textContent"), isText() ? m_text : QString()},
        {QStringLiteral("textEditable"), isText()},
        {QStringLiteral("textHorizontalAlignment"), m_horizontalAlignment},
        {QStringLiteral("textVerticalAlignment"), m_verticalAlignment},
        {QStringLiteral("fitToTextEnabled"), m_fitToText},
        {QStringLiteral("textFontFamily"), m_fontFamily},
        {QStringLiteral("textFontPixelSize"), m_fontPixelSize},
        {QStringLiteral("textFontWeight"), renderedFontWeight()},
        {QStringLiteral("textItalic"), m_italic},
        {QStringLiteral("textUnderline"), m_underline},
        {QStringLiteral("textUppercase"), m_uppercase},
        {QStringLiteral("textColor"), renderedTextColor().name(QColor::HexArgb)},
        {QStringLiteral("textOutlineWidthPx"),
             TextRenderMetrics::outlinePixels(renderedOutlineWidthPercent(),
                                               m_fontPixelSize)},
        {QStringLiteral("textOutlineColor"), renderedOutlineColor().name(QColor::HexArgb)},
        {QStringLiteral("textHighlightEnabled"), m_highlightEnabled},
        {QStringLiteral("textHighlightColor"), m_highlightColor.name(QColor::HexArgb)}
    };
    if (isVideo()) {
        map.insert(QStringLiteral("videoPlayerPtr"),
                   QVariant::fromValue<QObject*>(m_player));
        map.insert(QStringLiteral("videoSinkPtr"),
                   QVariant::fromValue<QObject*>(m_videoSink));
        map.insert(QStringLiteral("videoHasRenderedFrame"), m_hasRenderedFrame);
        map.insert(QStringLiteral("videoFirstFramePrimed"), m_firstFramePrimed);
        map.insert(QStringLiteral("videoPlaybackErrorCode"),
                   m_player ? static_cast<int>(m_player->error()) : 0);
        map.insert(QStringLiteral("videoPlaybackErrorString"),
                   m_player ? m_player->errorString() : QString());
    }
    return map;
}

void CanvasMedia::notifyChanged()
{
    emit changed();
}
