'use strict';

const crypto = require('node:crypto');

const PROTOCOL_VERSION = 8;
const RUNTIME_ID_PATTERN = /^[0-9a-f]{8}-[0-9a-f]{4}-4[0-9a-f]{3}-[89ab][0-9a-f]{3}-[0-9a-f]{12}$/i;
const MAX_INSTANCE_ORDINAL = 2147483647;
const BASE64URL_PATTERN = /^[A-Za-z0-9_-]+$/;

function base64url(buffer) {
    return Buffer.from(buffer).toString('base64url');
}

function decodeBase64url(value) {
    if (typeof value !== 'string' || !BASE64URL_PATTERN.test(value)) return null;
    const decoded = Buffer.from(value, 'base64url');
    return base64url(decoded) === value ? decoded : null;
}

function installationIdForPublicKey(publicKeyDer) {
    return base64url(crypto.createHash('sha256').update(publicKeyDer).digest());
}

function endpointIdForInstallation(installationId, instanceId) {
    return base64url(crypto.createHash('sha256')
        .update(`mouffette-endpoint-v1\n${installationId}\n${instanceId}`, 'utf8')
        .digest());
}

function instanceIdForOrdinal(instanceOrdinal) {
    if (!Number.isInteger(instanceOrdinal) || instanceOrdinal < 1
        || instanceOrdinal > MAX_INSTANCE_ORDINAL) return null;
    return instanceOrdinal === 1 ? 'primary' : `instance-${instanceOrdinal}`;
}

function challengePayload({ serverBootId, nonce, runtimeId, instanceId, instanceOrdinal }) {
    return Buffer.from(
        `mouffette-v${PROTOCOL_VERSION}\n${serverBootId}\n${nonce}\n${runtimeId}\n${instanceId}\n${instanceOrdinal}`,
        'utf8');
}

function createChallenge(serverBootId, now = Date.now()) {
    return Object.freeze({
        protocolVersion: PROTOCOL_VERSION,
        serverBootId,
        nonce: base64url(crypto.randomBytes(32)),
        issuedAt: now,
    });
}

function verifyAuthResponse(challenge, response, now, challengeTtlMs) {
    if (!Number.isSafeInteger(challengeTtlMs) || challengeTtlMs <= 0) {
        throw new TypeError('challengeTtlMs must be a positive integer');
    }
    if (!challenge || !response || response.protocolVersion !== PROTOCOL_VERSION) {
        return { ok: false, error: 'protocol_version_mismatch' };
    }
    if (response.serverBootId !== challenge.serverBootId
        || !Number.isFinite(challenge.issuedAt)
        || now - challenge.issuedAt < 0
        || now - challenge.issuedAt > challengeTtlMs) {
        return { ok: false, error: 'expired_or_mismatched_challenge' };
    }
    if (typeof response.runtimeId !== 'string'
        || !RUNTIME_ID_PATTERN.test(response.runtimeId)) {
        return { ok: false, error: 'invalid_runtime_id' };
    }
    const canonicalInstanceId = instanceIdForOrdinal(response.instanceOrdinal);
    if (!canonicalInstanceId) return { ok: false, error: 'invalid_instance_ordinal' };
    if (response.instanceId !== canonicalInstanceId) {
        return { ok: false, error: 'invalid_instance_id' };
    }
    try {
        const publicKeyDer = decodeBase64url(response.publicKey);
        const signature = decodeBase64url(response.signature);
        if (!publicKeyDer || !signature
            || publicKeyDer.length < 32 || publicKeyDer.length > 256
            || signature.length !== 64) {
            return { ok: false, error: 'invalid_identity_material' };
        }
        const publicKey = crypto.createPublicKey({
            key: publicKeyDer,
            type: 'spki',
            format: 'der',
        });
        if (publicKey.asymmetricKeyType !== 'ed25519') {
            return { ok: false, error: 'identity_key_must_be_ed25519' };
        }
        const valid = crypto.verify(null,
            challengePayload({ ...challenge,
                runtimeId: response.runtimeId,
                instanceId: response.instanceId,
                instanceOrdinal: response.instanceOrdinal }),
            publicKey,
            signature);
        if (!valid) return { ok: false, error: 'invalid_identity_signature' };
        const installationId = installationIdForPublicKey(publicKeyDer);
        if (typeof response.installationId !== 'string'
            || response.installationId !== installationId) {
            return { ok: false, error: 'installation_id_mismatch' };
        }
        const endpointId = endpointIdForInstallation(installationId, response.instanceId);
        return {
            ok: true,
            installationId,
            endpointId,
            instanceId: response.instanceId,
            instanceOrdinal: response.instanceOrdinal,
            runtimeId: response.runtimeId,
            publicKeyDer,
        };
    } catch (_) {
        return { ok: false, error: 'invalid_identity_material' };
    }
}

module.exports = {
    PROTOCOL_VERSION,
    MAX_INSTANCE_ORDINAL,
    instanceIdForOrdinal,
    challengePayload,
    createChallenge,
    installationIdForPublicKey,
    endpointIdForInstallation,
    verifyAuthResponse,
};
