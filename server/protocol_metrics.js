'use strict';

const METRICS = new Set([
    'remote_session_lease_expired_total',
    'remote_session_resumed_total',
    'scene_prepare_failed_total',
    'scene_clock_uncertainty_rejected_total',
    'scene_start_skew_rejected_total',
    'remote_session_cleanup_pending',
    'remote_cache_quarantined_bytes_total',
    'remote_session_cleanup_error_total',
]);

class ProtocolMetrics {
    constructor(options = {}) {
        this.logger = options.logger === undefined ? console.log : options.logger;
        this.values = new Map();
        this.onceKeys = new Set();
    }

    increment(name, amount = 1, correlationId = '') {
        this.#assertMetric(name);
        if (!Number.isSafeInteger(amount) || amount < 0) throw new Error('invalid_metric_amount');
        const value = (this.values.get(name) || 0) + amount;
        this.values.set(name, value);
        this.#emit(name, value, 'counter', correlationId, amount);
        return value;
    }

    incrementOnce(name, correlationId, amount = 1) {
        const key = `${name}:${String(correlationId || '')}`;
        if (this.onceKeys.has(key)) return this.values.get(name) || 0;
        this.onceKeys.add(key);
        return this.increment(name, amount, correlationId);
    }

    setGauge(name, value, correlationId = '') {
        this.#assertMetric(name);
        if (!Number.isSafeInteger(value) || value < 0) throw new Error('invalid_metric_value');
        this.values.set(name, value);
        this.#emit(name, value, 'gauge', correlationId);
        return value;
    }

    value(name) {
        this.#assertMetric(name);
        return this.values.get(name) || 0;
    }

    snapshot() {
        return Object.fromEntries(Array.from(METRICS, name => [name, this.value(name)]));
    }

    #assertMetric(name) {
        if (!METRICS.has(name)) throw new Error('unknown_metric');
    }

    #emit(name, value, kind, correlationId, delta) {
        if (typeof this.logger !== 'function') return;
        const record = {
            timestamp: new Date().toISOString(),
            event: 'protocol_metric',
            metric: name,
            kind,
            value,
        };
        if (delta !== undefined) record.delta = delta;
        if (correlationId) record.correlationId = String(correlationId).slice(0, 128);
        this.logger(JSON.stringify(record));
    }
}

module.exports = { METRICS, ProtocolMetrics };
