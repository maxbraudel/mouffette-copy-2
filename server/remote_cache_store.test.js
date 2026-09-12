'use strict';

const assert = require('node:assert/strict');
const fs = require('node:fs');
const os = require('node:os');
const path = require('node:path');

const { RemoteCacheStore } = require('./remote_cache_store');

const fsp = fs.promises;

const SENDER = 'sender_A-0123456789';
const SESSION = '11111111-2222-4333-8444-555555555555';

function binding(generation = 1) {
    return {
        senderEndpointId: SENDER,
        remoteSessionId: SESSION,
        generation,
        teardownId: `teardown-${generation}`,
    };
}

function simulatedCrash() {
    const error = new Error('simulated process exit');
    error.simulateCrash = true;
    return error;
}

async function temporaryUploads() {
    const temporary = await fsp.mkdtemp(path.join(os.tmpdir(), 'mouffette-cache-'));
    return { temporary, uploadsRoot: path.join(temporary, 'Uploads') };
}

async function runScheduled(queue) {
    while (queue.length > 0) await queue.shift()();
}

async function pathExists(candidate) {
    try {
        await fsp.lstat(candidate);
        return true;
    } catch (error) {
        if (error.code === 'ENOENT') return false;
        throw error;
    }
}

async function quarantineEntries(uploadsRoot) {
    return fsp.readdir(path.join(
        uploadsRoot, '.remote-cache-state', 'quarantine'));
}

async function testAtomicQuarantineAndReplay() {
    const fixture = await temporaryUploads();
    const scheduled = [];
    const store = new RemoteCacheStore({
        uploadsRoot: fixture.uploadsRoot,
        scheduler: task => scheduled.push(task),
    });
    try {
        await store.initialize();
        const active = await store.ensureActiveCache(binding(2));
        assert.equal(active.ok, true);
        await fsp.writeFile(path.join(active.cachePath, 'validated.bin'), 'asset');

        const committed = await store.commitCleanup(binding(2));
        assert.equal(committed.ok, true);
        assert.equal(committed.scope, 'server_cache');
        assert.equal(committed.state, 'committed');
        assert.equal(committed.logicalCommitted, true);
        assert.equal(committed.cacheQuarantined, true);
        assert.equal(committed.hadActiveCache, true);
        assert.equal(await pathExists(active.cachePath), false,
            'logical commit must make the active cache inaccessible');
        const quarantined = await quarantineEntries(fixture.uploadsRoot);
        assert.equal(quarantined.length, 1);
        const quarantineRoot = path.join(
            fixture.uploadsRoot, '.remote-cache-state', 'quarantine');
        if (process.platform !== 'win32') {
            assert.equal((await fsp.stat(quarantineRoot)).mode & 0o777, 0o700);
            assert.equal(
                (await fsp.stat(path.join(quarantineRoot, quarantined[0]))).mode & 0o777,
                0o700, 'the quarantined cache must remain behind private permissions');
        }

        const replay = await store.commitCleanup(binding(2));
        assert.equal(replay.ok, true);
        assert.equal(replay.replay, true);
        assert.equal(scheduled.length, 1, 'a replay must not queue duplicate deletion');
        const mismatchedTeardown = await store.commitCleanup({
            ...binding(2), teardownId: 'different-teardown',
        });
        assert.equal(mismatchedTeardown.ok, false);
        assert.equal(mismatchedTeardown.errorCode, 'teardown_mismatch');

        const stale = await store.commitCleanup(binding(1));
        assert.equal(stale.ok, false);
        assert.equal(stale.errorCode, 'stale_generation');
        assert.equal((await store.ensureActiveCache(binding(2))).errorCode,
            'generation_already_cleaned');

        await runScheduled(scheduled);
        await store.waitForIdle();
        const deleted = await store.cleanupStatus(binding());
        assert.equal(deleted.state, 'deleted');
        assert.equal(deleted.removedFileCount, 1);
        assert.deepEqual(await quarantineEntries(fixture.uploadsRoot), []);

        const next = await store.ensureActiveCache(binding(3));
        assert.equal(next.ok, true, 'a strictly newer generation may create a new cache');
        const nextCommit = await store.commitCleanup(binding(3));
        assert.equal(nextCommit.generation, 3);
        assert.equal(nextCommit.cacheQuarantined, true,
            'an empty cleanup still satisfies the quarantine postcondition');
        assert.equal(nextCommit.hadActiveCache, true);
        await runScheduled(scheduled);
        await store.waitForIdle();

        const latest = await store.ensureActiveCache(binding(5));
        assert.equal(latest.ok, true);
        await fsp.writeFile(path.join(latest.cachePath, 'latest.bin'), 'latest');
        const restartedStore = new RemoteCacheStore({
            uploadsRoot: fixture.uploadsRoot,
            scheduler: task => scheduled.push(task),
        });
        await restartedStore.initialize();
        const staleActiveCleanup = await restartedStore.commitCleanup(binding(4));
        assert.equal(staleActiveCleanup.errorCode, 'stale_generation');
        assert.equal(await pathExists(latest.cachePath), true,
            'an older teardown must never quarantine a newer active generation');
        assert.equal((await restartedStore.commitCleanup(binding(5))).ok, true);
        await runScheduled(scheduled);
        await restartedStore.waitForIdle();
    } finally {
        await fsp.rm(fixture.temporary, { recursive: true, force: true });
    }
}

