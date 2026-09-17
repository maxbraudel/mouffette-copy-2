'use strict';

const assert = require('node:assert/strict');
const { ProtocolMetrics } = require('./protocol_metrics');

const records = [];
const metrics = new ProtocolMetrics({ logger: line => records.push(JSON.parse(line)) });
metrics.incrementOnce('remote_session_lease_expired_total', 'session-a');
metrics.incrementOnce('remote_session_lease_expired_total', 'session-a');
metrics.incrementOnce('remote_session_lease_expired_total', 'session-b');
metrics.setGauge('remote_session_cleanup_pending', 2, 'session-b');
metrics.increment('remote_cache_quarantined_bytes_total', 4096, 'teardown-a');
metrics.incrementOnce('scene_start_skew_rejected_total', 'run-a');

assert.equal(metrics.value('remote_session_lease_expired_total'), 2);
assert.equal(metrics.value('remote_session_cleanup_pending'), 2);
assert.equal(metrics.value('remote_cache_quarantined_bytes_total'), 4096);
assert.equal(metrics.value('scene_start_skew_rejected_total'), 1);
assert.equal(records.every(record => record.event === 'protocol_metric'), true);
assert.throws(() => metrics.increment('private_key', 1), /unknown_metric/);
const bounded = new ProtocolMetrics({ logger: () => {}, maximumOnceKeys: 2 });
for (let index = 0; index < 100; ++index) {
    bounded.incrementOnce('remote_session_lease_expired_total', `bounded-${index}`);
}
assert.equal(bounded.onceKeys.size, 2);
console.log('protocol metrics tests passed');
