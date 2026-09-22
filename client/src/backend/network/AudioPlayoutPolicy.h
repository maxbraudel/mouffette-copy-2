#pragma once
#include <QtGlobal>
#include <algorithm>
#include <cstdlib>

// A bounded jitter target, independent of the source/local clock offset. A
// persistent route change can retire the disposable pipe, but never teach an
// old TCP backlog a new capture time inside its existing epoch.
class AudioPlayoutPolicy {
public:
    static constexpr qint64 MinimumDelayUs=80000, MaximumDelayUs=150000;
    qint64 targetDelayUs() const { return target; }
    void reset() { *this=AudioPlayoutPolicy(); }
    bool observe(qint64 sourceUs,qint64 arrivalUs,qint64 mappedUs) {
        if(sourceUs<0 || arrivalUs<0 || mappedUs<0) return false;
        const qint64 lag=arrivalUs-mappedUs, margin=target-lag;
        const bool cadence=lastSource>=0 && sourceUs>lastSource && arrivalUs>lastArrival
            && arrivalUs-lastArrival<=250000
            && std::abs((sourceUs-lastSource)-(arrivalUs-lastArrival))<=60000;
        lastSource=sourceUs; lastArrival=arrivalUs;
        if(margin<20000) {
            healthySince=-1;
            if(!cadence || pressureSince<0) { pressureSince=arrivalUs; pressureSource=sourceUs; }
            if(arrivalUs-pressureSince>=200000 && sourceUs-pressureSource>=150000)
                target=std::clamp(std::max(target,lag+40000),MinimumDelayUs,MaximumDelayUs);
        } else pressureSince=-1;

        // Recover the low-latency target slowly after a genuinely quiet path.
        if(target>MinimumDelayUs && margin>60000) {
            if(healthySince<0) healthySince=arrivalUs;
            if(arrivalUs-healthySince>=30000000 && arrivalUs-lastDecrease>=1000000) {
                target=std::max(MinimumDelayUs,target-1000); lastDecrease=arrivalUs;
            }
        } else healthySince=-1;

        if(lag<MaximumDelayUs) { stalledSince=-1; return false; }
        if(!cadence || stalledSince<0) { stalledSince=arrivalUs; stalledSource=sourceUs; }
        return arrivalUs-stalledSince>=1000000 && sourceUs-stalledSource>=800000;
    }
private:
    qint64 target=MinimumDelayUs,lastSource=-1,lastArrival=-1;
    qint64 pressureSince=-1,pressureSource=0,healthySince=-1,lastDecrease=0;
    qint64 stalledSince=-1,stalledSource=0;
};