async function testConcurrentEmptyCleanupIsIdempotent() {
    const fixture = await temporaryUploads();
    const scheduled = [];
    const store = new RemoteCacheStore({
        uploadsRoot: fixture.uploadsRoot,
        scheduler: task => scheduled.push(task),
    });
    try {
        await store.initialize();
        const [first, replay] = await Promise.all([
            store.commitCleanup(binding(8)),
            store.commitCleanup(binding(8)),
        ]);
        assert.equal(first.ok, true);
        assert.equal(first.replay, false);
        assert.equal(first.cacheQuarantined, true,
            'an absent cache already satisfies the quarantine postcondition');
        assert.equal(first.hadActiveCache, false);
        assert.equal(replay.replay, true);
        assert.equal(scheduled.length, 1);
        await runScheduled(scheduled);
        await store.waitForIdle();
        assert.equal((await store.cleanupStatus(binding())).state, 'deleted');
    } finally {
        await fsp.rm(fixture.temporary, { recursive: true, force: true });
    }
}

async function testTraversalAndSymlinksNeverEscapeRoot() {
    const fixture = await temporaryUploads();
    const external = path.join(fixture.temporary, 'external');
    await fsp.mkdir(external);
    await fsp.writeFile(path.join(external, 'keep.txt'), 'do not delete');
    const store = new RemoteCacheStore({ uploadsRoot: fixture.uploadsRoot });
    try {
        await store.initialize();
        assert.equal((await store.commitCleanup({
            senderEndpointId: '../external',
            remoteSessionId: SESSION,
            generation: 1,
        })).errorCode, 'invalid_sender_endpoint_id');
        assert.equal((await store.commitCleanup({
            senderEndpointId: SENDER,
            remoteSessionId: '../../external',
            generation: 1,
        })).errorCode, 'invalid_remote_session_id');

        await fsp.symlink(external, path.join(fixture.uploadsRoot, SENDER));
        const unsafe = await store.commitCleanup(binding(1));
        assert.equal(unsafe.ok, false);
        assert.equal(unsafe.state, 'cleanup_error');
        assert.equal(unsafe.errorCode, 'unsafe_symlink');
        assert.equal(JSON.stringify(unsafe).includes(fixture.temporary), false,
            'sanitized failures must not expose a filesystem path');
        assert.equal(await fsp.readFile(path.join(external, 'keep.txt'), 'utf8'),
            'do not delete');

        await fsp.unlink(path.join(fixture.uploadsRoot, SENDER));
        const recovered = await store.sweep();
        assert.equal(recovered.recovered, 1,
            'the failed intent must resolve before a newer cache can be created');
        await store.waitForIdle();
        const active = await store.ensureActiveCache(binding(2));
        assert.equal(active.ok, true);
        const link = path.join(active.cachePath, 'external-link');
        await fsp.symlink(path.join(external, 'keep.txt'), link);
        assert.equal((await store.commitCleanup(binding(2))).ok, true);
        await store.waitForIdle();
        assert.equal(await fsp.readFile(path.join(external, 'keep.txt'), 'utf8'),
            'do not delete', 'physical deletion must unlink, never follow, a symlink');

        const secondSender = 'sender_B-0123456789';
        const secondSession = '22222222-3333-4444-8555-666666666666';
        const secondSenderPath = path.join(fixture.uploadsRoot, secondSender);
        await fsp.mkdir(secondSenderPath, { mode: 0o700 });
        await fsp.symlink(external, path.join(secondSenderPath, secondSession));
        const unsafeSessionLink = await store.commitCleanup({
            senderEndpointId: secondSender,
            remoteSessionId: secondSession,
            generation: 1,
            teardownId: 'teardown-session-link',
        });
        assert.equal(unsafeSessionLink.errorCode, 'unsafe_symlink');
        assert.equal(await fsp.readFile(path.join(external, 'keep.txt'), 'utf8'),
            'do not delete');
    } finally {
        await fsp.rm(fixture.temporary, { recursive: true, force: true });
    }
}

