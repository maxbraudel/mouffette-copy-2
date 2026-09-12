'use strict';

const crypto = require('node:crypto');
const { EventEmitter } = require('node:events');
const fs = require('node:fs');
const path = require('node:path');

const fsp = fs.promises;

const STATE_VERSION = 1;
const CACHE_SCOPE = 'server_cache';
const IDENTIFIER_PATTERN = /^[A-Za-z0-9][A-Za-z0-9_-]{0,127}$/;
const OPAQUE_NAME_PATTERN = /^[A-Za-z0-9][A-Za-z0-9_-]{0,127}$/;
const WINDOWS_RESERVED_NAME = /^(con|prn|aux|nul|com[1-9]|lpt[1-9])$/i;
const TOMBSTONE_STATES = new Set(['committed', 'deleted', 'cleanup_error']);
const TRANSACTION_STATES = new Set(['prepared', 'committed', 'cleanup_error']);
const MAX_STATE_FILE_BYTES = 64 * 1024;

class RemoteCacheStoreError extends Error {
    constructor(code) {
        super(code);
        this.name = 'RemoteCacheStoreError';
        this.code = code;
    }
}

function errorCode(error) {
    if (error instanceof RemoteCacheStoreError) return error.code;
    switch (error && error.code) {
    case 'EACCES':
    case 'EPERM': return 'permission_denied';
    case 'ENOSPC': return 'storage_full';
    case 'EROFS': return 'read_only_storage';
    case 'EMFILE':
    case 'ENFILE': return 'storage_busy';
    default: return 'storage_io_error';
    }
}

function isMissing(error) {
    return !!error && error.code === 'ENOENT';
}

function isSimulatedCrash(error) {
    return !!error && error.simulateCrash === true;
}

function assertIdentifier(value, code) {
    if (typeof value !== 'string' || !IDENTIFIER_PATTERN.test(value)
        || WINDOWS_RESERVED_NAME.test(value)) {
        throw new RemoteCacheStoreError(code);
    }
    return value;
}

function assertGeneration(value) {
    if (!Number.isSafeInteger(value) || value < 1) {
        throw new RemoteCacheStoreError('invalid_generation');
    }
    return value;
}

/**
 * Transactional owner of server-side remote-session caches.
 *
 * Only identifiers are accepted from callers. Active paths are derived as
 * Uploads/<senderEndpointId>/<remoteSessionId>; callers can never nominate a
 * cleanup source or quarantine destination. A logical cleanup is durable once
 * the active directory has been atomically renamed and its tombstone written.
 */
class RemoteCacheStore extends EventEmitter {
    constructor(options = {}) {
        super();
        if (typeof options.uploadsRoot !== 'string' || options.uploadsRoot.length === 0
            || options.uploadsRoot.includes('\0')) {
            throw new RemoteCacheStoreError('invalid_uploads_root');
        }

        this.uploadsRoot = path.resolve(options.uploadsRoot);
        if (this.uploadsRoot === path.parse(this.uploadsRoot).root) {
            throw new RemoteCacheStoreError('invalid_uploads_root');
        }

        this.stateRoot = path.join(this.uploadsRoot, '.remote-cache-state');
        this.quarantineRoot = path.join(this.stateRoot, 'quarantine');
        this.activeStateRoot = path.join(this.stateRoot, 'active');
        this.transactionsRoot = path.join(this.stateRoot, 'transactions');
        this.tombstonesRoot = path.join(this.stateRoot, 'tombstones');
        this.now = options.now || (() => Date.now());
        this.idFactory = options.idFactory || (() => crypto.randomUUID());
        this.scheduler = options.scheduler || (task => setImmediate(task));
        this.faultInjector = options.faultInjector || null;

        this.tombstones = new Map(); // binding key -> latest committed generation
        this.activeGenerations = new Map(); // binding key -> latest opened generation
        this.transactions = new Map(); // transaction id -> durable cleanup intent
        this.deletionJobs = new Map(); // quarantine name -> Promise
        this.startupErrors = [];
        this.initializationPromise = null;
        this.exclusiveTail = Promise.resolve();
    }

    /** Initializes storage and resumes incomplete cleanups. Safe to replay. */
    async initialize() {
        try {
            await this.#ensureInitialized();
        } catch (error) {
            const code = errorCode(error);
            this.#emitCleanupError(code);
            return {
                ok: false,
                scope: CACHE_SCOPE,
                state: 'cleanup_error',
                recovered: 0,
                deletionScheduled: 0,
                errors: [code],
            };
        }
        const sweep = await this.sweep();
        const errors = [...this.startupErrors, ...sweep.errors];
        this.startupErrors = [];
        return {
            ok: errors.length === 0,
            scope: CACHE_SCOPE,
            state: errors.length === 0 ? 'committed' : 'cleanup_error',
            recovered: sweep.recovered,
            deletionScheduled: sweep.deletionScheduled,
            errors,
        };
    }

