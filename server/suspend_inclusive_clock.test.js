'use strict';

const assert = require('node:assert/strict');
const { createSuspendInclusiveClock } = require('./suspend_inclusive_clock');
const { MouffetteServer } = require('./server');

let awake = 100000;
let continuous = 100003;
let reads = 0;
const clock = createSuspendInclusiveClock({ platform: 'linux',
    hrtimeMs: () => awake,
    readBootTimeMs: () => { ++reads; return Math.floor(continuous / 10) * 10; } });
assert.equal(clock(), awake);
const first = clock();
for (let index = 0; index < 100; ++index) clock();
assert.equal(reads, 1, 'ordinary clock reads share the 100 ms sample');
continuous += 6000;
assert.equal(clock(), first, 'the high-resolution source alone misses suspend');
assert.equal(clock.refresh() - first, 6000, 'authority refresh observes wake before accepting traffic');
const resumed = clock();
awake += 50; continuous += 50;
assert.equal(clock() - resumed, 50);

// Arbitrary quantization phases and repeated sleeps never accumulate drift.
// The conservative budget guard covers the one centisecond lower-bound error.
const anchor = clock.refresh();
const realAnchor = continuous;
for (let index = 0; index < 500; ++index) {
    const awakeStep = (index * 17) % 300;
    awake += awakeStep;
    continuous += awakeStep + (index * 31) % 6001;
    const observed = clock.refresh() - anchor;
    assert.ok(observed + clock.uncertaintyMs >= continuous - realAnchor);
    assert.ok(observed <= continuous - realAnchor + 10);
}

// The injected policy guard expires sessions before the detection plus 3-second recovery
// budget could be extended by the Linux counter's centisecond quantization.
const server = new MouffetteServer({ port: 0, monotonicNow: clock,
    protocolLogger: () => {}, metricLogger: () => {} });
assert.equal(server.remoteSessions.leaseTimeoutMs, 4490);
const session = server.remoteSessions.open({ ownerEndpointId: 'A', targetEndpointId: 'B',
    ownerRuntimeId: 'a', targetRuntimeId: 'b', ownerConnectionGeneration: 1,
    targetConnectionGeneration: 1 }).session;
continuous += 4500;
server.sweepRemoteSessionLeases();
assert.equal(server.remoteSessions.sessions.has(session.remoteSessionId), false);

let native = 1000;
for (const platform of ['darwin', 'win32']) {
    const nativeClock = createSuspendInclusiveClock({ platform, hrtimeMs: () => native,
        readBootTimeMs: () => { throw new Error('must not use uptime/civil time'); } });
    assert.equal(nativeClock.uncertaintyMs, 0);
    const before = nativeClock();
    native += 6000;
    assert.equal(nativeClock.refresh() - before, 6000);
}
const badClock = createSuspendInclusiveClock({ platform: 'linux', hrtimeMs: () => 0,
    readBootTimeMs: () => NaN });
assert.throws(() => badClock.refresh(), /kernel clock is invalid/);
console.log('suspend-inclusive clock tests passed');
