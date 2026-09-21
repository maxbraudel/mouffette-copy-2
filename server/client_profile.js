'use strict';

const crypto = require('node:crypto');

const MAX_PROFILE_PICTURE_BYTES = 128 * 1024;
const MAX_USERNAME_LENGTH = 64;

// Inspect the bounded JPEG marker stream without loading an image codec into
// the relay. Recipients also decode the image before displaying it.
function isProfilePictureJpeg(bytes) {
    if (!Buffer.isBuffer(bytes) || bytes.length < 4
        || bytes.length > MAX_PROFILE_PICTURE_BYTES
        || bytes[0] !== 0xff || bytes[1] !== 0xd8) return false;
    let offset = 2;
    let frame = false;
    let scan = false;
    while (offset < bytes.length) {
        if (bytes[offset++] !== 0xff) return false;
        while (offset < bytes.length && bytes[offset] === 0xff) ++offset;
        if (offset >= bytes.length) return false;
        const marker = bytes[offset++];
        if (marker === 0xd9) return frame && scan && offset === bytes.length;
        if (marker === 0x00 || marker === 0xd8
            || (marker >= 0xd0 && marker <= 0xd7)
            || offset + 2 > bytes.length) return false;
        const length = bytes.readUInt16BE(offset);
        if (length < 2 || offset + length > bytes.length) return false;
        if (marker >= 0xc0 && marker <= 0xcf
            && ![0xc4, 0xc8, 0xcc].includes(marker)) {
            // Baseline, extended sequential and progressive 8-bit JPEG.
            if (![0xc0, 0xc1, 0xc2].includes(marker) || frame || length < 8
                || bytes[offset + 2] !== 8
                || bytes.readUInt16BE(offset + 3) !== 250
                || bytes.readUInt16BE(offset + 5) !== 250
                || ![1, 3].includes(bytes[offset + 7])
                || length !== 8 + 3 * bytes[offset + 7]) return false;
            frame = true;
        }
        if (marker === 0xda) {
            if (!frame || length < 8 || ![1, 2, 3].includes(bytes[offset + 2])
                || length !== 6 + 2 * bytes[offset + 2]) return false;
            scan = true;
            offset += length;
            // Entropy data may contain escaped FF bytes and restart markers.
            while (offset < bytes.length) {
                if (bytes[offset] !== 0xff) { ++offset; continue; }
                const next = bytes[offset + 1];
                if (next === 0x00 || (next >= 0xd0 && next <= 0xd7)) {
                    offset += 2;
                    continue;
                }
                break;
            }
        } else {
            offset += length;
        }
    }
    return false;
}

function normalizeClientProfile(message) {
    const username = message.username === undefined ? '' : message.username;
    const encoded = message.profilePictureJpeg === undefined ? '' : message.profilePictureJpeg;
    if (typeof username !== 'string' || Array.from(username.trim()).length > MAX_USERNAME_LENGTH
        || /[\u0000-\u001f\u007f-\u009f]/u.test(username)) {
        return { error: 'username must be a string of at most 64 characters without control characters' };
    }
    if (typeof encoded !== 'string'
        || encoded.length > Math.ceil(MAX_PROFILE_PICTURE_BYTES / 3) * 4) {
        return { error: 'profilePictureJpeg must be a base64 JPEG of at most 128 KiB' };
    }
    const bytes = Buffer.from(encoded, 'base64');
    if (encoded !== '' && (bytes.toString('base64') !== encoded || !isProfilePictureJpeg(bytes))) {
        return { error: 'profilePictureJpeg must be a canonical base64 250x250 JPEG of at most 128 KiB' };
    }
    return {
        username: username.trim(),
        profilePictureJpeg: encoded,
        profilePictureHash: bytes.length > 0
            ? crypto.createHash('sha256').update(bytes).digest('hex') : '',
    };
}

module.exports = { MAX_PROFILE_PICTURE_BYTES, MAX_USERNAME_LENGTH,
    isProfilePictureJpeg, normalizeClientProfile };
