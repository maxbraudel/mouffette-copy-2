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
    releaseResidencyResources();
}

void CanvasMedia::releaseResidencyResources()
{
    // Invalidate queued acquires even if suspension ends before they run.
    ++m_residencyGeneration;
    if (isVideo()) m_pendingPositionMs = positionMs();
    if (m_player) m_player->clearAsset();
    if (m_videoSink) m_videoSink->setVideoFrame({});
    if (m_residentFrameSource) m_residentFrameSource->clear();
    if (m_residencyAcquired) MediaResidencyManager::instance().release(m_residencyOwnerId);
    m_residencyAcquired = false;
    m_hasRenderedFrame = false;
    m_firstFramePrimed = false;
}

void CanvasMedia::setResidencySuspended(bool suspended)
{
    if (m_residencyRetired || m_residencySuspended == suspended) return;
    m_residencySuspended = suspended;
    if (isText()) return;
    if (suspended) {
        // An accepted import has an identity even when its original request
        // did not specify a digest. Reactivation must validate the same bytes.
        if (m_expectedSha256.isEmpty() && m_identityPublished)
            m_expectedSha256 = m_fileId;
        releaseResidencyResources();
    } else if (!m_sourcePath.isEmpty()) {
        requestResidency();
    }
    emit residencyChanged();
    emit runtimeStateChanged();
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
        emit changed();
    }
}

void CanvasMedia::setFileId(const QString& id)
{
    if (m_fileId == id) return;
    m_fileId = id;
    const QFileInfo source(m_sourcePath);
    m_sourceSizeBytes = source.isFile() ? source.size() : -1;
    emit changed();
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
        ++m_residencyGeneration;
        if (m_residencyAcquired) MediaResidencyManager::instance().release(m_residencyOwnerId);
        m_residencyAcquired = false;
        if (!path.isEmpty()) requestResidency();
        refreshResidency();
    }
    emit changed();
}

