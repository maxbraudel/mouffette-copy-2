'use strict';

const assert = require('node:assert/strict');
const {
    imageExtensions,
    videoExtensions,
    isAllowedMediaExtension,
} = require('./media_format_contract');

assert(imageExtensions.includes('webp'));
assert(videoExtensions.includes('mp4'));
assert.equal(isAllowedMediaExtension('webp'), true);
assert.equal(isAllowedMediaExtension('WeBp'), true);
assert.equal(isAllowedMediaExtension('.webp'), false);
assert.equal(isAllowedMediaExtension('webm'), false);

console.log('media format contract tests passed');
