#include <QtTest>
#include "backend/domain/scene/SceneTimeline.h"
using namespace SceneTimeline;

class SceneTimelineTest final : public QObject {
    Q_OBJECT
private slots:
    void linearEvaluationUsesSlotIndices() {
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
        MediaTrack t;QVERIFY(insertClip(t,{"old",100,1000,1000},5000));
        QVERIFY(insertClip(t,{"new",300,0,200},5000));
        QCOMPARE(t.clips.size(),3);
        QCOMPARE(t.clips[0].startSlot,100);QCOMPARE(t.clips[0].sourceStartSlot.value(),1000);QCOMPARE(t.clips[0].sourceEndSlot(),1200);
        QCOMPARE(t.clips[1].id,QString("new"));
        QCOMPARE(t.clips[2].startSlot,500);QCOMPARE(t.clips[2].sourceStartSlot.value(),1400);QCOMPARE(t.clips[2].sourceEndSlot(),2000);
        QVERIFY(splitClip(t,"new",400));QCOMPARE(t.clips.size(),4);
        auto restored=MediaTrack{};QVERIFY(MediaTrack::fromJson(t.toJson(),&restored,5000));QCOMPARE(restored.toJson(),t.toJson());
    }
    void clipPresenceIsHalfOpenAndGapsAreSilent() {
        SceneSettings grid;
        MediaTrack t;insertClip(t,{"a",12,3,6},60);insertClip(t,{"b",27,21,9},60);
        for (qreal time : {0.0, 399.999, 600.0, 700.0, 1200.0}) {
            const auto sample=evaluateVideo(t,time,1500,grid);
            QVERIFY(!sample.clipActive); QVERIFY(!sample.playing); QVERIFY(sample.clipId.isEmpty());
        }
        auto start=evaluateVideo(t,400,1500,grid);
        QCOMPARE(start.sourceTimeMs,100); QVERIFY(start.playing); QVERIFY(start.clipActive);
        QVERIFY(activeClip(t,12)); QVERIFY(activeClip(t,17)); QVERIFY(!activeClip(t,18));
        removeClip(t,"a");removeClip(t,"b");QVERIFY(t.clipsInitialized);
        QVERIFY(!evaluateVideo(t,900,1500,grid).clipActive);
    }
    void staticClipsShareEditingAndNeverAcquireSourceOffsets() {
        MediaTrack t; ElementState state;
        QVERIFY(upsertKeyframe(t,{"key",25,state},100));
        QVERIFY(insertClip(t,{"full",0,std::nullopt,100},100));
        QVERIFY(trimClip(t,"full",10,80,100));
        QVERIFY(splitClip(t,"full",30));
        QVERIFY(moveClip(t,"full",50,100));
        QVERIFY(insertClip(t,{"overwrite",55,std::nullopt,10},100));
        for (const auto& clip : t.clips) QVERIFY(!clip.sourceStartSlot);
        QCOMPARE(t.keyframes.first().slot,25);
        MediaTrack restored; QVERIFY(MediaTrack::fromJson(t.toJson(),&restored,100));
        QCOMPARE(restored.toJson(),t.toJson());
        QVERIFY(validateMediaTrack(t,"image",0));
        QVERIFY(!validateMediaTrack(t,"video",1000));
        t.keyframes.clear(); QVERIFY(validateMediaTrack(t,"text",0));
        auto malformed=t.toJson(); auto clips=malformed["clips"].toArray();
        auto clip=clips.first().toObject();clip.remove("sourceStartSlot");clips[0]=clip;malformed["clips"]=clips;
        QVERIFY(!MediaTrack::fromJson(malformed,&restored,100));
    }
    void extensionsRecoverSourceAndCutsPreserveEntirelyHeldFragments() {
        SceneSettings grid; MediaTrack t;
        QVERIFY(insertClip(t,{"source",12,0,8},60));
        QVERIFY(trimClip(t,"source",0,30,60));
        QCOMPARE(t.clips.first().sourceStartSlot.value(),-12);
        const auto leading=evaluateVideo(t,100,250,grid);
        QVERIFY(leading.clipActive); QVERIFY(!leading.playing); QCOMPARE(leading.sourceTimeMs,0);
        QVERIFY(evaluateVideo(t,400,250,grid).playing);
        QCOMPARE(evaluateVideo(t,500,250,grid).sourceTimeMs,100);
        const auto trailing=evaluateVideo(t,650,250,grid);
        QVERIFY(trailing.clipActive); QVERIFY(!trailing.playing); QCOMPARE(trailing.sourceTimeMs,249);
        QVERIFY(!evaluateVideo(t,1000,250,grid).clipActive);
        QVERIFY(splitClip(t,"source",6)); // An entirely frozen leading fragment.
        QVERIFY(!evaluateVideo(t,199,250,grid).playing);
        QVERIFY(splitClip(t,t.clips.last().id,24)); // An entirely frozen trailing fragment.
        const auto tail=t.clips.last(); QVERIFY(*tail.sourceStartSlot > grid.sourceSlots(250));
        QVERIFY(moveClip(t,tail.id,40,60));
        QCOMPARE(evaluateVideo(t,grid.timeMs(40),250,grid).sourceTimeMs,249);
        QVERIFY(trimClip(t,tail.id,24,46,60)); // Recover earlier source frames.
        QVERIFY(evaluateVideo(t,grid.timeMs(28),250,grid).playing);
        MediaTrack restored; QVERIFY(MediaTrack::fromJson(t.toJson(),&restored,60));
        QCOMPARE(restored.toJson(),t.toJson()); QVERIFY(validateMediaTrack(restored,"video",250));
    }
    void steppedInterpolationMatchesBothExamplesAndReversePlayback() {
        SceneSettings grid; ElementState a,b; a.opacity=0; b.opacity=1; b.uppercase=true;
        for (qint64 distance : {1,2}) {
            MediaTrack t; upsertKeyframe(t,{"a",10,a},100); upsertKeyframe(t,{"b",10+distance,b},100);
            for (int direction : {1,-1}) for(int n=0;n<=distance;++n) {
                const auto slot = 10 + (direction==1 ? n : distance-n);
                const auto at = evaluate(a,t,grid.slotAt(grid.timeMs(slot)));
                QCOMPARE(at.opacity,qreal(slot-10)/distance);
                QCOMPARE(evaluate(a,t,grid.slotAt(grid.timeMs(slot)+1)).toJson(),at.toJson());
                QCOMPARE(at.uppercase,slot==10+distance);
            }
        }
    }
    void gridConversionsAreExactAndBounded() {
        SceneSettings grid;
        for (int rate : {1, 24, 30, 60, 144, 240}) {
            grid.slotsPerSecond=rate;
            for(qint64 n=0;n<=grid.maxSlot();++n) {
                QCOMPARE(grid.slotAt(grid.timeMs(n)),n);
                QCOMPARE(grid.nearestSlot(grid.timeMs(n)),n);
                if (n<grid.maxSlot()) {
                    QCOMPARE(grid.nearestSlot(grid.timeMs(n)+grid.timeMs(1)/2),n+1);
                    QCOMPARE(grid.slotAt(grid.timeMs(n+1)-0.000001),n);
                }
            }
        }
        grid.slotsPerSecond=30; grid.maxDurationMs=999;
        QCOMPARE(grid.maxSlot(),29); QCOMPARE(grid.nearestSlot(999),29);
        QCOMPARE(grid.slotAt(-50),0); QCOMPARE(grid.nearestSlot(1e100),29);
        SceneSettings parsed;
        grid.maxDurationMs=33; QVERIFY(!SceneSettings::fromJson(grid.toJson(),&parsed));
        grid.maxDurationMs=34; QVERIFY(SceneSettings::fromJson(grid.toJson(),&parsed));
        grid.slotsPerSecond=241; QVERIFY(!SceneSettings::fromJson(grid.toJson(),&parsed));
    }
    void videoCompensationIsSilentAndNeverMultipliedByCuts() {
        SceneSettings grid;
        for (qint64 duration : {1, 250, 300}) {
            MediaTrack t;
            const qint64 occupied=grid.sourceSlots(duration);
            QVERIFY(insertClip(t,{"clip",0,0,occupied},grid.maxSlot()));
            const auto before=evaluateVideo(t,duration-0.1,duration,grid);
            QVERIFY(before.playing);
            const auto after=evaluateVideo(t,duration,duration,grid);
            QVERIFY(!after.playing);
            QCOMPARE(after.clipActive, grid.timeMs(occupied) > duration);
            if (after.clipActive) QCOMPARE(after.sourceTimeMs,duration-1);
            if (duration==250) {
                QCOMPARE(occupied,8); QVERIFY(qAbs(grid.timeMs(occupied)-266.6666666667)<1e-7);
                QVERIFY(splitClip(t,"clip",4));
                QCOMPARE(t.clips[0].durationSlots,4); QCOMPARE(t.clips[1].durationSlots,4);
                QCOMPARE(t.clips[1].sourceStartSlot.value(),4);
                QVERIFY(evaluateVideo(t,249,250,grid).playing);
                QVERIFY(!evaluateVideo(t,250,250,grid).playing);
                const QString tail=t.clips[1].id;
                QVERIFY(trimClip(t,tail,4,100,100));
                QCOMPARE(t.clips[1].endSlot(),100);
                QVERIFY(evaluateVideo(t,3000,250,grid).clipActive);
                QVERIFY(!evaluateVideo(t,3000,250,grid).playing);
                QVERIFY(trimClip(t,tail,4,8,100));
                QVERIFY(moveClip(t,tail,10,100)); QCOMPARE(t.clips[1].durationSlots,4);
                auto copy=t.clips[1]; copy.id="copy"; copy.startSlot=20;
                QVERIFY(insertClip(t,copy,100)); QCOMPARE(t.clips.last().sourceEndSlot(),8);
                QVERIFY(insertClip(t,{"overwrite",22,0,1},100));
                QCOMPARE(t.clips.last().startSlot,23); QCOMPARE(t.clips.last().sourceStartSlot.value(),7);
                QCOMPARE(t.clips.last().durationSlots,1); // Only this source tail pads.
                QVERIFY(!evaluateVideo(t,grid.timeMs(23)+20,250,grid).playing);
            }
            if(duration==300) QCOMPARE(occupied,9);
        }
    }
    void moveAndTrimAreBoundedAndDoNotRipple() {
        MediaTrack t;insertClip(t,{"a",100,500,500},2000);insertClip(t,{"b",1000,0,200},2000);
        QVERIFY(moveClip(t,"a",1900,2000));QCOMPARE(t.clips.last().startSlot,1500);
        QVERIFY(trimClip(t,"a",1400,1800,2000));
        QCOMPARE(t.clips.last().sourceStartSlot.value(),400);QCOMPARE(t.clips.last().sourceEndSlot(),800);
        QCOMPARE(t.clips.first().startSlot,1000);
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
        SceneSettings s;auto settings=s.toJson();settings["stopSlot"]=0;QVERIFY(SceneSettings::fromJson(settings,&s));
        QCOMPARE(s.effectiveStopMs(),0);
        settings["stopSlot"]=-2;QVERIFY(!SceneSettings::fromJson(settings,&s));
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