void CanvasMedia::requestResidency()
{
    if (m_residencyRetired || m_residencySuspended || m_sourcePath.isEmpty()) return;
    const QString path = m_sourcePath;
    const QString expected = m_expectedSha256;
    const quint64 generation = m_residencyGeneration;
    // Defer even a shared-cache hit until the document has adopted the node
    // and installed identity/invalidation observers.
    QMetaObject::invokeMethod(this, [this, path, expected, generation]() {
        if (m_residencyRetired || m_residencySuspended || m_residencyAcquired
            || m_residencyGeneration != generation
            || m_sourcePath != path || m_expectedSha256 != expected) return;
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
    if (isText()) return QStringLiteral("ready");
    return m_residencySuspended ? QStringLiteral("suspended")
                               : MediaResidencyManager::instance().state(m_residencyOwnerId);
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
    if (isText() || m_residencyRetired || m_residencySuspended) return;
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

void CanvasMedia::setPositionAndScale(const QPointF& position, qreal scale)
{
    if (!std::isfinite(position.x()) || !std::isfinite(position.y())
        || !std::isfinite(scale) || scale <= 0.0001) return;
    if (m_position == position && qFuzzyCompare(m_scale, scale)) return;
    // Centered resizing changes both values together. Observers must see one
    // complete geometry, never an intermediate resize around the old origin.
    m_position = position;
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
    emit presentationChanged();
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

void CanvasMedia::setUploadNotUploaded()
{
    if (m_uploadState == UploadState::NotUploaded && m_uploadProgress == 0) return;
    m_uploadState = UploadState::NotUploaded;
    m_uploadProgress = 0;
    emit uploadStateChanged();
    emit presentationChanged();
}

void CanvasMedia::setUploadUploading(int progress)
{
    progress = std::clamp(progress, 0, 100);
    if (m_uploadState == UploadState::Uploading && m_uploadProgress == progress) return;
    m_uploadState = UploadState::Uploading;
    m_uploadProgress = progress;
    emit uploadStateChanged();
    emit presentationChanged();
}

void CanvasMedia::setUploadUploaded()
{
    if (m_uploadState == UploadState::Uploaded && m_uploadProgress == 100) return;
    m_uploadState = UploadState::Uploaded;
    m_uploadProgress = 100;
    emit uploadStateChanged();
    emit presentationChanged();
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
    if (m_evaluatedElementState && !m_elementDraft) return qRound(m_evaluatedElementState->fontWeight);
    return m_fontWeightOverride ? m_fontWeight : 400;
}

QColor CanvasMedia::renderedTextColor() const
{
    if (m_evaluatedElementState && !m_elementDraft) return m_evaluatedElementState->textColor;
    return m_textColorOverride ? m_textColor : QColor(Qt::white);
}

qreal CanvasMedia::renderedOutlineWidthPercent() const
{
    if (m_evaluatedElementState && !m_elementDraft) return m_evaluatedElementState->outlineWidthPercent;
    return m_outlineWidthOverride ? m_outlineWidthPercent : 0.0;
}

QColor CanvasMedia::renderedOutlineColor() const
{
    if (m_evaluatedElementState && !m_elementDraft) return m_evaluatedElementState->outlineColor;
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
        fitted = m_baseSize.toSize();
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
    if (!isVideo() || m_residencyRetired || m_residencySuspended) return;
    if (m_player) {
        initializeVideoOutputs();
        return;
    }
    m_player = new ResidentVideoPlayer(this);
    connect(m_player, &ResidentVideoPlayer::playbackStateChanged,
            this, &CanvasMedia::runtimeStateChanged);
    connect(m_player, &ResidentVideoPlayer::positionChanged,
            this, &CanvasMedia::runtimeStateChanged);
    connect(m_player, &ResidentVideoPlayer::mediaStatusChanged, this,
            [this](QMediaPlayer::MediaStatus status) {
        if ((status == QMediaPlayer::LoadedMedia || status == QMediaPlayer::BufferedMedia)
            && m_pendingPositionMs >= 0) {
            const qint64 target = m_pendingPositionMs;
            m_pendingPositionMs = -1;
            m_player->setPosition(target);
            emit runtimeStateChanged();
        }

    });
    connect(m_player, &ResidentVideoPlayer::durationChanged, this, [this](qint64 duration) {
        if (duration > 0) m_sourceDurationMs = duration;
        emit runtimeStateChanged();
    });
    connect(m_player, &ResidentVideoPlayer::errorChanged,
            this, &CanvasMedia::runtimeStateChanged);
    initializeVideoOutputs();
}

void CanvasMedia::initializeVideoOutputs()
{
    if (m_audioOutput || m_audioInitializationPending) return;
    m_audioInitializationPending = true;
    auto* audio = new QFutureWatcher<QAudioDevice>(this);
    connect(audio, &QFutureWatcher<QAudioDevice>::finished, this, [this, audio] {
        const QAudioDevice device = audio->result();
        audio->deleteLater();
        m_audioInitializationPending = false;
        if (m_residencyRetired || m_residencySuspended) return;
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
    // Timeline playback gates the device independently of authored mute.
    return m_muted;
}

void CanvasMedia::setMuted(bool muted, bool updateAudioOutput)
{
    if (this->muted() == muted) return;
    m_muted = muted;
    if (m_audioOutput && updateAudioOutput) m_audioOutput->setMuted(muted);
    notifyChanged();
    emit audioStateChanged();
    emit runtimeStateChanged();
}

qreal CanvasMedia::volume() const
{
    return m_volume;
}

void CanvasMedia::setVolume(qreal volume)
{
    if (!std::isfinite(volume)) return;
    const qreal normalized = std::clamp<qreal>(volume, 0.0, 1.0);
    if (qFuzzyCompare(this->volume(), normalized)) return;
    m_volume = normalized;
    if (m_audioOutput) m_audioOutput->setVolume(normalized);
    notifyChanged();
    emit audioStateChanged();
    emit runtimeStateChanged();
}

void CanvasMedia::setPositionMs(qint64 positionMs)
{
    if (!isVideo()) return;
    const qint64 target = qMax<qint64>(0, positionMs);
    // A restored or pasted video can still be loading. Preserve the requested
    // preview position until the decoder is ready to accept the seek.
    if (!m_player || m_player->duration() <= 0 || m_player->mediaStatus() == QMediaPlayer::LoadingMedia
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
    if (m_authorElementState) {
        const bool wasDraft=m_elementDraft;
        m_elementDraft = true;
        emit presentationChanged();
        if (!wasDraft) emit draftChanged();
    } else {
        emit changed();
    }
}

SceneTimeline::ElementState CanvasMedia::captureElementFields() const
{
    SceneTimeline::ElementState s;
    s.type = typeName(); s.position = m_position; s.baseSize = m_baseSize;
    s.scale = m_scale; s.size = m_baseSize * m_scale; s.z = m_z;
    s.visible = m_contentVisible; s.opacity = m_contentOpacity;
    s.opacityOverrideEnabled = m_settings.opacityOverrideEnabled;
    bool ok = false; s.rawOpacity = m_settings.opacityText.toDouble(&ok) / 100.0;
    if (!ok || !std::isfinite(s.rawOpacity)) s.rawOpacity = 1.0;
    s.rawOpacity = qBound<qreal>(0.0, s.rawOpacity, 1.0);
    s.muted = m_muted; s.volume = m_volume;
    s.text = m_text; s.fontFamily = m_fontFamily; s.fontPixelSize = m_fontPixelSize;
    s.fontWeight = renderedFontWeight(); s.rawFontWeight = m_fontWeight;
    s.fontWeightOverrideEnabled = m_fontWeightOverride;
    s.italic = m_italic; s.underline = m_underline; s.uppercase = m_uppercase;
    s.textColor = renderedTextColor(); s.rawTextColor = m_textColor; s.textColorOverrideEnabled = m_textColorOverride;
    s.highlightEnabled = m_highlightEnabled; s.highlightColor = m_highlightColor;
    s.outlineWidthPercent = renderedOutlineWidthPercent(); s.rawOutlineWidthPercent = m_outlineWidthPercent;
    s.outlineWidthOverrideEnabled = m_outlineWidthOverride;
    s.outlineColor = renderedOutlineColor(); s.rawOutlineColor = m_outlineColor; s.outlineColorOverrideEnabled = m_outlineColorOverride;
    s.fitToText = m_fitToText; s.horizontalAlignment = m_horizontalAlignment; s.verticalAlignment = m_verticalAlignment;
    return s;
}
SceneTimeline::ElementState CanvasMedia::authorElementState() const
{
    return m_authorElementState ? *m_authorElementState : captureElementFields();
}
SceneTimeline::ElementState CanvasMedia::displayedElementState() const
{
    return m_evaluatedElementState && !m_elementDraft ? *m_evaluatedElementState : captureElementFields();
}
void CanvasMedia::applyElementFields(const SceneTimeline::ElementState& s)
{
    m_position=s.position; m_scale=s.scale; m_baseSize=s.size/s.scale; m_z=s.z;
    m_contentVisible=s.visible; m_contentOpacity=s.opacity;
    m_settings.opacityOverrideEnabled=s.opacityOverrideEnabled;
    m_settings.opacityText=QString::number(s.rawOpacity*100.0,'g',15);
    m_muted=s.muted; m_volume=s.volume;
    m_settings.volumeOverrideEnabled=true; m_settings.volumeText=QString::number(s.volume*100.0,'g',15);
    m_text=s.text; m_fontFamily=s.fontFamily; m_fontPixelSize=qRound(s.fontPixelSize);
    m_fontWeight=qRound(s.rawFontWeight); m_fontWeightOverride=s.fontWeightOverrideEnabled;
    m_italic=s.italic; m_underline=s.underline; m_uppercase=s.uppercase;
    m_textColor=s.rawTextColor; m_textColorOverride=s.textColorOverrideEnabled;
    m_highlightEnabled=s.highlightEnabled; m_highlightColor=s.highlightColor;
    m_outlineWidthPercent=s.rawOutlineWidthPercent; m_outlineWidthOverride=s.outlineWidthOverrideEnabled;
    m_outlineColor=s.rawOutlineColor; m_outlineColorOverride=s.outlineColorOverrideEnabled;
    m_fitToText=s.fitToText; m_horizontalAlignment=s.horizontalAlignment; m_verticalAlignment=s.verticalAlignment;
}
void CanvasMedia::setElementState(const SceneTimeline::ElementState& state)
{
    const auto author=SceneTimeline::materialize(state);
    if (m_authorElementState) m_authorElementState=author;
    else applyElementFields(author);
    const bool wasDraft=m_elementDraft; m_elementDraft=false;
    emit changed(); if (wasDraft) emit draftChanged();
}
void CanvasMedia::setEvaluatedElementState(const SceneTimeline::ElementState& state)
{
    if (!m_authorElementState) m_authorElementState=captureElementFields();
    const bool wasDraft=m_elementDraft;
    m_evaluatedElementState=state; m_elementDraft=false;
    applyElementFields(state);
    emit presentationChanged(); if (wasDraft) emit draftChanged();
}
void CanvasMedia::clearEvaluatedElementState()
{
    if (!m_authorElementState) return;
    const auto author=*m_authorElementState; const bool wasDraft=m_elementDraft;
    m_authorElementState.reset(); m_evaluatedElementState.reset(); m_elementDraft=false;
    applyElementFields(author);
    emit presentationChanged(); if (wasDraft) emit draftChanged();
}
void CanvasMedia::beginElementEdit()
{
    if (!m_authorElementState || m_elementDraft) return;
    const auto materialized=SceneTimeline::materialize(displayedElementState());
    m_evaluatedElementState=materialized;
    applyElementFields(materialized);
}
void CanvasMedia::setTimelineTrack(const SceneTimeline::MediaTrack& track)
{
    if (m_timelineTrack.toJson()==track.toJson()) return;
    m_timelineTrack=track;
    emit changed();
}
void CanvasMedia::ensureDefaultVideoClip(qint64 maximum)
{
    if (!isVideo() || m_timelineTrack.clipsInitialized || !m_player || m_player->duration()<=0) return;
    auto track=m_timelineTrack;
    track.clipsInitialized=true;
    track.clips.append({SceneTimeline::newId(),0,0,qMin(maximum,m_player->duration())});
    setTimelineTrack(track);
}

void CanvasMedia::restoreSourceDurationMs(qint64 duration)
{
    if (isVideo() && duration >= 0 && duration <= SceneTimeline::MaximumSupportedDurationMs)
        m_sourceDurationMs = duration;
}
