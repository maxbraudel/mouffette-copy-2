'use strict';
const { spawn } = require('node:child_process');
const path = require('node:path');
const configurations = [64, 256, 512].flatMap(rate => ['downlink', 'uplink']
    .flatMap(direction => [1, 32, 256].map(count => [rate, direction, count])));
async function worker() {
    while (configurations.length) {
        const configuration = configurations.shift();
        await new Promise((resolve, reject) => {
            const child = spawn(process.execPath, [path.join(__dirname, 'slow-link-probe.js'), ...configuration.map(String)]);
            let output = '';
            child.stdout.on('data', data => { output += data; });
            child.stderr.on('data', data => { output += data; });
            child.once('error', reject);
            child.once('close', code => {
                if (code !== 0) return reject(new Error(`${configuration.join('/')} failed:\n${output}`));
                const result = output.split('\n').find(line => line.startsWith('AUDIT_RESULT '));
                if (!result) return reject(new Error('Probe returned no verification result'));
                console.log(result); resolve();
            });
        });
    }
}
Promise.all(Array.from({ length: 6 }, worker)).catch(error => { console.error(error); process.exitCode = 1; });
