// Run with: node --test scripts/test_firmware_update.js
const { test } = require('node:test');
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');

const source = fs.readFileSync(`${__dirname}/../src/app_spa.js`, 'utf8');
const helpers = source.slice(source.indexOf('function parseFirmwareVersion('),
    source.indexOf('async function checkFirmwareUpdate('));

function fixture({ product = 'TANGO', title, asset = true, githubStatus = 200 } = {}) {
    const calls = [];
    const model = product.toLowerCase();
    const context = vm.createContext({
        console: { info() {} },
        document: { getElementById: () => ({ textContent: `${product}-v2.0.0-beta.02b` }) },
        async fetchWithRetry(url) {
            calls.push(url);
            if (url === '/api/firmware/check') {
                return { ok: true, json: async () => ({ success: true, source: 'api' }) };
            }
            return {
                ok: githubStatus === 200, status: githubStatus,
                json: async () => ({
                    tag_name: `beta-${model}`, draft: false, prerelease: true,
                    name: title || `${product} v2 beta 02c`,
                    assets: asset ? [{
                        name: 'firmware.bin', state: 'uploaded',
                        browser_download_url: `https://github.com/Boondock-Echo/Echo-Tango/releases/download/beta-${model}/firmware.bin`
                    }] : []
                })
            };
        }
    });
    vm.runInContext(helpers, context);
    return { context, calls };
}

for (const product of ['TANGO', 'ECHO']) {
    test(`GitHub update selects the ${product} release without calling the device API`, async () => {
        const { context, calls } = fixture({ product });
        const result = await context.getFirmwareUpdateInfo();
        assert.equal(result.source, 'github');
        assert.equal(result.upgrade_available, true);
        assert.equal(calls.length, 1);
        assert.ok(calls[0].endsWith(`beta-${product.toLowerCase()}`));
    });
}

for (const title of ['Tango v2 beta 02a', 'Tango v2 beta 02b']) {
    test(`GitHub ${title} does not offer an older or equal update`, async () => {
        const { context, calls } = fixture({ title });
        assert.equal((await context.getFirmwareUpdateInfo()).upgrade_available, false);
        assert.equal(calls.length, 1);
    });
}

for (const options of [
    { githubStatus: 403 }, { githubStatus: 404 }, { githubStatus: 500 },
    { asset: false }, { title: 'Unknown version' }, { title: 'Echo v2 beta 02c' }
]) {
    test(`Unusable GitHub release falls back: ${JSON.stringify(options)}`, async () => {
        const { context, calls } = fixture(options);
        assert.equal((await context.getFirmwareUpdateInfo()).source, 'api');
        assert.equal(calls[1], '/api/firmware/check');
    });
}

test('Network rejection falls back to the device API', async () => {
    const { context } = fixture();
    const fetch = context.fetchWithRetry;
    context.fetchWithRetry = url => url.startsWith('https:')
        ? Promise.reject(new Error('Failed to fetch')) : fetch(url);
    assert.equal((await context.getFirmwareUpdateInfo()).source, 'api');
});

test('Version comparison handles numeric components, prereleases, and stable versions', () => {
    const { context } = fixture();
    for (const [next, current] of [
        ['v2.0.0-beta.10', 'v2.0.0-beta.9'],
        ['v2.0.0-beta.02c', 'v2.0.0-beta.02b'],
        ['v2.0.0-rc.1', 'v2.0.0-beta.10'],
        ['v2.0.0', 'v2.0.0-rc.1'],
        ['v2.1.0', 'v2.0.9']
    ]) {
        assert.equal(context.isNewerFirmware(next, current), true);
        assert.equal(context.isNewerFirmware(current, next), false);
    }
    assert.throws(() => context.parseFirmwareVersion('latest-tango'));
});
