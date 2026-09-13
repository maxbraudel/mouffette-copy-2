'use strict';

const contract = require('./media_format_contract.json');

function validateExtensionList(name, values) {
    if (!Array.isArray(values) || values.length === 0) {
        throw new Error(`Media format contract ${name} must be a non-empty array`);
    }
    const normalized = values.map(value => {
        if (typeof value !== 'string' || !/^[a-z0-9]{1,16}$/.test(value)) {
            throw new Error(`Media format contract ${name} contains an invalid extension`);
        }
        return value;
    });
    if (new Set(normalized).size !== normalized.length) {
        throw new Error(`Media format contract ${name} contains duplicates`);
    }
    return Object.freeze(normalized);
}

if (contract.schemaVersion !== 1) {
    throw new Error('Unsupported media format contract schema');
}

const imageExtensions = validateExtensionList(
    'imageExtensions', contract.imageExtensions);
const videoExtensions = validateExtensionList(
    'videoExtensions', contract.videoExtensions);
const allExtensions = [...imageExtensions, ...videoExtensions];
if (new Set(allExtensions).size !== allExtensions.length) {
    throw new Error('Media format contract extension groups overlap');
}
const allowedMediaExtensions = new Set(allExtensions);

function isAllowedMediaExtension(value) {
    return typeof value === 'string'
        && allowedMediaExtensions.has(value.toLowerCase());
}

module.exports = Object.freeze({
    imageExtensions,
    videoExtensions,
    isAllowedMediaExtension,
});
