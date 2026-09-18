#include <QtTest>
#include "backend/domain/scene/SceneTimeline.h"
using namespace SceneTimeline;

class SceneTimelineTest final : public QObject {
    Q_OBJECT
private slots:
    void continuousEvaluationDoesNotQuantizeTime() {
        ElementState a,b; a.position={0,10}; a.size={100,200}; a.baseSize=a.size;
        b=a;b.position={100,110};b.size={300,400};b.scale=2;b.baseSize=b.size/b.scale;
        MediaTrack t; QVERIFY(upsertKeyframe(t,{"a",100,a},180000));QVERIFY(upsertKeyframe(t,{"b",1100,b},180000));
        const auto middle=evaluate(a,t,433);
        QCOMPARE(middle.position,QPointF(33.3,43.3));
        QCOMPARE(middle.size,QSizeF(166.6,266.6));
        QCOMPARE(middle.baseSize*middle.scale,middle.size);
        QCOMPARE(evaluate(a,t,0).toJson(),a.toJson());
        QCOMPARE(evaluate(a,t,180000).toJson(),b.toJson());
    }
    void discreteChangesAndOverridesHaveIndependentSemantics() {
        ElementState a,b;a.type="text";b=a;
        a.opacity=1;a.opacityOverrideEnabled=false;a.rawOpacity=.07;
        b.opacity=.2;b.rawOpacity=.2;b.opacityOverrideEnabled=true;b.uppercase=true;
        b.textColor=Qt::red;b.rawTextColor=Qt::red;b.textColorOverrideEnabled=true;
        MediaTrack t;upsertKeyframe(t,{"a",0,a},1000);upsertKeyframe(t,{"b",1000,b},1000);
        const auto middle=evaluate(a,t,500);
        QVERIFY(qAbs(middle.opacity-.6)<1e-12);QVERIFY(!middle.uppercase);QVERIFY(!middle.opacityOverrideEnabled);
        QVERIFY(evaluate(a,t,1000).uppercase);
        const auto captured=materialize(middle);
        QVERIFY(captured.opacityOverrideEnabled);QCOMPARE(captured.rawOpacity,captured.opacity);
        QVERIFY(captured.textColorOverrideEnabled);QCOMPARE(captured.rawTextColor,captured.textColor);
        ElementState restored;QVERIFY(ElementState::fromJson(captured.toJson(),&restored));
        QCOMPARE(restored.toJson(),captured.toJson());
        QCOMPARE(materialize(a).rawOpacity,a.rawOpacity);
    }
    void keyframeOverwriteAndMoveKeepUniqueTimestamps() {
        ElementState s;MediaTrack t;
        QVERIFY(upsertKeyframe(t,{"a",10,s},100));QVERIFY(upsertKeyframe(t,{"b",20,s},100));
        QVERIFY(moveKeyframe(t,"a",20,100));QCOMPARE(t.keyframes.size(),1);QCOMPARE(t.keyframes[0].id,QString("a"));
        QVERIFY(!upsertKeyframe(t,{"x",101,s},100));QVERIFY(removeKeyframe(t,"a"));QVERIFY(t.keyframes.isEmpty());
    }
    void derivedTextOutlineMatchesLocalRenderer() {
        ElementState state;state.type="text";state.fontPixelSize=48;
        state.outlineWidthPercent=0;QCOMPARE(state.toJson()["textOutlineWidthPx"].toDouble(),0.0);
        state.outlineWidthPercent=1;QCOMPARE(state.toJson()["textOutlineWidthPx"].toDouble(),1.0);
        state.outlineWidthPercent=12.5;QCOMPARE(state.toJson()["textOutlineWidthPx"].toDouble(),6.0);
        state.fontPixelSize=48.6;state.outlineWidthPercent=50;
        QCOMPARE(state.toJson()["textOutlineWidthPx"].toDouble(),25.0);
    }
    void overwritePreservesSourceCorrectFragments() {
        MediaTrack t;QVERIFY(insertClip(t,{"old",100,1000,2000},5000));
        QVERIFY(insertClip(t,{"new",300,0,200},5000));
        QCOMPARE(t.clips.size(),3);
        QCOMPARE(t.clips[0].startMs,100);QCOMPARE(t.clips[0].sourceInMs,1000);QCOMPARE(t.clips[0].sourceOutMs,1200);
        QCOMPARE(t.clips[1].id,QString("new"));
        QCOMPARE(t.clips[2].startMs,500);QCOMPARE(t.clips[2].sourceInMs,1400);QCOMPARE(t.clips[2].sourceOutMs,2000);
        QVERIFY(splitClip(t,"new",400));QCOMPARE(t.clips.size(),4);
        auto restored=MediaTrack{};QVERIFY(MediaTrack::fromJson(t.toJson(),&restored,5000));QCOMPARE(restored.toJson(),t.toJson());
    }
    void clipSamplesHoldFramesAndSilenceGaps() {
        MediaTrack t;insertClip(t,{"a",400,100,300},2000);insertClip(t,{"b",900,700,1000},2000);
        auto before=evaluateVideo(t,0,1500);QCOMPARE(before.sourceTimeMs,100);QVERIFY(!before.playing);
        auto start=evaluateVideo(t,400,1500);QCOMPARE(start.sourceTimeMs,100);QVERIFY(start.playing);
        auto gap=evaluateVideo(t,700,1500);QCOMPARE(gap.sourceTimeMs,299);QVERIFY(!gap.playing);
        auto end=evaluateVideo(t,1200,1500);QCOMPARE(end.sourceTimeMs,999);QVERIFY(!end.playing);
        removeClip(t,"a");removeClip(t,"b");QVERIFY(t.clipsInitialized);QCOMPARE(evaluateVideo(t,900,1500).sourceTimeMs,0);
    }
    void moveAndTrimAreBoundedAndDoNotRipple() {
        MediaTrack t;insertClip(t,{"a",100,500,1000},2000);insertClip(t,{"b",1000,0,200},2000);
        QVERIFY(moveClip(t,"a",1900,2000));QCOMPARE(t.clips.last().startMs,1500);
        QVERIFY(trimClip(t,"a",1400,1800,2000,3000));
        QCOMPARE(t.clips.last().sourceInMs,400);QCOMPARE(t.clips.last().sourceOutMs,800);
        QCOMPARE(t.clips.first().startMs,1000);
    }
    void canonicalSchemasRejectUnknownFieldsAndNoncanonicalColors() {
        ElementState e;e.type="text";ElementState parsed;
        const auto original=e.toJson();
        for(const auto key:{"autoPlay","fadeInSeconds","playing","unknown"}) {
            auto value=original;value[key]=true;QVERIFY(!ElementState::fromJson(value,&parsed));
        }
        for(const auto color:{"red","#fff","#ffffff","#ggffffff"}) {
            auto value=original;value["textColor"]=color;QVERIFY(!ElementState::fromJson(value,&parsed));
        }
        auto invalid=original;invalid["textOutlineWidthPx"]=2049;QVERIFY(!ElementState::fromJson(invalid,&parsed));
        auto media=original;media["mediaId"]="id";media["timeline"]=MediaTrack{}.toJson();
        QVERIFY(ElementState::fromMediaJson(media,&parsed));QVERIFY(!ElementState::fromJson(media,&parsed));
        auto wrongMetadata=media;wrongMetadata["durationMs"]=1000;QVERIFY(!ElementState::fromMediaJson(wrongMetadata,&parsed));
        wrongMetadata=media;wrongMetadata["assetId"]="text-asset";QVERIFY(!ElementState::fromMediaJson(wrongMetadata,&parsed));
        media["autoDisplay"]=false;QVERIFY(!ElementState::fromMediaJson(media,&parsed));
        auto settings=SceneSettings{}.toJson();settings["gridStepMs"]=100;SceneSettings scene;
        QVERIFY(!SceneSettings::fromJson(settings,&scene));
        auto track=MediaTrack{}.toJson();track["unknown"]=0;MediaTrack checked;
        QVERIFY(!MediaTrack::fromJson(track,&checked,180000));
    }

