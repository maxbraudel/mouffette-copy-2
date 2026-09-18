#include "SceneTimeline.h"
#include "backend/domain/media/TextRenderState.h"

#include <QJsonArray>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <cmath>
#include <limits>

namespace SceneTimeline {
namespace {
bool fail(QString* error, const QString& message) { if (error) *error = message; return false; }
bool onlyKeys(const QJsonObject& object, const QStringList& keys) {
    if (object.size() != keys.size()) return false;
    for (const QString& key : keys) if (!object.contains(key)) return false;
    return true;
}
bool number(const QJsonObject& o, const char* key, double low, double high, double* out) {
    const auto v = o.value(QLatin1String(key));
    if (!v.isDouble() || !std::isfinite(v.toDouble()) || v.toDouble() < low || v.toDouble() > high) return false;
    *out = v.toDouble(); return true;
}
bool integer(const QJsonObject& o, const char* key, qint64 low, qint64 high, qint64* out) {
    double n; if (!number(o, key, low, high, &n) || std::floor(n) != n) return false;
    *out = static_cast<qint64>(n); return true;
}
bool boolean(const QJsonObject& o, const char* key, bool* out) {
    auto v = o.value(QLatin1String(key)); if (!v.isBool()) return false; *out = v.toBool(); return true;
}
bool color(const QJsonObject& o, const char* key, QColor* out) {
    auto v = o.value(QLatin1String(key)); if (!v.isString()) return false;
    const QString text=v.toString();
    if (text.size()!=9 || text[0]!=QLatin1Char('#')) return false;
    for (qsizetype i=1;i<text.size();++i) {
        const auto c=text[i].toLower();
        if (!(c>=QLatin1Char('0') && c<=QLatin1Char('9')) && !(c>=QLatin1Char('a') && c<=QLatin1Char('f'))) return false;
    }
    QColor c(text); if (!c.isValid()) return false; *out = c; return true;
}
qreal blend(qreal a, qreal b, qreal f) { return a + (b - a) * f; }
QColor blendColor(const QColor& a, const QColor& b, qreal f) {
    return QColor::fromRgbF(blend(a.redF(), b.redF(), f), blend(a.greenF(), b.greenF(), f),
                            blend(a.blueF(), b.blueF(), f), blend(a.alphaF(), b.alphaF(), f));
}
void sortTrack(MediaTrack& t) {
    std::sort(t.keyframes.begin(), t.keyframes.end(), [](const Keyframe& a, const Keyframe& b) { return a.slot < b.slot; });
}
bool validClip(const Clip& c, qint64 maximum) {
    return !c.id.isEmpty() && c.startSlot >= 0 && c.startSlot < maximum
        && c.durationSlots > 0 && c.durationSlots <= maximum - c.startSlot
        && (!c.sourceStartSlot || (*c.sourceStartSlot >= -MaximumSourceOffsetSlots
            && *c.sourceStartSlot <= MaximumSourceOffsetSlots - c.durationSlots));
}
}
QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

QJsonObject ElementState::toJson() const {
    QJsonObject o{
        {"type", type}, {"x", position.x()}, {"y", position.y()},
        {"width", size.width()}, {"height", size.height()},
        {"baseWidth", baseSize.width()}, {"baseHeight", baseSize.height()}, {"scale", scale},
        {"visible", visible}, {"contentOpacity", opacity},
        {"opacityOverrideEnabled", opacityOverrideEnabled}, {"rawOpacity", rawOpacity}
    };
    if (type == QLatin1String("video")) { o.insert("muted", muted); o.insert("volume", volume); }
    if (type == QLatin1String("text")) {
        o.insert("text", text); o.insert("fontFamily", fontFamily); o.insert("fontPixelSize", fontPixelSize);
        o.insert("fontWeight", fontWeight); o.insert("fontItalic", italic); o.insert("fontUnderline", underline);
        o.insert("fontUppercase", uppercase); o.insert("textColor", textColor.name(QColor::HexArgb));
        o.insert("textOutlineWidthPx", TextRenderMetrics::outlinePixels(outlineWidthPercent, qRound(fontPixelSize)));
        o.insert("textOutlineWidthPercent", outlineWidthPercent);
        o.insert("textBorderColor", outlineColor.name(QColor::HexArgb));
        o.insert("textHighlightEnabled", highlightEnabled); o.insert("textHighlightColor", highlightColor.name(QColor::HexArgb));
        o.insert("textFitToTextEnabled", fitToText); o.insert("horizontalAlignment", horizontalAlignment);
        o.insert("verticalAlignment", verticalAlignment);
        o.insert("fontWeightOverrideEnabled", fontWeightOverrideEnabled); o.insert("rawFontWeight", rawFontWeight);
        o.insert("textColorOverrideEnabled", textColorOverrideEnabled); o.insert("rawTextColor", rawTextColor.name(QColor::HexArgb));
        o.insert("outlineWidthOverrideEnabled", outlineWidthOverrideEnabled); o.insert("rawOutlineWidthPercent", rawOutlineWidthPercent);
        o.insert("outlineColorOverrideEnabled", outlineColorOverrideEnabled); o.insert("rawOutlineColor", rawOutlineColor.name(QColor::HexArgb));
    }
    return o;
}
bool ElementState::fromJson(const QJsonObject& o, ElementState* output, QString* error) {
    if (!output) return fail(error, "Missing element output");
    ElementState s;
    s.type = o.value("type").toString();
    if (s.type != "text" && s.type != "image" && s.type != "video") return fail(error, "Invalid element type");
    if (!onlyKeys(o,s.toJson().keys())) return fail(error,"Unexpected or missing element properties");
    double x,y,w,h,bw,bh;
    if (!number(o,"x",-1e9,1e9,&x) || !number(o,"y",-1e9,1e9,&y)
        || !number(o,"width",0.0001,1e9,&w) || !number(o,"height",0.0001,1e9,&h)
        || !number(o,"baseWidth",0.0001,1e9,&bw) || !number(o,"baseHeight",0.0001,1e9,&bh)
        || !number(o,"scale",0.0001,1e9,&s.scale)
        || !boolean(o,"visible",&s.visible) || !number(o,"contentOpacity",0,1,&s.opacity)
        || !boolean(o,"opacityOverrideEnabled",&s.opacityOverrideEnabled) || !number(o,"rawOpacity",0,1,&s.rawOpacity))
        return fail(error,"Invalid element geometry or opacity");
    s.position = {x,y}; s.size = {w,h}; s.baseSize = {bw,bh};
    if (s.type == "video" && (!boolean(o,"muted",&s.muted) || !number(o,"volume",0,1,&s.volume)))
        return fail(error,"Invalid video audio state");
    if (s.type == "text") {
        double outlinePixels;
        if (!number(o,"textOutlineWidthPx",0,2048,&outlinePixels)
            || !o.value("text").isString() || !o.value("fontFamily").isString()
            || !number(o,"fontPixelSize",1,2048,&s.fontPixelSize) || !number(o,"fontWeight",1,1000,&s.fontWeight)
            || !boolean(o,"fontItalic",&s.italic) || !boolean(o,"fontUnderline",&s.underline)
            || !boolean(o,"fontUppercase",&s.uppercase) || !color(o,"textColor",&s.textColor)
            || !boolean(o,"textHighlightEnabled",&s.highlightEnabled) || !color(o,"textHighlightColor",&s.highlightColor)
            || !number(o,"textOutlineWidthPercent",0,100,&s.outlineWidthPercent) || !color(o,"textBorderColor",&s.outlineColor)
            || !boolean(o,"textFitToTextEnabled",&s.fitToText)
            || !boolean(o,"fontWeightOverrideEnabled",&s.fontWeightOverrideEnabled) || !number(o,"rawFontWeight",1,1000,&s.rawFontWeight)
            || !boolean(o,"textColorOverrideEnabled",&s.textColorOverrideEnabled) || !color(o,"rawTextColor",&s.rawTextColor)
            || !boolean(o,"outlineWidthOverrideEnabled",&s.outlineWidthOverrideEnabled) || !number(o,"rawOutlineWidthPercent",0,100,&s.rawOutlineWidthPercent)
            || !boolean(o,"outlineColorOverrideEnabled",&s.outlineColorOverrideEnabled) || !color(o,"rawOutlineColor",&s.rawOutlineColor))
            return fail(error,"Invalid text state");
        s.text = o.value("text").toString(); s.fontFamily = o.value("fontFamily").toString();
        if (s.text.size() > 1000000 || s.fontFamily.size() > 1024) return fail(error,"Text state exceeds limits");
        s.horizontalAlignment = o.value("horizontalAlignment").toString();
        s.verticalAlignment = o.value("verticalAlignment").toString();
        if ((s.horizontalAlignment != "left" && s.horizontalAlignment != "center" && s.horizontalAlignment != "right")
            || (s.verticalAlignment != "top" && s.verticalAlignment != "center" && s.verticalAlignment != "bottom"))
            return fail(error,"Invalid text alignment");
    }
    *output = s; return true;
}
bool ElementState::fromMediaJson(const QJsonObject& media,ElementState* output,QString* error) {
    QJsonObject intrinsic=media;
    for (const char* key : {"mediaId","fileId","fileName","spans","timeline"})
        intrinsic.remove(QLatin1String(key));
    if (media.value("type").toString() != QLatin1String("text")) intrinsic.remove("assetId");
    if (media.value("type").toString() == QLatin1String("video")) intrinsic.remove("durationMs");
    return fromJson(intrinsic,output,error);
}
QJsonObject MediaTrack::toJson() const {
    QJsonArray keys;
    for (const auto& k : keyframes) keys.append(QJsonObject{{"id",k.id},{"slot",double(k.slot)},{"state",k.state.toJson()}});
    return {{"keyframes",keys},{"trackIndex",trackIndex},{"clip",QJsonObject{
        {"id",clip.id},{"startSlot",double(clip.startSlot)},
        {"sourceStartSlot",clip.sourceStartSlot ? QJsonValue(double(*clip.sourceStartSlot)) : QJsonValue(QJsonValue::Null)},
        {"durationSlots",double(clip.durationSlots)}}}};
}
bool MediaTrack::fromJson(const QJsonObject& o, MediaTrack* out, qint64 maximum, QString* error) {
    qint64 index = 0;
    if (!out || !onlyKeys(o,{"keyframes","clip","trackIndex"}) || maximum < 1
        || maximum > MaximumSupportedDurationMs * 240 / 1000 || !o.value("keyframes").isArray()
        || !o.value("clip").isObject() || !integer(o,"trackIndex",MinimumTrackIndex,MaximumTrackIndex,&index))
        return fail(error,"Invalid media timeline");
    MediaTrack t; t.trackIndex = int(index); QSet<QString> ids; QSet<qint64> times;
    const auto keys = o.value("keyframes").toArray();
    if (keys.size() > 10000) return fail(error,"Timeline exceeds item limit");
    for (auto v : keys) {
        const auto k = v.toObject(); Keyframe key; key.id = k.value("id").toString();
        if (!onlyKeys(k,{"id","slot","state"}) || key.id.isEmpty() || key.id.size() > 128 || ids.contains(key.id)
            || !integer(k,"slot",0,maximum,&key.slot) || times.contains(key.slot)
            || !k.value("state").isObject() || !ElementState::fromJson(k.value("state").toObject(),&key.state,error))
            return fail(error,"Invalid or duplicate keyframe");
        ids.insert(key.id); times.insert(key.slot); t.keyframes.append(key);
    }
    const auto c = o.value("clip").toObject();
    auto& clip = t.clip; clip.id = c.value("id").toString();
    qint64 source = 0;
    if (!c.value("sourceStartSlot").isNull()) {
        if (!integer(c,"sourceStartSlot",-MaximumSourceOffsetSlots,MaximumSourceOffsetSlots,&source))
            return fail(error,"Invalid clip source offset");
        clip.sourceStartSlot = source;
    }
    if (!onlyKeys(c,{"id","startSlot","sourceStartSlot","durationSlots"}) || clip.id.isEmpty()
        || clip.id.size() > 128 || ids.contains(clip.id)
        || !integer(c,"startSlot",0,maximum,&clip.startSlot)
        || !integer(c,"durationSlots",1,MaximumSupportedDurationMs * 240 / 1000,&clip.durationSlots)
        || !validClip(clip,maximum)) return fail(error,"Invalid clip");
    sortTrack(t); *out=t; return true;
}
qint64 SceneSettings::slotAt(qreal ms) const {
    if (!std::isfinite(ms)) return 0;
    const qreal bounded = qBound(0.0, ms, timeMs(maxSlot()));
    qint64 slot = qBound<qint64>(0, qint64(std::floor(bounded * slotsPerSecond / 1000.0)), maxSlot());
    // Correct binary roundoff against the canonical boundary itself, without
    // moving genuinely earlier timestamps into the following slot.
    if (slot > 0 && bounded < timeMs(slot)) --slot;
    else if (slot < maxSlot() && bounded >= timeMs(slot + 1)) ++slot;
    return slot;
}
qint64 SceneSettings::nearestSlot(qreal ms) const {
    if (!std::isfinite(ms)) return 0;
    const qint64 slot = slotAt(ms);
    if (slot == maxSlot()) return slot;
    const qreal midpoint = (qreal(slot) + 0.5) * 1000.0 / slotsPerSecond;
    // A mathematical tie may arrive one ULP below the represented midpoint.
    return std::nextafter(ms, std::numeric_limits<qreal>::infinity()) >= midpoint ? slot + 1 : slot;
}
qint64 SceneSettings::sourceSlots(qint64 duration) const {
    return duration > 0 && duration <= MaximumSupportedDurationMs
        ? (duration * slotsPerSecond + 999) / 1000 : 0;
}
QJsonObject SceneSettings::toJson() const {
    return {{"maxDurationMs", maxDurationMs}, {"stopSlot", stopSlot}, {"slotsPerSecond", slotsPerSecond}};
}
bool SceneSettings::fromJson(const QJsonObject& o, SceneSettings* out, QString* error) {
    SceneSettings s; qint64 rate;
    if (!out || !onlyKeys(o, {"maxDurationMs", "stopSlot", "slotsPerSecond"})
        || !integer(o, "maxDurationMs", 1, MaximumSupportedDurationMs, &s.maxDurationMs)
        || !integer(o, "slotsPerSecond", 1, 240, &rate)) return fail(error, "Invalid scene grid settings");
    s.slotsPerSecond = int(rate);
    if (s.maxSlot() < 1 || !integer(o, "stopSlot", -1, s.maxSlot(), &s.stopSlot))
        return fail(error, "Scene duration must contain a complete slot and Stop must be inside it");
    *out = s; return true;
}
ElementState evaluate(const ElementState& base, const MediaTrack& track, qint64 time) {
    if (track.keyframes.isEmpty()) return base;
    if (time <= track.keyframes.first().slot) return track.keyframes.first().state;
    if (time >= track.keyframes.last().slot) return track.keyframes.last().state;
    auto right = std::upper_bound(track.keyframes.cbegin(),track.keyframes.cend(),time,
        [](qint64 t,const Keyframe& k){return t<k.slot;});
    const auto& a=*(right-1); const auto& b=*right;
    if(time==a.slot) return a.state;
    qreal f=qreal(time-a.slot)/qreal(b.slot-a.slot); ElementState s=a.state;
    s.position = a.state.position+(b.state.position-a.state.position)*f;
    s.size = QSizeF(blend(a.state.size.width(),b.state.size.width(),f),blend(a.state.size.height(),b.state.size.height(),f));
    s.scale=blend(a.state.scale,b.state.scale,f); s.baseSize=s.size/s.scale;
    s.opacity=blend(a.state.opacity,b.state.opacity,f); s.volume=blend(a.state.volume,b.state.volume,f);
    s.fontPixelSize=blend(a.state.fontPixelSize,b.state.fontPixelSize,f); s.fontWeight=blend(a.state.fontWeight,b.state.fontWeight,f);
    s.outlineWidthPercent=blend(a.state.outlineWidthPercent,b.state.outlineWidthPercent,f);
    s.textColor=blendColor(a.state.textColor,b.state.textColor,f); s.outlineColor=blendColor(a.state.outlineColor,b.state.outlineColor,f);
    s.highlightColor=blendColor(a.state.highlightColor,b.state.highlightColor,f); return s;
}
ElementState materialize(const ElementState& value) {
    ElementState s=value;
    if(s.opacityOverrideEnabled || !qFuzzyCompare(1+s.opacity,2.0)) {s.opacityOverrideEnabled=true;s.rawOpacity=s.opacity;}
    if(s.fontWeightOverrideEnabled || !qFuzzyCompare(s.fontWeight,400.0)) {s.fontWeightOverrideEnabled=true;s.rawFontWeight=s.fontWeight;}
    if(s.textColorOverrideEnabled || s.textColor != QColor(Qt::white)) {s.textColorOverrideEnabled=true;s.rawTextColor=s.textColor;}
    if(s.outlineWidthOverrideEnabled || !qFuzzyIsNull(s.outlineWidthPercent)) {s.outlineWidthOverrideEnabled=true;s.rawOutlineWidthPercent=s.outlineWidthPercent;}
    if(s.outlineColorOverrideEnabled || s.outlineColor != QColor(Qt::black)) {s.outlineColorOverrideEnabled=true;s.rawOutlineColor=s.outlineColor;}
    return s;
}
const Clip* activeClip(const MediaTrack& track, qint64 slot) {
    const auto& clip = track.clip;
    return slot >= clip.startSlot && slot < clip.endSlot() ? &clip : nullptr;
}
bool validateMediaTrack(const MediaTrack& track, const QString& type, qint64 duration, QString* error) {
    if (type != "video" && type != "image" && type != "text") return fail(error,"Invalid media type");
    for (const auto& key : track.keyframes)
        if (key.state.type != type) return fail(error,"A keyframe must describe the same media type");
    if (type == "video" && (duration < 0 || duration > MaximumSupportedDurationMs))
        return fail(error,"Invalid video source duration");
    {
        const auto& clip = track.clip;
        if (clip.id.isEmpty() || clip.durationSlots <= 0) return fail(error,"An instance requires one clip");
        if (clip.sourceStartSlot.has_value() != (type == "video"))
            return fail(error,"Clip source offset does not match its media type");
        if (type == "video" && duration <= 0) return fail(error,"A clip requires a loaded video source");
    }
    return true;
}
VideoSample evaluateVideo(const MediaTrack& track, qreal time, qint64 duration, const SceneSettings& settings) {
    VideoSample result;
    if (!std::isfinite(time) || time < 0 || duration <= 0) return result;
    const auto* clip = activeClip(track, settings.slotAt(time));
    if (!clip || !clip->sourceStartSlot) return result;
    result.clipActive = true;
    result.clipId = clip->id;
    const qreal source = time + settings.timeMs(*clip->sourceStartSlot - clip->startSlot);
    result.playing = source >= 0 && source < duration;
    result.sourceTimeMs = qRound64(qBound(qreal(0), source, qreal(duration - 1)));
    return result;
}
QJsonObject evaluateMedia(const QJsonObject& media, qreal time, const SceneSettings& settings) {
    ElementState base; MediaTrack track;
    if (!ElementState::fromMediaJson(media, &base)
        || !MediaTrack::fromJson(media.value("timeline").toObject(), &track, settings.maxSlot())) return {};
    QJsonObject result = media;
    const auto evaluated = evaluate(base, track, settings.slotAt(time)).toJson();
    for (auto it = evaluated.begin(); it != evaluated.end(); ++it) result.insert(it.key(), it.value());
    result.insert("clipActive", activeClip(track, settings.slotAt(time)) != nullptr);
    return result;
}
bool upsertKeyframe(MediaTrack& track,Keyframe key,qint64 maximum) {
    if(key.slot<0 || key.slot>maximum) return false;
    ElementState checked;if(!ElementState::fromJson(key.state.toJson(),&checked)) return false;
    if(key.id.isEmpty()) key.id=newId();
    for(qsizetype i=track.keyframes.size();i-->0;) if(track.keyframes[i].slot==key.slot || track.keyframes[i].id==key.id) track.keyframes.removeAt(i);
    track.keyframes.append(key);sortTrack(track);return true;
}
bool removeKeyframe(MediaTrack& track,const QString& id) {
    for(qsizetype i=0;i<track.keyframes.size();++i) if(track.keyframes[i].id==id){track.keyframes.removeAt(i);return true;}return false;
}
bool moveKeyframe(MediaTrack& track,const QString& id,qint64 time,qint64 maximum) {
    for(const auto& k:track.keyframes) if(k.id==id){auto copy=k;copy.slot=qBound<qint64>(0,time,maximum);return upsertKeyframe(track,copy,maximum);}return false;
}
}