    /** Returns the validated active cache path; it performs no filesystem I/O. */
    activeCachePath(binding) {
        const normalized = this.#normalizeBinding(binding, false);
        return this.#activePath(normalized);
    }

    /** Creates (or reuses) a generation's active cache directory securely. */
    async ensureActiveCache(binding) {
        let normalized;
        try {
            normalized = this.#normalizeBinding(binding, true);
            await this.#ensureInitialized();
        } catch (error) {
            return this.#failure(errorCode(error));
        }

        return this.#exclusive(async () => {
            const tombstone = this.tombstones.get(normalized.key);
            if (tombstone && tombstone.generation >= normalized.generation) {
                return this.#failure(tombstone.generation === normalized.generation
                    ? 'generation_already_cleaned' : 'stale_generation', {
                    committedGeneration: tombstone.generation,
                });
            }
            const pending = this.#transactionsForKey(normalized.key)
                .find(transaction => {
                    const durable = this.tombstones.get(transaction.key);
                    return !durable || durable.generation < transaction.generation;
                });
            if (pending) {
                return this.#failure('cleanup_in_progress', {
                    committedGeneration: tombstone ? tombstone.generation : undefined,
                });
            }

            const currentActive = this.activeGenerations.get(normalized.key);
            if (currentActive && currentActive.generation > normalized.generation) {
                return this.#failure('stale_generation', {
                    activeGeneration: currentActive.generation,
                });
            }

            try {
                const activeRecord = {
                    version: STATE_VERSION,
                    key: normalized.key,
                    senderEndpointId: normalized.senderEndpointId,
                    remoteSessionId: normalized.remoteSessionId,
                    generation: normalized.generation,
                    updatedAt: this.now(),
                };
                await this.#writeActiveGeneration(activeRecord);
                this.activeGenerations.set(normalized.key, activeRecord);
                const senderPath = path.join(this.uploadsRoot, normalized.senderEndpointId);
                await this.#ensureChildDirectory(this.uploadsRoot, senderPath, true);
                const cachePath = this.#activePath(normalized);
                await this.#ensureChildDirectory(senderPath, cachePath, true);
                return {
                    ok: true,
                    scope: CACHE_SCOPE,
                    state: 'active',
                    generation: normalized.generation,
                    cachePath,
                };
            } catch (error) {
                const code = errorCode(error);
                this.#emitCleanupError(code, normalized);
                return this.#failure(code);
            }
        });
    }

    /**
     * Atomically hides a cache, commits its generation tombstone, then queues
     * physical deletion. Replays for the same generation return the tombstone.
     */
    async commitCleanup(binding) {
        let normalized;
        try {
            normalized = this.#normalizeBinding(binding, true, true);
            await this.#ensureInitialized();
        } catch (error) {
            return this.#failure(errorCode(error));
        }

        return this.#exclusive(async () => {
            const committed = this.tombstones.get(normalized.key);
            if (committed && committed.generation > normalized.generation) {
                return this.#failure('stale_generation', {
                    committedGeneration: committed.generation,
                });
            }
            const activeGeneration = this.activeGenerations.get(normalized.key);
            if (activeGeneration && activeGeneration.generation > normalized.generation) {
                return this.#failure('stale_generation', {
                    activeGeneration: activeGeneration.generation,
                });
            }
            if (committed && committed.generation === normalized.generation) {
                if (committed.teardownId !== normalized.teardownId) {
                    return this.#failure('teardown_mismatch', {
                        committedGeneration: committed.generation,
                    });
                }
                await this.#clearActiveGeneration(committed).catch(error =>
                    this.#emitCleanupError(errorCode(error), normalized));
                this.#scheduleDeletionForTombstone(committed);
                return this.#publicTombstone(committed, true);
            }

            // Finish older durable intents first. They can own a quarantine that
            // must not be lost when a later generation supersedes its tombstone.
            for (const older of this.#transactionsForKey(normalized.key)) {
                if (older.generation >= normalized.generation) continue;
                try {
                    const recovered = await this.#advanceTransaction(older);
                    this.#scheduleDeletion(older, recovered);
                } catch (error) {
                    if (isSimulatedCrash(error)) throw error;
                    const code = errorCode(error);
                    await this.#recordTransactionError(older, code);
                    return this.#failure('prior_cleanup_incomplete', {
                        priorErrorCode: code,
                    });
                }
            }

            let transaction = this.#transactionsForKey(normalized.key)
                .find(candidate => candidate.generation === normalized.generation);
            if (!transaction) {
                try {
                    transaction = this.#newTransaction(normalized);
                    await this.#writeTransaction(transaction);
                    this.transactions.set(transaction.transactionId, transaction);
                    await this.#fault('after_intent_persisted', normalized);
                } catch (error) {
                    if (isSimulatedCrash(error)) throw error;
                    const code = errorCode(error);
                    this.#emitCleanupError(code, normalized);
                    return this.#failure(code);
                }
            }

            try {
                const tombstone = await this.#advanceTransaction(transaction);
                await this.#fault('after_logical_commit', normalized);
                this.#scheduleDeletion(transaction, tombstone);
                return this.#publicTombstone(tombstone, false);
            } catch (error) {
                if (isSimulatedCrash(error)) throw error;
                const code = errorCode(error);
                const durable = this.tombstones.get(normalized.key);
                if (durable && durable.generation === normalized.generation) {
                    this.#scheduleDeletion(transaction, durable);
                    return this.#publicTombstone(durable, false);
                }
                await this.#recordTransactionError(transaction, code);
                return this.#failure(code, {
                    cacheQuarantined: transaction.cacheQuarantined,
                });
            }
        });
    }

    /** Returns the latest durable state without exposing filesystem paths. */
    async cleanupStatus(binding) {
        let normalized;
        try {
            normalized = this.#normalizeBinding(binding, false);
            await this.#ensureInitialized();
        } catch (error) {
            return this.#failure(errorCode(error));
        }
        return this.#exclusive(async () => {
            const tombstone = this.tombstones.get(normalized.key);
            if (tombstone) return this.#publicTombstone(tombstone, true);
            const pending = this.#transactionsForKey(normalized.key).at(-1);
            if (!pending) return null;
            return {
                ok: false,
                scope: CACHE_SCOPE,
                state: pending.state === 'cleanup_error' ? 'cleanup_error' : 'pending',
                logicalCommitted: false,
                generation: pending.generation,
                errorCode: pending.errorCode,
            };
        });
    }

    /** Resumes journals and removes orphaned quarantine entries. */
    async sweep() {
        try {
            await this.#ensureInitialized();
        } catch (error) {
            const code = errorCode(error);
            this.#emitCleanupError(code);
            return {
                ok: false,
                scope: CACHE_SCOPE,
                state: 'cleanup_error',
                recovered: 0,
                deletionScheduled: 0,
                errors: [code],
            };
        }
        return this.#exclusive(async () => {
            const summary = { recovered: 0, deletionScheduled: 0, errors: [] };
            for (const transaction of this.#orderedTransactions()) {
                try {
                    let tombstone = this.tombstones.get(transaction.key);
                    if (!tombstone || tombstone.generation < transaction.generation) {
                        tombstone = await this.#advanceTransaction(transaction);
                        summary.recovered += 1;
                    }
                    if (this.#scheduleDeletion(transaction, tombstone)) {
                        summary.deletionScheduled += 1;
                    }
                } catch (error) {
                    if (isSimulatedCrash(error)) throw error;
                    const code = errorCode(error);
                    summary.errors.push(code);
                    await this.#recordTransactionError(transaction, code);
                }
            }

            const referenced = new Set(Array.from(this.transactions.values(),
                transaction => transaction.quarantineName));
            let entries = [];
            try {
                await this.#assertDirectoryNoSymlink(
                    this.quarantineRoot, 'unsafe_quarantine_root');
                entries = await fsp.readdir(this.quarantineRoot, { withFileTypes: true });
            } catch (error) {
                if (!isMissing(error)) summary.errors.push(errorCode(error));
            }
            for (const entry of entries) {
                if (!OPAQUE_NAME_PATTERN.test(entry.name)) {
                    summary.errors.push('unsafe_quarantine_entry');
                    continue;
                }
                if (!referenced.has(entry.name)
                    && this.#scheduleOrphanDeletion(entry.name)) {
                    summary.deletionScheduled += 1;
                }
            }
            return {
                ok: summary.errors.length === 0,
                scope: CACHE_SCOPE,
                state: summary.errors.length === 0 ? 'committed' : 'cleanup_error',
                ...summary,
            };
        });
    }

    /** Waits for all currently scheduled physical deletion jobs. */
    async waitForIdle() {
        while (this.deletionJobs.size > 0) {
            await Promise.allSettled(Array.from(this.deletionJobs.values()));
        }
    }

    async close() {
        await this.waitForIdle();
    }

    #normalizeBinding(binding, requireGeneration, requireTeardown = false) {
        if (!binding || typeof binding !== 'object' || Array.isArray(binding)) {
            throw new RemoteCacheStoreError('invalid_cache_binding');
        }
        const senderEndpointId = assertIdentifier(
            binding.senderEndpointId, 'invalid_sender_endpoint_id');
        const remoteSessionId = assertIdentifier(
            binding.remoteSessionId, 'invalid_remote_session_id');
        const normalized = {
            senderEndpointId,
            remoteSessionId,
            key: this.#bindingKey(senderEndpointId, remoteSessionId),
        };
        if (requireGeneration) normalized.generation = assertGeneration(binding.generation);
        if (requireTeardown && binding.teardownId === undefined) {
            throw new RemoteCacheStoreError('invalid_teardown_id');
        }
        if (binding.teardownId !== undefined) {
            normalized.teardownId = assertIdentifier(binding.teardownId, 'invalid_teardown_id');
        }
        return normalized;
    }

    #bindingKey(senderEndpointId, remoteSessionId) {
        return crypto.createHash('sha256')
            .update(senderEndpointId, 'utf8')
            .update('\0', 'utf8')
            .update(remoteSessionId, 'utf8')
            .digest('hex');
    }

    #activePath(binding) {
        const candidate = path.join(
            this.uploadsRoot, binding.senderEndpointId, binding.remoteSessionId);
        this.#assertContained(this.uploadsRoot, candidate);
        return candidate;
    }

    #quarantinePath(name) {
        if (!OPAQUE_NAME_PATTERN.test(name)) {
            throw new RemoteCacheStoreError('invalid_quarantine_record');
        }
        const candidate = path.join(this.quarantineRoot, name);
        this.#assertContained(this.quarantineRoot, candidate);
        return candidate;
    }

    #assertContained(parent, candidate) {
        const relative = path.relative(parent, candidate);
        if (!relative || relative.startsWith(`..${path.sep}`) || relative === '..'
            || path.isAbsolute(relative)) {
            throw new RemoteCacheStoreError('path_escape_rejected');
        }
    }

    async #ensureInitialized() {
        if (!this.initializationPromise) {
            this.initializationPromise = this.#setupStorage();
        }
        await this.initializationPromise;
    }

    async #setupStorage() {
        await fsp.mkdir(this.uploadsRoot, { recursive: true, mode: 0o700 });
        await this.#assertDirectoryNoSymlink(this.uploadsRoot, 'unsafe_uploads_root');
        await this.#ensureChildDirectory(this.uploadsRoot, this.stateRoot, true);
        await this.#ensureChildDirectory(this.stateRoot, this.quarantineRoot, true);
        await this.#ensureChildDirectory(this.stateRoot, this.activeStateRoot, true);
        await this.#ensureChildDirectory(this.stateRoot, this.transactionsRoot, true);
        await this.#ensureChildDirectory(this.stateRoot, this.tombstonesRoot, true);
        await this.#loadState();
    }

    async #ensureChildDirectory(parent, child, privateDirectory) {
        this.#assertContained(parent, child);
        await this.#assertDirectoryNoSymlink(parent, 'unsafe_parent_directory');
        try {
            await fsp.mkdir(child, { mode: 0o700 });
        } catch (error) {
            if (error.code !== 'EEXIST') throw error;
        }
        await this.#assertDirectoryNoSymlink(child, 'unsafe_symlink');
        if (privateDirectory) await fsp.chmod(child, 0o700);
    }

    async #assertDirectoryNoSymlink(candidate, code) {
        let stat;
        try {
            stat = await fsp.lstat(candidate);
        } catch (error) {
            if (isMissing(error)) throw new RemoteCacheStoreError('missing_storage_directory');
            throw error;
        }
        if (stat.isSymbolicLink() || !stat.isDirectory()) {
            throw new RemoteCacheStoreError(code);
        }
    }

    async #activeState(binding) {
        const senderPath = path.join(this.uploadsRoot, binding.senderEndpointId);
        let senderStat;
        try {
            senderStat = await fsp.lstat(senderPath);
        } catch (error) {
            if (isMissing(error)) return { exists: false, path: this.#activePath(binding) };
            throw error;
        }
        if (senderStat.isSymbolicLink() || !senderStat.isDirectory()) {
            throw new RemoteCacheStoreError('unsafe_symlink');
        }

        const activePath = this.#activePath(binding);
        let activeStat;
        try {
            activeStat = await fsp.lstat(activePath);
        } catch (error) {
            if (isMissing(error)) return { exists: false, path: activePath };
            throw error;
        }
        if (activeStat.isSymbolicLink() || !activeStat.isDirectory()) {
            throw new RemoteCacheStoreError('unsafe_symlink');
        }
        return { exists: true, path: activePath, senderPath };
    }

    #newTransaction(binding) {
        let transactionId = null;
        for (let attempt = 0; attempt < 16; ++attempt) {
            transactionId = `q-${String(this.idFactory()).replace(/-/g, '')}`;
            const tombstoneUsesId = Array.from(this.tombstones.values())
                .some(tombstone => tombstone.transactionId === transactionId);
            if (OPAQUE_NAME_PATTERN.test(transactionId)
                && !this.transactions.has(transactionId) && !tombstoneUsesId) break;
            transactionId = null;
        }
        if (!transactionId) throw new RemoteCacheStoreError('id_generation_failed');
        const now = this.now();
        return {
            version: STATE_VERSION,
            transactionId,
            key: binding.key,
            senderEndpointId: binding.senderEndpointId,
            remoteSessionId: binding.remoteSessionId,
            generation: binding.generation,
            teardownId: binding.teardownId,
            quarantineName: transactionId,
            cacheQuarantined: false,
            state: 'prepared',
            createdAt: now,
            updatedAt: now,
        };
    }

    async #advanceTransaction(transaction) {
        const current = this.tombstones.get(transaction.key);
        if (current && current.generation >= transaction.generation) {
            await this.#clearActiveGeneration(transaction).catch(error =>
                this.#emitCleanupError(errorCode(error), transaction));
            return current;
        }
        const activeGeneration = this.activeGenerations.get(transaction.key);
        if (activeGeneration && activeGeneration.generation > transaction.generation) {
            throw new RemoteCacheStoreError('stale_generation');
        }

        const active = await this.#activeState(transaction);
        await this.#assertDirectoryNoSymlink(
            this.quarantineRoot, 'unsafe_quarantine_root');
        const quarantinePath = this.#quarantinePath(transaction.quarantineName);
        let quarantineExists = false;
        try {
            const stat = await fsp.lstat(quarantinePath);
            if (stat.isSymbolicLink() || !stat.isDirectory()) {
                throw new RemoteCacheStoreError('unsafe_symlink');
            }
            quarantineExists = true;
        } catch (error) {
            if (!isMissing(error)) throw error;
        }

        if (active.exists && quarantineExists) {
            throw new RemoteCacheStoreError('ambiguous_cache_state');
        }
        if (active.exists) {
            await fsp.rename(active.path, quarantinePath);
            await fsp.chmod(quarantinePath, 0o700);
            await this.#syncDirectory(active.senderPath);
            await this.#syncDirectory(this.quarantineRoot);
            quarantineExists = true;
            await this.#fault('after_quarantine_rename', transaction);
            transaction.cacheQuarantined = true;
            transaction.updatedAt = this.now();
            await this.#writeTransaction(transaction);
        } else if (quarantineExists) {
            transaction.cacheQuarantined = true;
        }

        const now = this.now();
        const tombstone = {
            version: STATE_VERSION,
            key: transaction.key,
            senderEndpointId: transaction.senderEndpointId,
            remoteSessionId: transaction.remoteSessionId,
            generation: transaction.generation,
            teardownId: transaction.teardownId,
            transactionId: transaction.transactionId,
            quarantineName: quarantineExists ? transaction.quarantineName : undefined,
            // This is the logical postcondition consumed by teardown ACKs. It
            // is true for an already-empty cache as well as for an actual move.
            cacheQuarantined: true,
            hadActiveCache: quarantineExists,
            logicalCommitted: true,
            state: 'committed',
            removedFileCount: 0,
            committedAt: now,
            updatedAt: now,
        };
        await this.#writeTombstone(tombstone);
        this.tombstones.set(tombstone.key, tombstone);
        await this.#clearActiveGeneration(transaction).catch(error =>
            this.#emitCleanupError(errorCode(error), transaction));
        transaction.state = 'committed';
        transaction.updatedAt = now;
        try {
            await this.#writeTransaction(transaction);
        } catch (error) {
            // The tombstone is authoritative. Leaving the older intent on disk
            // is safe: startup recovery will pair it with this tombstone.
            this.#emitCleanupError(errorCode(error), transaction);
        }
        this.#emitSafe('cleanupCommitted', this.#publicTombstone(tombstone, false));
        return tombstone;
    }

    async #recordTransactionError(transaction, code) {
        transaction.state = 'cleanup_error';
        transaction.errorCode = code;
        transaction.updatedAt = this.now();
        try {
            await this.#writeTransaction(transaction);
            this.transactions.set(transaction.transactionId, transaction);
        } catch (_) {
            // No raw filesystem error is surfaced or logged from this layer.
        }
        this.#emitCleanupError(code, transaction);
    }

    #scheduleDeletionForTombstone(tombstone) {
        if (!tombstone.quarantineName || tombstone.state === 'deleted') return false;
        const transaction = Array.from(this.transactions.values()).find(candidate =>
            candidate.transactionId === tombstone.transactionId) || {
            version: STATE_VERSION,
            transactionId: tombstone.transactionId,
            key: tombstone.key,
            senderEndpointId: tombstone.senderEndpointId,
            remoteSessionId: tombstone.remoteSessionId,
            generation: tombstone.generation,
            teardownId: tombstone.teardownId,
            quarantineName: tombstone.quarantineName,
            cacheQuarantined: tombstone.hadActiveCache === true,
            state: 'committed',
            createdAt: tombstone.committedAt,
            updatedAt: tombstone.updatedAt,
        };
        this.transactions.set(transaction.transactionId, transaction);
        return this.#scheduleDeletion(transaction, tombstone);
    }

    #scheduleDeletion(transaction, tombstone) {
        if (!transaction.quarantineName) return false;
        if (this.deletionJobs.has(transaction.quarantineName)) return false;
        if (tombstone && tombstone.transactionId === transaction.transactionId
            && tombstone.state === 'deleted') return false;

        let settle;
        const job = new Promise(resolve => { settle = resolve; });
        this.deletionJobs.set(transaction.quarantineName, job);
        const runner = async () => {
            try {
                await this.#performDeletion(transaction);
            } finally {
                this.deletionJobs.delete(transaction.quarantineName);
                settle();
            }
        };
        try {
            this.scheduler(runner);
        } catch (error) {
            void this.#markDeletionFailure(transaction, errorCode(error)).finally(() => {
                this.deletionJobs.delete(transaction.quarantineName);
                settle();
            });
        }
        return true;
    }

    #scheduleOrphanDeletion(quarantineName) {
        if (this.deletionJobs.has(quarantineName)) return false;
        let settle;
        const job = new Promise(resolve => { settle = resolve; });
        this.deletionJobs.set(quarantineName, job);
        const runner = async () => {
            try {
                await this.#assertDirectoryNoSymlink(
                    this.quarantineRoot, 'unsafe_quarantine_root');
                await this.#removeTreeNoFollow(this.#quarantinePath(quarantineName));
            } catch (error) {
                this.#emitCleanupError(errorCode(error));
            } finally {
                this.deletionJobs.delete(quarantineName);
                settle();
            }
        };
        try {
            this.scheduler(runner);
        } catch (error) {
            this.deletionJobs.delete(quarantineName);
            settle();
            this.#emitCleanupError(errorCode(error));
        }
        return true;
    }

    async #performDeletion(transaction) {
        try {
            await this.#fault('before_physical_delete', transaction);
            await this.#assertDirectoryNoSymlink(
                this.quarantineRoot, 'unsafe_quarantine_root');
            const removedFileCount = await this.#removeTreeNoFollow(
                this.#quarantinePath(transaction.quarantineName));
            await this.#exclusive(async () => {
                const current = this.tombstones.get(transaction.key);
                if (current && current.transactionId === transaction.transactionId) {
                    const completed = {
                        ...current,
                        state: 'deleted',
                        errorCode: undefined,
                        removedFileCount,
                        deletedAt: this.now(),
                        updatedAt: this.now(),
                    };
                    await this.#writeTombstone(completed);
                    this.tombstones.set(completed.key, completed);
                    this.#emitSafe('cleanupComplete', this.#publicTombstone(completed, false));
                }
                await this.#removeTransaction(transaction);
            });
        } catch (error) {
            if (isSimulatedCrash(error)) return;
            await this.#markDeletionFailure(transaction, errorCode(error));
        }
    }

    async #markDeletionFailure(transaction, code) {
        await this.#exclusive(async () => {
            transaction.state = 'cleanup_error';
            transaction.errorCode = code;
            transaction.updatedAt = this.now();
            try {
                await this.#writeTransaction(transaction);
                this.transactions.set(transaction.transactionId, transaction);
            } catch (_) {
                // Recovery still sees the previous durable intent.
            }
            const current = this.tombstones.get(transaction.key);
            if (current && current.transactionId === transaction.transactionId) {
                const failed = {
                    ...current,
                    state: 'cleanup_error',
                    errorCode: code,
                    updatedAt: this.now(),
                };
                try {
                    await this.#writeTombstone(failed);
                } catch (_) {
                    // Never replace the useful sanitized error with a path-bearing
                    // filesystem exception.
                }
                this.tombstones.set(failed.key, failed);
            }
            this.#emitCleanupError(code, transaction);
        });
    }

    async #removeTreeNoFollow(candidate) {
        let stat;
        try {
            stat = await fsp.lstat(candidate);
        } catch (error) {
            if (isMissing(error)) return 0;
            throw error;
        }
        if (stat.isSymbolicLink() || !stat.isDirectory()) {
            await fsp.unlink(candidate);
            return 1;
        }

        await fsp.chmod(candidate, 0o700);
        let removed = 0;
        const names = await fsp.readdir(candidate);
        for (const name of names) {
            if (name === '.' || name === '..' || name.includes(path.sep)) {
                throw new RemoteCacheStoreError('unsafe_quarantine_entry');
            }
            removed += await this.#removeTreeNoFollow(path.join(candidate, name));
        }
        await fsp.rmdir(candidate);
        return removed;
    }

    async #loadState() {
        this.tombstones.clear();
        this.activeGenerations.clear();
        this.transactions.clear();
        await this.#loadStateDirectory(this.tombstonesRoot, 'tombstone');
        await this.#loadStateDirectory(this.activeStateRoot, 'active');
        await this.#loadStateDirectory(this.transactionsRoot, 'transaction');
    }

    async #loadStateDirectory(directory, kind) {
        const entries = await fsp.readdir(directory, { withFileTypes: true });
        for (const entry of entries) {
            const candidate = path.join(directory, entry.name);
            if (entry.name.includes('.tmp-')) {
                if (entry.isFile() || entry.isSymbolicLink()) {
                    try { await fsp.unlink(candidate); } catch (_) { /* best effort */ }
                } else {
                    this.startupErrors.push('invalid_state_entry');
                    this.#emitCleanupError('invalid_state_entry');
                }
                continue;
            }
            if (!entry.isFile() || !entry.name.endsWith('.json')) {
                this.startupErrors.push('invalid_state_entry');
                this.#emitCleanupError('invalid_state_entry');
                continue;
            }
            try {
                const record = await this.#readStateFile(candidate);
                if (kind === 'tombstone') {
                    this.#validateTombstone(record);
                    if (entry.name !== `${record.key}.json`) {
                        throw new RemoteCacheStoreError('invalid_tombstone_record');
                    }
                    const current = this.tombstones.get(record.key);
                    if (!current || current.generation < record.generation) {
                        this.tombstones.set(record.key, record);
                    }
                } else if (kind === 'active') {
                    this.#validateActiveGeneration(record);
                    if (entry.name !== `${record.key}.json`) {
                        throw new RemoteCacheStoreError('invalid_active_record');
                    }
                    const current = this.activeGenerations.get(record.key);
                    if (!current || current.generation < record.generation) {
                        this.activeGenerations.set(record.key, record);
                    }
                } else {
                    this.#validateTransaction(record);
                    if (entry.name !== `${record.transactionId}.json`) {
                        throw new RemoteCacheStoreError('invalid_transaction_record');
                    }
                    this.transactions.set(record.transactionId, record);
                }
            } catch (error) {
                const code = error instanceof SyntaxError
                    ? 'corrupt_state_record' : errorCode(error);
                this.startupErrors.push(code);
                this.#emitCleanupError(code);
            }
        }
    }

    async #readStateFile(candidate) {
        const stat = await fsp.lstat(candidate);
        if (stat.isSymbolicLink() || !stat.isFile() || stat.size > MAX_STATE_FILE_BYTES) {
            throw new RemoteCacheStoreError('invalid_state_entry');
        }
        return JSON.parse(await fsp.readFile(candidate, 'utf8'));
    }

    #validateTransaction(record) {
        if (!record || record.version !== STATE_VERSION
            || !OPAQUE_NAME_PATTERN.test(record.transactionId)
            || !OPAQUE_NAME_PATTERN.test(record.quarantineName)
            || !TRANSACTION_STATES.has(record.state)
            || typeof record.cacheQuarantined !== 'boolean'
            || !Number.isFinite(record.createdAt)
            || !Number.isFinite(record.updatedAt)
            || (record.errorCode !== undefined
                && !IDENTIFIER_PATTERN.test(record.errorCode))) {
            throw new RemoteCacheStoreError('invalid_transaction_record');
        }
        const binding = this.#normalizeBinding(record, true, true);
        if (binding.key !== record.key || record.transactionId !== record.quarantineName) {
            throw new RemoteCacheStoreError('invalid_transaction_record');
        }
    }

    #validateActiveGeneration(record) {
        if (!record || record.version !== STATE_VERSION
            || !Number.isFinite(record.updatedAt)) {
            throw new RemoteCacheStoreError('invalid_active_record');
        }
        const binding = this.#normalizeBinding(record, true);
        if (binding.key !== record.key) {
            throw new RemoteCacheStoreError('invalid_active_record');
        }
    }

    #validateTombstone(record) {
        if (!record || record.version !== STATE_VERSION
            || !OPAQUE_NAME_PATTERN.test(record.transactionId)
            || !TOMBSTONE_STATES.has(record.state)
            || record.logicalCommitted !== true
            || record.cacheQuarantined !== true
            || typeof record.hadActiveCache !== 'boolean'
            || !Number.isSafeInteger(record.removedFileCount)
            || record.removedFileCount < 0
            || !Number.isFinite(record.committedAt)
            || !Number.isFinite(record.updatedAt)
            || (record.deletedAt !== undefined && !Number.isFinite(record.deletedAt))
            || (record.errorCode !== undefined
                && !IDENTIFIER_PATTERN.test(record.errorCode))) {
            throw new RemoteCacheStoreError('invalid_tombstone_record');
        }
        if (record.quarantineName !== undefined
            && (!OPAQUE_NAME_PATTERN.test(record.quarantineName)
                || record.quarantineName !== record.transactionId)) {
            throw new RemoteCacheStoreError('invalid_tombstone_record');
        }
        if ((record.quarantineName !== undefined) !== record.hadActiveCache) {
            throw new RemoteCacheStoreError('invalid_tombstone_record');
        }
        const binding = this.#normalizeBinding(record, true, true);
        if (binding.key !== record.key) {
            throw new RemoteCacheStoreError('invalid_tombstone_record');
        }
    }

    async #writeTransaction(transaction) {
        await this.#atomicWriteJson(
            path.join(this.transactionsRoot, `${transaction.transactionId}.json`),
            transaction);
    }

    async #writeActiveGeneration(record) {
        await this.#atomicWriteJson(
            path.join(this.activeStateRoot, `${record.key}.json`), record);
    }

    async #writeTombstone(tombstone) {
        await this.#atomicWriteJson(
            path.join(this.tombstonesRoot, `${tombstone.key}.json`), tombstone);
    }

    async #atomicWriteJson(destination, value) {
        const temporary = `${destination}.tmp-${crypto.randomBytes(8).toString('hex')}`;
        let handle;
        try {
            await this.#assertDirectoryNoSymlink(
                path.dirname(destination), 'unsafe_state_directory');
            handle = await fsp.open(temporary, 'wx', 0o600);
            await handle.writeFile(`${JSON.stringify(value)}\n`, 'utf8');
            await handle.sync();
            await handle.close();
            handle = null;
            await fsp.rename(temporary, destination);
            await this.#syncDirectory(path.dirname(destination));
        } catch (error) {
            if (handle) {
                try { await handle.close(); } catch (_) { /* best effort */ }
            }
            try { await fsp.unlink(temporary); } catch (_) { /* best effort */ }
            throw error;
        }
    }

    async #syncDirectory(directory) {
        let handle;
        try {
            handle = await fsp.open(directory, 'r');
            await handle.sync();
        } catch (error) {
            if (!['EINVAL', 'ENOTSUP', 'EISDIR'].includes(error.code)) throw error;
        } finally {
            if (handle) await handle.close();
        }
    }

    async #removeTransaction(transaction) {
        this.transactions.delete(transaction.transactionId);
        try {
            await this.#assertDirectoryNoSymlink(
                this.transactionsRoot, 'unsafe_state_directory');
            await fsp.unlink(path.join(
                this.transactionsRoot, `${transaction.transactionId}.json`));
            await this.#syncDirectory(this.transactionsRoot);
        } catch (error) {
            if (!isMissing(error)) throw error;
        }
    }

    async #clearActiveGeneration(binding) {
        const current = this.activeGenerations.get(binding.key);
        if (!current || current.generation > binding.generation) return;
        this.activeGenerations.delete(binding.key);
        try {
            await this.#assertDirectoryNoSymlink(
                this.activeStateRoot, 'unsafe_state_directory');
            await fsp.unlink(path.join(this.activeStateRoot, `${binding.key}.json`));
            await this.#syncDirectory(this.activeStateRoot);
        } catch (error) {
            if (!isMissing(error)) throw error;
        }
    }

    #transactionsForKey(key) {
        return Array.from(this.transactions.values())
            .filter(transaction => transaction.key === key)
            .sort((left, right) => left.generation - right.generation);
    }

    #orderedTransactions() {
        return Array.from(this.transactions.values()).sort((left, right) =>
            left.createdAt - right.createdAt || left.generation - right.generation);
    }

    #publicTombstone(tombstone, replay) {
        return {
            ok: true,
            scope: CACHE_SCOPE,
            replay,
            state: tombstone.state,
            logicalCommitted: true,
            senderEndpointId: tombstone.senderEndpointId,
            remoteSessionId: tombstone.remoteSessionId,
            teardownId: tombstone.teardownId,
            cacheQuarantined: tombstone.cacheQuarantined,
            hadActiveCache: tombstone.hadActiveCache === true,
            generation: tombstone.generation,
            committedAt: tombstone.committedAt,
            deletedAt: tombstone.deletedAt,
            removedFileCount: tombstone.removedFileCount || 0,
            deletionPending: tombstone.state !== 'deleted',
            errorCode: tombstone.errorCode,
        };
    }

    #failure(code, extra = {}) {
        return {
            ok: false,
            scope: CACHE_SCOPE,
            state: 'cleanup_error',
            logicalCommitted: false,
            errorCode: code,
            ...extra,
        };
    }

    #emitCleanupError(code, binding) {
        this.#emitSafe('cleanupError', {
            scope: CACHE_SCOPE,
            state: 'cleanup_error',
            errorCode: code,
            generation: binding && binding.generation,
            senderEndpointId: binding && binding.senderEndpointId,
            remoteSessionId: binding && binding.remoteSessionId,
            teardownId: binding && binding.teardownId,
        });
    }

    #emitSafe(eventName, payload) {
        try {
            this.emit(eventName, payload);
        } catch (_) {
            // Storage correctness must not depend on an observer implementation.
        }
    }

    async #fault(point, binding) {
        if (!this.faultInjector) return;
        await this.faultInjector(point, {
            generation: binding.generation,
            senderEndpointId: binding.senderEndpointId,
            remoteSessionId: binding.remoteSessionId,
        });
    }

    #exclusive(operation) {
        const result = this.exclusiveTail.then(operation, operation);
        this.exclusiveTail = result.catch(() => {});
        return result;
    }
}

module.exports = {
    RemoteCacheStore,
    RemoteCacheStoreError,
    STATE_VERSION,
    CACHE_SCOPE,
};
