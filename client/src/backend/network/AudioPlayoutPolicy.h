#pragma once
#include <QtGlobal>
#include "backend/audiosharing/AudioOutputTiming.h"
#include <algorithm>
#include <cstdlib>

// A bounded jitter target, independent of the source/local clock offset. A
// persistent route change can retire the disposable pipe, but never teach an
// old TCP backlog a new capture time inside its existing epoch.
class AudioPlayoutPolicy {
public:
    static constexpr qint64 MinimumDelayUs=80000, InitialDelayUs=120000, MaximumDelayUs=150000;
    static constexpr qint64 RequiredHeadroomUs=60000;
    qint64 targetDelayUs() const { return target; }
    qint64 outputQuantumUs() const { return outputQuantum; }
    void setOutputQuantumUs(qint64 quantumUs) {
        const auto quantum=AudioOutputTiming::normalizeQuantumUs(quantumUs);
        const auto change=AudioOutputTiming::extraPlayoutDelayUs(quantum)
            -AudioOutputTiming::extraPlayoutDelayUs(outputQuantum);
        outputQuantum=quantum;
        target=std::clamp(target+change,AudioOutputTiming::minimumPlayoutDelayUs(quantum),
            AudioOutputTiming::maximumPlayoutDelayUs(quantum));
        if(change) healthySince=-1;
    }
    void reset() {
        const auto quantum=outputQuantum;
        *this=AudioPlayoutPolicy();
        setOutputQuantumUs(quantum);
    }
    bool observe(qint64 sourceUs,qint64 arrivalUs,qint64 mappedUs) {
        if(sourceUs<0 || arrivalUs<0 || mappedUs<0) return false;
        const qint64 lag=arrivalUs-mappedUs, margin=target-lag;
        const bool cadence=lastSource>=0 && sourceUs>lastSource && arrivalUs>=lastArrival
            && arrivalUs-lastArrival<=250000
            && std::abs((sourceUs-lastSource)-(arrivalUs-lastArrival))<=60000;
        lastSource=sourceUs; lastArrival=arrivalUs;
        // Source collection + packetization + codec lookahead can already
        // exceed80ms when video establishes the clock. Reserve the decoder
        // runway plus the hardware quantum BEFORE rendering packet1.
        // Waiting200ms for pressure confirmation caused startup underruns.
        const auto headroom=AudioOutputTiming::requiredHeadroomUs(outputQuantum);
        const auto minimum=AudioOutputTiming::minimumPlayoutDelayUs(outputQuantum);
        const auto maximum=AudioOutputTiming::maximumPlayoutDelayUs(outputQuantum);
        if(margin<headroom) {
            healthySince=-1;
            target=std::clamp(std::max(target,lag+headroom),minimum,maximum);
        }

        // Recover the low-latency target slowly after a genuinely quiet path.
        if(target>minimum && margin>headroom) {
            if(healthySince<0) healthySince=arrivalUs;
            if(arrivalUs-healthySince>=30000000 && arrivalUs-lastDecrease>=1000000) {
                target=std::max(minimum,target-1000); lastDecrease=arrivalUs;
            }
        } else healthySince=-1;

        // Fresh packets can still be impossible to play within the bounded
        // deadline. Without this check, a stable120ms path could stay silent
        // forever while reporting "available". Exclude the optional jitter
        // allowance: only a full hardware quantum + recovery reservoir are
        // mandatory. One healthy packet ends the pressure interval.
        const bool unplayable=lag+AudioOutputTiming::normalizeQuantumUs(outputQuantum)
            +AudioOutputTiming::recoveryRunwayUs(outputQuantum)>maximum;
        if(lag<MaximumDelayUs && !unplayable) { stalledSince=-1; return false; }
        // Equal arrival times are valid for ordered packets delivered in one
        // event-loop tick. A burst alone still cannot satisfy the independent
        // one-second arrival and800ms source-progress requirements below.
        if(!cadence || stalledSince<0) { stalledSince=arrivalUs; stalledSource=sourceUs; }
        return arrivalUs-stalledSince>=1000000 && sourceUs-stalledSource>=800000;
    }
private:
    qint64 outputQuantum=AudioOutputTiming::DefaultQuantumUs;
    qint64 target=InitialDelayUs,lastSource=-1,lastArrival=-1;
    qint64 healthySince=-1,lastDecrease=0;
    qint64 stalledSince=-1,stalledSource=0;
};