async function testCrashAfterIntentRecoversAndQuarantines() {
    const fixture = await temporaryUploads();
    const crashedStore = new RemoteCacheStore({
        uploadsRoot: fixture.uploadsRoot,
        scheduler: () => {},
        faultInjector(point) {
            if (point === 'after_intent_persisted') throw simulatedCrash();
        },
    });
    try {
        await crashedStore.initialize();
        const active = await crashedStore.ensureActiveCache(binding(6));
        await fsp.writeFile(path.join(active.cachePath, 'asset.dat'), 'content');
        await assert.rejects(crashedStore.commitCleanup(binding(6)), error =>
            error && error.simulateCrash === true);
        assert.equal(await pathExists(active.cachePath), true,
            'a crash before rename leaves the active cache for journal recovery');

        const scheduled = [];
        const recoveredStore = new RemoteCacheStore({
            uploadsRoot: fixture.uploadsRoot,
            scheduler: task => scheduled.push(task),
        });
        const startup = await recoveredStore.initialize();
        assert.equal(startup.ok, true);
        assert.equal(startup.recovered, 1);
        assert.equal(await pathExists(active.cachePath), false,
            'startup must finish the atomic quarantine from the durable intent');
        assert.equal((await recoveredStore.cleanupStatus(binding())).generation, 6);
        await runScheduled(scheduled);
        await recoveredStore.waitForIdle();
    } finally {
        await fsp.rm(fixture.temporary, { recursive: true, force: true });
    }
}

async function testStartupSweepDeletesOrphanQuarantine() {
    const fixture = await temporaryUploads();
    const scheduled = [];
    const store = new RemoteCacheStore({
        uploadsRoot: fixture.uploadsRoot,
        scheduler: task => scheduled.push(task),
    });
    try {
        await store.initialize();
        const orphan = path.join(
            fixture.uploadsRoot, '.remote-cache-state', 'quarantine', 'q-orphan');
        await fsp.mkdir(orphan, { mode: 0o700 });
        await fsp.writeFile(path.join(orphan, 'leftover.bin'), 'leftover');
        const sweep = await store.sweep();
        assert.equal(sweep.ok, true);
        assert.equal(sweep.deletionScheduled, 1);
        await runScheduled(scheduled);
        await store.waitForIdle();
        assert.equal(await pathExists(orphan), false);
    } finally {
        await fsp.rm(fixture.temporary, { recursive: true, force: true });
    }
}

