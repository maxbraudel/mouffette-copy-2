#include "backend/domain/media/CanvasMedia.h"
#include "backend/domain/media/TextRenderState.h"

#include <QAudioOutput>
#include <QFileInfo>
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
}

CanvasMedia::~CanvasMedia()
{
    if (m_player) m_player->stop();
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

void CanvasMedia::setSourcePath(const QString& path)
{
    if (m_sourcePath == path) return;
    m_sourcePath = path;
    const QFileInfo source(path);
    m_sourceSizeBytes = source.isFile() ? source.size() : -1;
    if (m_player) m_player->setSource(QUrl::fromLocalFile(path));
    notifyChanged();
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
    m_player = new QMediaPlayer(this);
    m_videoSink = new QVideoSink(this);
    m_audioOutput = new QAudioOutput(this);
    m_player->setAudioOutput(m_audioOutput);
    m_player->setVideoOutput(m_videoSink);
    m_audioOutput->setVolume(1.0);
    connect(m_videoSink, &QVideoSink::videoFrameChanged, this,
            [this](const QVideoFrame& frame) {
        if (frame.isValid()) {
            m_hasRenderedFrame = true;
            m_firstFramePrimed = true;
            emit runtimeStateChanged();
            notifyChanged();
        }
    });
    connect(m_player, &QMediaPlayer::playbackStateChanged,
            this, &CanvasMedia::runtimeStateChanged);
    connect(m_player, &QMediaPlayer::positionChanged,
            this, &CanvasMedia::runtimeStateChanged);
    connect(m_player, &QMediaPlayer::durationChanged,
            this, &CanvasMedia::runtimeStateChanged);
    connect(m_player, &QMediaPlayer::errorChanged,
            this, &CanvasMedia::runtimeStateChanged);
    if (!m_sourcePath.isEmpty()) {
        m_player->setSource(QUrl::fromLocalFile(m_sourcePath));
    }
}

bool CanvasMedia::isPlaying() const
{
    return m_player
        && m_player->playbackState() == QMediaPlayer::PlayingState;
}

bool CanvasMedia::muted() const
{
    return m_audioOutput && m_audioOutput->isMuted();
}

void CanvasMedia::setMuted(bool muted)
{
    if (!m_audioOutput || m_audioOutput->isMuted() == muted) return;
    m_audioOutput->setMuted(muted);
    emit runtimeStateChanged();
}

qreal CanvasMedia::volume() const
{
    return m_audioOutput ? m_audioOutput->volume() : 1.0;
}

void CanvasMedia::setVolume(qreal volume)
{
    if (!m_audioOutput) return;
    m_audioOutput->setVolume(std::clamp<qreal>(volume, 0.0, 1.0));
    emit runtimeStateChanged();
}

void CanvasMedia::setRepeatEnabled(bool enabled)
{
    if (m_repeatEnabled == enabled) return;
    m_repeatEnabled = enabled;
    if (m_player) m_player->setLoops(enabled ? QMediaPlayer::Infinite : 1);
    emit runtimeStateChanged();
}

void CanvasMedia::togglePlayPause()
{
    if (!m_player) return;
    isPlaying() ? m_player->pause() : m_player->play();
}

void CanvasMedia::stopToBeginning()
{
    if (!m_player) return;
    m_player->pause();
    m_player->setPosition(0);
}

void CanvasMedia::seekToRatio(qreal ratio)
{
    if (!m_player || m_player->duration() <= 0) return;
    ratio = std::clamp<qreal>(ratio, 0.0, 1.0);
    m_player->setPosition(qRound64(ratio * m_player->duration()));
}

void CanvasMedia::setPositionMs(qint64 positionMs)
{
    if (m_player) m_player->setPosition(qMax<qint64>(0, positionMs));
}

qint64 CanvasMedia::positionMs() const
{
    return m_player ? m_player->position() : 0;
}

QVariantMap CanvasMedia::toModelMap(qreal unit) const
{
    const qreal safeUnit = unit > 0.0001 ? unit : 1.0;
    QVariantMap map{
        {QStringLiteral("mediaId"), m_mediaId},
        {QStringLiteral("mediaType"), typeName()},
        {QStringLiteral("x"), m_position.x() * safeUnit},
        {QStringLiteral("y"), m_position.y() * safeUnit},
        {QStringLiteral("width"), m_baseSize.width() * safeUnit},
        {QStringLiteral("height"), m_baseSize.height() * safeUnit},
        {QStringLiteral("scale"), m_scale},
        {QStringLiteral("z"), m_z},
        {QStringLiteral("selected"), m_selected},
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
        {QStringLiteral("textOutlineWidthPercent"), renderedOutlineWidthPercent()},
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
        map.insert(QStringLiteral("videoHasPosterFrame"), false);
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
