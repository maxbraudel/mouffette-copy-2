#include "SceneTimeline.h"
#include "backend/domain/media/TextRenderState.h"

#include <QJsonArray>
#include <QSet>
#include <QUuid>
#include <algorithm>
#include <cmath>

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
    std::sort(t.keyframes.begin(), t.keyframes.end(), [](const Keyframe& a, const Keyframe& b) { return a.timeMs < b.timeMs; });
    std::sort(t.clips.begin(), t.clips.end(), [](const VideoClip& a, const VideoClip& b) { return a.startMs < b.startMs; });
}
bool validClip(const VideoClip& c, qint64 maximum) {
    return !c.id.isEmpty() && c.startMs >= 0 && c.sourceInMs >= 0 && c.sourceOutMs > c.sourceInMs
        && c.sourceOutMs <= MaximumSupportedDurationMs && c.endMs() <= maximum;
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
    for (const auto& k : keyframes) keys.append(QJsonObject{{"id",k.id},{"timeMs",double(k.timeMs)},{"state",k.state.toJson()}});
    for (const auto& c : clips) segments.append(QJsonObject{{"id",c.id},{"startMs",double(c.startMs)},
        {"sourceInMs",double(c.sourceInMs)},{"sourceOutMs",double(c.sourceOutMs)}});
    return {{"keyframes",keys},{"clips",segments},{"clipsInitialized",clipsInitialized}};
}
bool MediaTrack::fromJson(const QJsonObject& o, MediaTrack* out, qint64 maximum, QString* error) {
    if (!out || !onlyKeys(o,{"keyframes","clips","clipsInitialized"}) || maximum < 1 || maximum > MaximumSupportedDurationMs || !o.value("keyframes").isArray()
        || !o.value("clips").isArray() || !o.value("clipsInitialized").isBool()) return fail(error,"Invalid media timeline");
    MediaTrack t; t.clipsInitialized = o.value("clipsInitialized").toBool(); QSet<QString> ids; QSet<qint64> times;
    auto keys = o.value("keyframes").toArray(), clips = o.value("clips").toArray();
    if (keys.size() > 10000 || clips.size() > 10000) return fail(error,"Timeline exceeds item limit");
    for (auto v : keys) {
        auto k = v.toObject(); Keyframe key; key.id = k.value("id").toString();
        if (!onlyKeys(k,{"id","timeMs","state"}) || key.id.isEmpty() || key.id.size() > 128 || ids.contains(key.id)
            || !integer(k,"timeMs",0,maximum,&key.timeMs) || times.contains(key.timeMs)
            || !k.value("state").isObject() || !ElementState::fromJson(k.value("state").toObject(),&key.state,error))
            return fail(error,"Invalid or duplicate keyframe");
        ids.insert(key.id); times.insert(key.timeMs); t.keyframes.append(key);
    }
    for (auto v : clips) {
        auto c = v.toObject(); VideoClip clip; clip.id = c.value("id").toString();
        if (!onlyKeys(c,{"id","startMs","sourceInMs","sourceOutMs"}) || clip.id.isEmpty() || clip.id.size() > 128 || ids.contains(clip.id)
            || !integer(c,"startMs",0,maximum,&clip.startMs)
            || !integer(c,"sourceInMs",0,MaximumSupportedDurationMs,&clip.sourceInMs)
            || !integer(c,"sourceOutMs",1,MaximumSupportedDurationMs,&clip.sourceOutMs) || !validClip(clip,maximum))
            return fail(error,"Invalid video clip");
        ids.insert(clip.id); t.clips.append(clip);
    }
    sortTrack(t);
    for (qsizetype i=1;i<t.clips.size();++i) if(t.clips[i-1].endMs()>t.clips[i].startMs) return fail(error,"Overlapping clips");
    if (!t.clipsInitialized && !t.clips.isEmpty()) return fail(error,"Uninitialized clip list is not empty");
    *out=t; return true;
}
QJsonObject SceneSettings::toJson() const { return {{"maxDurationMs",double(maxDurationMs)},{"stopTimeMs",double(stopTimeMs)}}; }
bool SceneSettings::fromJson(const QJsonObject& o, SceneSettings* out, QString* error) {
    SceneSettings s;
    if(!out || !onlyKeys(o,{"maxDurationMs","stopTimeMs"}) || !integer(o,"maxDurationMs",1,MaximumSupportedDurationMs,&s.maxDurationMs)
        || !integer(o,"stopTimeMs",-1,s.maxDurationMs,&s.stopTimeMs)) return fail(error,"Invalid scene timeline settings");
    *out=s; return true;
}
ElementState evaluate(const ElementState& base, const MediaTrack& track, qint64 time) {
    if (track.keyframes.isEmpty()) return base;
    if (time <= track.keyframes.first().timeMs) return track.keyframes.first().state;
    if (time >= track.keyframes.last().timeMs) return track.keyframes.last().state;
    auto right = std::upper_bound(track.keyframes.cbegin(),track.keyframes.cend(),time,
        [](qint64 t,const Keyframe& k){return t<k.timeMs;});
    const auto& a=*(right-1); const auto& b=*right;
    if(time==a.timeMs) return a.state;
    qreal f=qreal(time-a.timeMs)/qreal(b.timeMs-a.timeMs); ElementState s=a.state;
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
VideoSample evaluateVideo(const MediaTrack& track,qint64 time,qint64 duration) {
    VideoSample result;
    if(track.clips.isEmpty()) return result;
    const VideoClip* previous=nullptr;
    for(const auto& c:track.clips) {
        if(time<c.startMs) {result.sourceTimeMs=previous?previous->sourceOutMs-1:c.sourceInMs;break;}
        if(time<c.endMs()) {result.sourceTimeMs=c.sourceInMs+time-c.startMs;result.playing=true;result.clipId=c.id;break;}
        previous=&c; result.sourceTimeMs=c.sourceOutMs-1;
    }
    if(duration>0) result.sourceTimeMs=qBound<qint64>(0,result.sourceTimeMs,duration-1);
    return result;
}
QJsonObject evaluateMedia(const QJsonObject& media,qint64 time) {
    ElementState base; MediaTrack track;
    if(!ElementState::fromMediaJson(media,&base) || !MediaTrack::fromJson(media.value("timeline").toObject(),&track,MaximumSupportedDurationMs)) return {};
    QJsonObject result=media;const auto evaluated=evaluate(base,track,time).toJson();
    for(auto it=evaluated.begin();it!=evaluated.end();++it) result.insert(it.key(),it.value());
    return result;
}
bool upsertKeyframe(MediaTrack& track,Keyframe key,qint64 maximum) {
    if(key.timeMs<0 || key.timeMs>maximum) return false;
    ElementState checked;if(!ElementState::fromJson(key.state.toJson(),&checked)) return false;
    if(key.id.isEmpty()) key.id=newId();
    for(qsizetype i=track.keyframes.size();i-->0;) if(track.keyframes[i].timeMs==key.timeMs || track.keyframes[i].id==key.id) track.keyframes.removeAt(i);
    track.keyframes.append(key);sortTrack(track);return true;
}
bool removeKeyframe(MediaTrack& track,const QString& id) {
    for(qsizetype i=0;i<track.keyframes.size();++i) if(track.keyframes[i].id==id){track.keyframes.removeAt(i);return true;}return false;
}
bool moveKeyframe(MediaTrack& track,const QString& id,qint64 time,qint64 maximum) {
    for(const auto& k:track.keyframes) if(k.id==id){auto copy=k;copy.timeMs=qBound<qint64>(0,time,maximum);return upsertKeyframe(track,copy,maximum);}return false;
}
bool insertClip(MediaTrack& track,VideoClip clip,qint64 maximum) {
    if(clip.id.isEmpty()) clip.id=newId();
    if(!validClip(clip,maximum)) return false;
    QList<VideoClip> result;
    for(const auto& old:track.clips) {
        if(old.id==clip.id) continue;
        if(old.endMs()<=clip.startMs || old.startMs>=clip.endMs()){result.append(old);continue;}
        if(old.startMs<clip.startMs) {auto left=old;left.sourceOutMs=left.sourceInMs+clip.startMs-left.startMs;result.append(left);}
        if(old.endMs()>clip.endMs()) {auto right=old;right.id=newId();right.sourceInMs+=clip.endMs()-right.startMs;right.startMs=clip.endMs();result.append(right);}
    }
    result.append(clip);track.clips=result;track.clipsInitialized=true;sortTrack(track);return true;
}
bool removeClip(MediaTrack& track,const QString& id) {
    for(qsizetype i=0;i<track.clips.size();++i) if(track.clips[i].id==id){track.clips.removeAt(i);track.clipsInitialized=true;return true;}return false;
}
bool moveClip(MediaTrack& track,const QString& id,qint64 start,qint64 maximum) {
    for(const auto& c:track.clips) if(c.id==id){if(maximum<c.durationMs())return false;auto moved=c;moved.startMs=qBound<qint64>(0,start,maximum-c.durationMs());return insertClip(track,moved,maximum);}return false;
}
bool splitClip(MediaTrack& track,const QString& id,qint64 time) {
    for(qsizetype i=0;i<track.clips.size();++i) if(track.clips[i].id==id) {
        const auto old=track.clips[i];if(time<=old.startMs || time>=old.endMs()) return false;
        auto left=old,right=old;left.sourceOutMs=old.sourceInMs+time-old.startMs;
        right.id=newId();right.startMs=time;right.sourceInMs=left.sourceOutMs;
        track.clips[i]=left;track.clips.insert(i+1,right);return true;
    }return false;
}
bool trimClip(MediaTrack& track,const QString& id,qint64 start,qint64 end,qint64 maximum,qint64 duration) {
    if (duration <= 0 || maximum <= 0) return false;
    for(const auto& c:track.clips) if(c.id==id) {
        auto trimmed=c;const qint64 minimum=qMax<qint64>(0,c.startMs-c.sourceInMs);
        const qint64 maximumEnd=qMin(maximum,c.startMs+duration-c.sourceInMs);
        if (maximumEnd <= minimum) return false;
        trimmed.startMs=qBound(minimum,start,qMax(minimum,maximumEnd-1));
        end=qBound(trimmed.startMs+1,end,maximumEnd);
        trimmed.sourceInMs=c.sourceInMs+trimmed.startMs-c.startMs;
        trimmed.sourceOutMs=trimmed.sourceInMs+end-trimmed.startMs;
        return insertClip(track,trimmed,maximum);
    }return false;
}
}