async function testCrashAfterRenameRecoversFromIntent() {
    const fixture = await temporaryUploads();
    let crashEnabled = true;
    const crashedStore = new RemoteCacheStore({
        uploadsRoot: fixture.uploadsRoot,
        scheduler: () => {},
        faultInjector(point) {
            if (crashEnabled && point === 'after_quarantine_rename') {
                throw simulatedCrash();
            }
        },
    });
    try {
        await crashedStore.initialize();
        const active = await crashedStore.ensureActiveCache(binding(4));
        await fsp.writeFile(path.join(active.cachePath, 'asset.dat'), 'content');
        await assert.rejects(crashedStore.commitCleanup(binding(4)), error =>
            error && error.simulateCrash === true);
        crashEnabled = false;
        assert.equal(await pathExists(active.cachePath), false);
        assert.equal((await quarantineEntries(fixture.uploadsRoot)).length, 1);

        const scheduled = [];
        const recoveredStore = new RemoteCacheStore({
            uploadsRoot: fixture.uploadsRoot,
            scheduler: task => scheduled.push(task),
        });
        const startup = await recoveredStore.initialize();
        assert.equal(startup.ok, true);
        assert.equal(startup.recovered, 1,
            'startup sweep must commit the pre-rename durable intent');
        const status = await recoveredStore.cleanupStatus(binding());
        assert.equal(status.logicalCommitted, true);
        assert.equal(status.generation, 4);
        assert.equal((await recoveredStore.commitCleanup(binding(4))).replay, true);
        await runScheduled(scheduled);
        await recoveredStore.waitForIdle();
        assert.equal((await recoveredStore.cleanupStatus(binding())).state, 'deleted');
    } finally {
        await fsp.rm(fixture.temporary, { recursive: true, force: true });
    }
}

async function testDeletionFailureIsDurableAndRetried() {
    const fixture = await temporaryUploads();
    let failDeletion = true;
    const store = new RemoteCacheStore({
        uploadsRoot: fixture.uploadsRoot,
        faultInjector(point) {
            if (failDeletion && point === 'before_physical_delete') {
                throw new Error('injected deletion failure');
            }
        },
    });
    try {
        await store.initialize();
        const active = await store.ensureActiveCache(binding(5));
        await fsp.writeFile(path.join(active.cachePath, 'asset.dat'), 'content');
        assert.equal((await store.commitCleanup(binding(5))).state, 'committed');
        await store.waitForIdle();
        const failed = await store.cleanupStatus(binding());
        assert.equal(failed.ok, true, 'logical commit remains valid after delete failure');
        assert.equal(failed.state, 'cleanup_error');
        assert.equal(failed.logicalCommitted, true);
        assert.equal((await quarantineEntries(fixture.uploadsRoot)).length, 1);

        failDeletion = false;
        const recoveredStore = new RemoteCacheStore({ uploadsRoot: fixture.uploadsRoot });
        const restart = await recoveredStore.initialize();
        assert.equal(restart.deletionScheduled, 1,
            'startup must retry a durable cleanup_error');
        await recoveredStore.waitForIdle();
        assert.equal((await recoveredStore.cleanupStatus(binding())).state, 'deleted');
        assert.deepEqual(await quarantineEntries(fixture.uploadsRoot), []);
    } finally {
        await fsp.rm(fixture.temporary, { recursive: true, force: true });
    }
}

(async () => {
    await testAtomicQuarantineAndReplay();
    await testConcurrentEmptyCleanupIsIdempotent();
    await testTraversalAndSymlinksNeverEscapeRoot();
    await testCrashAfterRenameRecoversFromIntent();
    await testCrashAfterIntentRecoversAndQuarantines();
    await testStartupSweepDeletesOrphanQuarantine();
    await testDeletionFailureIsDurableAndRetried();
    console.log('remote cache store tests passed');
})().catch(error => {
    console.error(error);
    process.exitCode = 1;
});
