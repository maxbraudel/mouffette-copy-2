'use strict';

const fs = require('node:fs');

function readLinuxBootTimeMs() {
    // procfs is a kernel-generated counter, not a disk file or civil clock.
    // Requiring its centisecond representation avoids os.uptime's coarser
    // fallback on platforms/containers where procfs is unavailable.
    const text = fs.readFileSync('/proc/uptime', 'utf8');
    const match = /^(\d+\.\d{2,})\s/.exec(text);
    if (!match) throw new Error('Suspend-inclusive /proc/uptime is unavailable');
    return Number(match[1]) * 1000;
}

function createSuspendInclusiveClock(options = {}) {
    const platform = options.platform || process.platform;
    const hrtimeMs = options.hrtimeMs
        || (() => Number(process.hrtime.bigint() / 1_000_000n));
    const readBootTimeMs = options.readBootTimeMs || readLinuxBootTimeMs;
    const sampleIntervalMs = options.sampleIntervalMs ?? 100;
    let baseBoot = null;
    let baseHr = 0;
    let previousBoot = 0;
    let lastSampleHr = -Infinity;
    let correction = 0;
    let last = 0;

    function sample(force = false) {
        const before = hrtimeMs();
        if (platform === 'linux' && (force || before - lastSampleHr >= sampleIntervalMs)) {
            const boot = readBootTimeMs();
            if (!Number.isFinite(boot) || boot < 0 || (baseBoot !== null && boot < previousBoot))
                throw new Error('Suspend-inclusive kernel clock is invalid or regressed');
            if (baseBoot === null) {
                baseBoot = boot;
                baseHr = hrtimeMs();
            } else {
                // Use the pre-read hrtime here, then return post-read time:
                // read latency can only shorten authority budgets. Quantized
                // boot-time differences can undercount suspend by < 10 ms.
                correction = Math.max(correction, boot - baseBoot - (before - baseHr));
            }
            previousBoot = boot;
            lastSampleHr = before;
        }
        last = Math.max(last, hrtimeMs() + correction);
        return last;
    }

    const now = () => sample(false);
    // Force a sample before accepting a network message or processing a lease
    // deadline: hrtime itself may barely advance across a Linux suspend.
    now.refresh = () => sample(true);
    now.uncertaintyMs = platform === 'linux' ? 10 : 0;
    return now;
}

const monotonicNow = createSuspendInclusiveClock();
module.exports = { createSuspendInclusiveClock, monotonicNow, readLinuxBootTimeMs };
