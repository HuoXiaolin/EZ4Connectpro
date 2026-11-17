const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const path = require('node:path');
const script = fs.readFileSync(path.join(__dirname, '../../resource/browser_extension/cleanup.js'), 'utf8');

async function run(kind) {
    let handler;
    const base = 'http://127.0.0.1:12345/ez4connectpro/' + 'a'.repeat(32) + '/return';
    const storage = {'portal-7': {returned: base, gateway: 'https://vpn.example', expires: Date.now() + 10000}};
    const written = [];
    let requests = 0;
    const noop = {addListener() {}};
    const chrome = {
        runtime: {onMessage: {addListener(fn) { handler = fn; }}},
        storage: {session: {async get(key) { return {[key]: storage[key]}; }, async remove(key) { delete storage[key]; }}},
        tabs: {onRemoved: noop, onUpdated: noop},
        alarms: {onAlarm: noop, clear() {}},
        declarativeNetRequest: {async updateSessionRules() {}},
        cookies: {async set(value) { written.push(value); }}
    };
    const fetch = async (url, options) => {
        ++requests;
        assert.equal(url, base.replace('/return', '/resources'));
        assert.equal(options.credentials, 'omit');
        assert.equal(options.redirect, 'error');
        if (requests === 1) return {status: 202};
        return {status: 200, ok: true, async json() {
            return {url: kind === 'foreign-url' ? 'https://other.example/portal/' : 'https://vpn.example/portal/',
                cookies: [{url: kind === 'foreign-cookie' ? 'https://other.example/' : 'https://vpn.example/',
                    name: 'sid', value: 'fixture-only'}]};
        }};
    };
    const sandbox = {chrome, URL, Date, AbortSignal, fetch, setTimeout: fn => setTimeout(fn, 1)};
    vm.createContext(sandbox);
    vm.runInContext(script, sandbox);
    const sender = {url: base, frameId: kind === 'subframe' ? 1 : 0, tab: {id: kind === 'foreign-tab' ? 8 : 7}};
    const result = await new Promise(resolve => handler({type: 'sso-resources'}, sender, resolve));
    if (kind === 'valid') {
        assert.equal(result.url, 'https://vpn.example/portal/');
        assert.equal(written.length, 1);
        assert.equal(written[0].httpOnly, true);
        assert.equal(written[0].secure, true);
        assert.equal(written[0].url, 'https://vpn.example/');
        assert.equal(storage['portal-7'], undefined);
    } else {
        assert.ok(!result || result.error);
        assert.equal(written.length, 0);
    }
    if (kind === 'subframe' || kind === 'foreign-tab') assert.equal(requests, 0);
}

async function authorizeResources(valid) {
    const nonce = 'b'.repeat(32);
    const begin = 'http://127.0.0.1:12345/ez4connectpro/' + nonce + '/begin';
    const storage = {[nonce]: {begin, tabId: 7}};
    let destination, addedRules = 0;
    const controls = {status: {}, allow: {addEventListener() {}}};
    const sandbox = {URL, Date, AbortSignal,
        document: {getElementById(id) { return controls[id]; }},
        history: {replaceState() {}},
        location: {hash: '#' + nonce, pathname: '/authorize.html', replace(url) { destination = url; }},
        fetch: async (url, opts) => {
            assert.equal(url, begin.replace('/begin', '/metadata'));
            assert.equal(opts.credentials, 'omit'); assert.equal(opts.redirect, 'error');
            return {ok: true, json: async () => ({resourceOnly: true, gateway: 'https://vpn.example',
                login: valid ? 'https://vpn.example/portal/' : 'https://foreign.example/portal/'})};
        },
        chrome: {
            declarativeNetRequest: {async updateSessionRules(rule) { addedRules += rule.addRules?.length || 0; }},
            permissions: {async contains() { return true; }},
            storage: {session: {async get(key) { return {[key]: storage[key]}; }, async remove(key) { delete storage[key]; }, async set(data) { Object.assign(storage, data); }}}
        }
    };
    vm.createContext(sandbox);
    await vm.runInContext(fs.readFileSync(path.join(__dirname, '../../resource/browser_extension/authorize.js'), 'utf8'), sandbox);
    assert.equal(addedRules, 0, 'Resource-only mode must not intercept login tickets');
    if (valid) {
        assert.equal(destination, begin.replace('/begin', '/return'));
        assert.equal(storage['portal-7'].gateway, 'https://vpn.example');
    } else {
        assert.equal(destination, undefined);
        assert.equal(storage['portal-7'], undefined);
    }
}

(async () => {
    for (const kind of ['valid', 'foreign-url', 'foreign-cookie', 'foreign-tab', 'subframe']) await run(kind);
    await authorizeResources(true); await authorizeResources(false);
    console.log('7 resource-extension isolation and standalone-opening scenarios passed');
})().catch(error => { console.error(error); process.exitCode = 1; });
