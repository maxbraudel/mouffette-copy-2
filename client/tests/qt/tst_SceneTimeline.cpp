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
    void oneClipAndTrackHaveCanonicalRoundTrip() {
        MediaTrack track; track.trackIndex=4; track.clip={"clip",100,1000,1000};
        MediaTrack restored; QVERIFY(MediaTrack::fromJson(track.toJson(),&restored,5000));
        QCOMPARE(restored.toJson(),track.toJson());
        QVERIFY(!track.toJson().contains("clips")); QVERIFY(!track.toJson().contains("clipsInitialized"));
        auto invalid=track.toJson(); invalid["clips"]=QJsonArray{};
        QVERIFY(!MediaTrack::fromJson(invalid,&restored,5000));
        for (int index : {MinimumTrackIndex, -1, 0, MaximumTrackIndex}) {
            track.trackIndex = index;
            QVERIFY(MediaTrack::fromJson(track.toJson(), &restored, 5000));
            QCOMPARE(restored.toJson(), track.toJson());
            QVERIFY(std::isfinite(trackZ(index))); QVERIFY(trackZ(index) > 0);
        }
        QVERIFY(trackZ(-2) > trackZ(-1)); QVERIFY(trackZ(-1) > trackZ(0));
        invalid=track.toJson(); invalid["trackIndex"]=MinimumTrackIndex-1;
        QVERIFY(!MediaTrack::fromJson(invalid,&restored,5000));
        invalid["trackIndex"]=MaximumTrackIndex+1;
        QVERIFY(!MediaTrack::fromJson(invalid,&restored,5000));
        track.clip.durationSlots=0; QVERIFY(!MediaTrack::fromJson(track.toJson(),&restored,5000));
        QVERIFY(trackZ(0)>trackZ(1)); QVERIFY(trackZ(MaximumTrackIndex)>0);
    }

    void clipPresenceIsHalfOpenAndGapsAreSilent() {
        SceneSettings grid; MediaTrack t; t.clip={"a",12,3,6};
        for (qreal time : {0.0,399.999,600.0,700.0,1200.0}) {
            const auto sample=evaluateVideo(t,time,1500,grid);
            QVERIFY(!sample.clipActive); QVERIFY(!sample.playing); QVERIFY(sample.clipId.isEmpty());
        }
        const auto start=evaluateVideo(t,400,1500,grid);
        QCOMPARE(start.sourceTimeMs,100); QVERIFY(start.playing); QVERIFY(start.clipActive);
        QVERIFY(activeClip(t,12)); QVERIFY(activeClip(t,17)); QVERIFY(!activeClip(t,18));
    }

    void staticClipsNeverAcquireSourceOffsets() {
        MediaTrack t; ElementState state; t.clip={"clip",10,std::nullopt,70};
        QVERIFY(upsertKeyframe(t,{"key",25,state},100));
        MediaTrack restored; QVERIFY(MediaTrack::fromJson(t.toJson(),&restored,100));
        QCOMPARE(restored.toJson(),t.toJson()); QVERIFY(validateMediaTrack(t,"image",0));
        QVERIFY(!validateMediaTrack(t,"video",1000));
        t.keyframes.clear(); QVERIFY(validateMediaTrack(t,"text",0));
        auto malformed=t.toJson(); auto clip=malformed["clip"].toObject();
        clip.remove("sourceStartSlot");malformed["clip"]=clip;
        QVERIFY(!MediaTrack::fromJson(malformed,&restored,100));
    }

    void signedSourceOffsetsHoldFirstAndLastFramesSilently() {
        SceneSettings grid; MediaTrack t; t.clip={"source",0,-12,30};
        const auto leading=evaluateVideo(t,100,250,grid);
        QVERIFY(leading.clipActive); QVERIFY(!leading.playing); QCOMPARE(leading.sourceTimeMs,0);
        QVERIFY(evaluateVideo(t,400,250,grid).playing);
        QCOMPARE(evaluateVideo(t,500,250,grid).sourceTimeMs,100);
        const auto trailing=evaluateVideo(t,650,250,grid);
        QVERIFY(trailing.clipActive); QVERIFY(!trailing.playing); QCOMPARE(trailing.sourceTimeMs,249);
        QVERIFY(!evaluateVideo(t,1000,250,grid).clipActive);
        MediaTrack restored; QVERIFY(MediaTrack::fromJson(t.toJson(),&restored,60));
        QCOMPARE(restored.toJson(),t.toJson()); QVERIFY(validateMediaTrack(restored,"video",250));
        t.clip.sourceStartSlot=MaximumSourceOffsetSlots;
        QVERIFY(!MediaTrack::fromJson(t.toJson(),&restored,60));
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
    void videoCompensationIsSilentAndUsesProjectCadence() {
        SceneSettings grid;
        for (qint64 duration : {1,250,300}) {
            MediaTrack t; const qint64 occupied=grid.sourceSlots(duration);
            t.clip={"clip",0,0,occupied};
            QVERIFY(evaluateVideo(t,duration-0.1,duration,grid).playing);
            const auto after=evaluateVideo(t,duration,duration,grid);
            QVERIFY(!after.playing); QCOMPARE(after.clipActive,grid.timeMs(occupied)>duration);
            if (after.clipActive) QCOMPARE(after.sourceTimeMs,duration-1);
            if (duration==250) {
                QCOMPARE(occupied,8);
                // The source-correct right fragment has only the original tail padding.
                t.clip={"right",4,4,4};
                QVERIFY(evaluateVideo(t,249,250,grid).playing);
                QVERIFY(!evaluateVideo(t,250,250,grid).playing);
            }
            if (duration==300) QCOMPARE(occupied,9);
        }
    }

    void clipBoundsAndLegacyZAreRejected() {
        MediaTrack track; track.clip={"clip",90,std::nullopt,11}; MediaTrack parsed;
        QVERIFY(!MediaTrack::fromJson(track.toJson(),&parsed,100));
        track.clip.durationSlots=10; QVERIFY(MediaTrack::fromJson(track.toJson(),&parsed,100));
        ElementState state; auto old=state.toJson(); old["z"]=4;
        QVERIFY(!ElementState::fromJson(old,&state)); QVERIFY(!state.toJson().contains("z"));
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
        MediaTrack t;t.clip={"clip",0,std::nullopt,100};ElementState e;upsertKeyframe(t,{"a",10,e},100);
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
