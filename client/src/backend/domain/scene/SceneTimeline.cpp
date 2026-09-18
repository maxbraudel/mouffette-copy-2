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
    std::sort(t.clips.begin(), t.clips.end(), [](const VideoClip& a, const VideoClip& b) { return a.startSlot < b.startSlot; });
}
bool validClip(const VideoClip& c, qint64 maximum) {
    return !c.id.isEmpty() && c.startSlot >= 0 && c.sourceStartSlot >= 0 && c.durationSlots > 0
        && c.sourceEndSlot() <= MaximumSupportedDurationMs * 240 / 1000 && c.endSlot() <= maximum;
}
}
QString newId() { return QUuid::createUuid().toString(QUuid::WithoutBraces); }

QJsonObject ElementState::toJson() const {
    QJsonObject o{
        {"type", type}, {"x", position.x()}, {"y", position.y()},
        {"width", size.width()}, {"height", size.height()},
        {"baseWidth", baseSize.width()}, {"baseHeight", baseSize.height()}, {"scale", scale},
        {"visible", visible}, {"z", z}, {"contentOpacity", opacity},
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
        || !number(o,"scale",0.0001,1e9,&s.scale) || !number(o,"z",-1e9,1e9,&s.z)
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
    QJsonArray keys, segments;
    for (const auto& k : keyframes) keys.append(QJsonObject{{"id",k.id},{"slot",double(k.slot)},{"state",k.state.toJson()}});
    for (const auto& c : clips) segments.append(QJsonObject{{"id",c.id},{"startSlot",double(c.startSlot)},
        {"sourceStartSlot",double(c.sourceStartSlot)},{"durationSlots",double(c.durationSlots)}});
    return {{"keyframes",keys},{"clips",segments},{"clipsInitialized",clipsInitialized}};
}
bool MediaTrack::fromJson(const QJsonObject& o, MediaTrack* out, qint64 maximum, QString* error) {
    if (!out || !onlyKeys(o,{"keyframes","clips","clipsInitialized"}) || maximum < 1 || maximum > MaximumSupportedDurationMs * 240 / 1000 || !o.value("keyframes").isArray()
        || !o.value("clips").isArray() || !o.value("clipsInitialized").isBool()) return fail(error,"Invalid media timeline");
    MediaTrack t; t.clipsInitialized = o.value("clipsInitialized").toBool(); QSet<QString> ids; QSet<qint64> times;
    auto keys = o.value("keyframes").toArray(), clips = o.value("clips").toArray();
    if (keys.size() > 10000 || clips.size() > 10000) return fail(error,"Timeline exceeds item limit");
    for (auto v : keys) {
        auto k = v.toObject(); Keyframe key; key.id = k.value("id").toString();
        if (!onlyKeys(k,{"id","slot","state"}) || key.id.isEmpty() || key.id.size() > 128 || ids.contains(key.id)
            || !integer(k,"slot",0,maximum,&key.slot) || times.contains(key.slot)
            || !k.value("state").isObject() || !ElementState::fromJson(k.value("state").toObject(),&key.state,error))
            return fail(error,"Invalid or duplicate keyframe");
        ids.insert(key.id); times.insert(key.slot); t.keyframes.append(key);
    }
    for (auto v : clips) {
        auto c = v.toObject(); VideoClip clip; clip.id = c.value("id").toString();
        if (!onlyKeys(c,{"id","startSlot","sourceStartSlot","durationSlots"}) || clip.id.isEmpty() || clip.id.size() > 128 || ids.contains(clip.id)
            || !integer(c,"startSlot",0,maximum,&clip.startSlot)
            || !integer(c,"sourceStartSlot",0,MaximumSupportedDurationMs * 240 / 1000,&clip.sourceStartSlot)
            || !integer(c,"durationSlots",1,MaximumSupportedDurationMs * 240 / 1000,&clip.durationSlots) || !validClip(clip,maximum))
            return fail(error,"Invalid video clip");
        ids.insert(clip.id); t.clips.append(clip);
    }
    sortTrack(t);
    for (qsizetype i=1;i<t.clips.size();++i) if(t.clips[i-1].endSlot()>t.clips[i].startSlot) return fail(error,"Overlapping clips");
    if (!t.clipsInitialized && !t.clips.isEmpty()) return fail(error,"Uninitialized clip list is not empty");
    *out=t; return true;
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
VideoSample evaluateVideo(const MediaTrack& track, qreal time, qint64 duration, const SceneSettings& settings) {
    VideoSample result;
    if (track.clips.isEmpty() || duration <= 0) return result;
    const auto exitTime = [&](const VideoClip& clip) {
        return qMax<qint64>(0, qCeil(qMin(qreal(duration), settings.timeMs(clip.sourceEndSlot()))) - 1);
    };
    const VideoClip* previous = nullptr;
    for (const auto& clip : track.clips) {
        if (time < settings.timeMs(clip.startSlot)) {
            result.sourceTimeMs = previous ? exitTime(*previous) : qRound64(settings.timeMs(clip.sourceStartSlot));
            break;
        }
        if (time < settings.timeMs(clip.endSlot())) {
            const qreal source = time + settings.timeMs(clip.sourceStartSlot - clip.startSlot);
            result.playing = source < qMin(qreal(duration), settings.timeMs(clip.sourceEndSlot()));
            result.sourceTimeMs = result.playing ? qRound64(source) : exitTime(clip);
            result.clipId = clip.id;
            break;
        }
        previous = &clip;
        result.sourceTimeMs = exitTime(clip);
    }
    result.sourceTimeMs = qBound<qint64>(0, result.sourceTimeMs, duration - 1);
    return result;
}
QJsonObject evaluateMedia(const QJsonObject& media, qreal time, const SceneSettings& settings) {
    ElementState base; MediaTrack track;
    if (!ElementState::fromMediaJson(media, &base)
        || !MediaTrack::fromJson(media.value("timeline").toObject(), &track, settings.maxSlot())) return {};
    QJsonObject result = media;
    const auto evaluated = evaluate(base, track, settings.slotAt(time)).toJson();
    for (auto it = evaluated.begin(); it != evaluated.end(); ++it) result.insert(it.key(), it.value());
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
bool insertClip(MediaTrack& track,VideoClip clip,qint64 maximum) {
    if(clip.id.isEmpty()) clip.id=newId();
    if(!validClip(clip,maximum)) return false;
    QList<VideoClip> result;
    for(const auto& old:track.clips) {
        if(old.id==clip.id) continue;
        if(old.endSlot()<=clip.startSlot || old.startSlot>=clip.endSlot()){result.append(old);continue;}
        if(old.startSlot<clip.startSlot) {auto left=old;left.durationSlots=clip.startSlot-left.startSlot;result.append(left);}
        if(old.endSlot()>clip.endSlot()) {auto right=old;right.id=newId();right.durationSlots=old.endSlot()-clip.endSlot();right.sourceStartSlot+=clip.endSlot()-right.startSlot;right.startSlot=clip.endSlot();result.append(right);}
    }
    result.append(clip);track.clips=result;track.clipsInitialized=true;sortTrack(track);return true;
}
bool removeClip(MediaTrack& track,const QString& id) {
    for(qsizetype i=0;i<track.clips.size();++i) if(track.clips[i].id==id){track.clips.removeAt(i);track.clipsInitialized=true;return true;}return false;
}
bool moveClip(MediaTrack& track,const QString& id,qint64 start,qint64 maximum) {
    for(const auto& c:track.clips) if(c.id==id){if(maximum<c.durationSlots)return false;auto moved=c;moved.startSlot=qBound<qint64>(0,start,maximum-c.durationSlots);return insertClip(track,moved,maximum);}return false;
}
bool splitClip(MediaTrack& track,const QString& id,qint64 time) {
    for(qsizetype i=0;i<track.clips.size();++i) if(track.clips[i].id==id) {
        const auto old=track.clips[i];if(time<=old.startSlot || time>=old.endSlot()) return false;
        auto left=old,right=old;left.durationSlots=time-old.startSlot;
        right.id=newId();right.startSlot=time;right.sourceStartSlot=old.sourceStartSlot+left.durationSlots;right.durationSlots=old.durationSlots-left.durationSlots;
        track.clips[i]=left;track.clips.insert(i+1,right);return true;
    }return false;
}
bool trimClip(MediaTrack& track,const QString& id,qint64 start,qint64 end,qint64 maximum,qint64 duration) {
    if (duration <= 0 || maximum <= 0) return false;
    for(const auto& c:track.clips) if(c.id==id) {
        auto trimmed=c;const qint64 minimum=qMax<qint64>(0,c.startSlot-c.sourceStartSlot);
        const qint64 maximumEnd=qMin(maximum,c.startSlot+duration-c.sourceStartSlot);
        if (maximumEnd <= minimum) return false;
        trimmed.startSlot=qBound(minimum,start,qMax(minimum,maximumEnd-1));
        end=qBound(trimmed.startSlot+1,end,maximumEnd);
        trimmed.sourceStartSlot=c.sourceStartSlot+trimmed.startSlot-c.startSlot;
        trimmed.durationSlots=end-trimmed.startSlot;
        return insertClip(track,trimmed,maximum);
    }return false;
}
}