    void validationRejectsDuplicatesOverlapAndInvalidStop() {
        SceneSettings s;auto settings=s.toJson();settings["stopTimeMs"]=0;QVERIFY(SceneSettings::fromJson(settings,&s));
        QCOMPARE(s.effectiveStopMs(),0);
        settings["stopTimeMs"]=-2;QVERIFY(!SceneSettings::fromJson(settings,&s));
        MediaTrack t;ElementState e;upsertKeyframe(t,{"a",10,e},100);
        auto json=t.toJson();auto keys=json["keyframes"].toArray();auto dup=keys[0].toObject();dup["id"]="other";keys.append(dup);json["keyframes"]=keys;
        QVERIFY(!MediaTrack::fromJson(json,&t,100));
        auto element=e.toJson();element["contentOpacity"]=2.0;QVERIFY(!ElementState::fromJson(element,&e));
    }
    void measuresRepresentativeEvaluationWorkload() {
        QList<MediaTrack> tracks;
        for(int media=0;media<100;++media) {
            MediaTrack track;
            for(int key=0;key<100;++key) {
                ElementState state;state.type="text";state.position={qreal(media+key),qreal(key)};
                state.opacity=qreal(key)/100;state.textColor=QColor::fromHsv(key*3,200,200);
                track.keyframes.append({QString::number(key),key*1800,state});
            }
            tracks.append(track);
        }
        double checksum=0;QElapsedTimer timer;timer.start();
        for(int tick=0;tick<1000;++tick) for(const auto& track:tracks)
            checksum+=evaluate(track.keyframes.first().state,track,tick*179).position.x();
        const double elapsedMs=timer.nsecsElapsed()/1000000.0;
        QVERIFY(checksum>0);
        qInfo().nospace()<<"Timeline evaluator: 100 media x 100 keys x 1000 ticks in "
                        <<elapsedMs<<" ms ("<<elapsedMs/1000.0<<" ms/tick), checksum="<<checksum;
    }
};
QTEST_APPLESS_MAIN(SceneTimelineTest)
#include "tst_SceneTimeline.moc"
