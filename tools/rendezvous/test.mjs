// docs/design/rendezvous.md "Tests": the Worker (worker.mjs) over the node:sqlite stand-in of local.mjs.
// Run: node --test engine/tools/rendezvous/test.mjs (also the ctest Tools.RendezvousWorker).
import { test } from 'node:test';
import assert from 'node:assert/strict';
import worker, { parseIpv4, isPrivate, parseCands } from './worker.mjs';
import { makeEnv } from './local.mjs';

// The token 00 01 .. 0f (docs/llm/nat-r1.md decision 4).
const ROOM = '94c0f469fd9e5d1ce662cc248d17ff7a';
const KEY = 'f7282e40f4707117ccb8083adbd5a27a';
const KEY_HASH = 'a93aa346bd1ff4bdc6b3779969104681545f24951254e477260694aec1a2551d';
const OTHER_KEY = '0123456789abcdef0123456789abcdef';
const BUILD = '00000000000000ab';
const BASE = 'http://rv.test';
const CALLER = '203.0.113.20';

function setup(options = {}) {
  const t = { clock: 1_000_000 };
  t.env = makeEnv({ waitSteps: 3, waitStepMs: 10, now: () => t.clock, ...options });
  t.call = async (method, path, { body, key, caller = CALLER, raw } = {}) => {
    const headers = { 'cf-connecting-ip': caller };
    if (key) headers.authorization = `Bearer ${key}`;
    const init = { method, headers };
    if (raw !== undefined) init.body = raw;
    else if (body !== undefined) init.body = JSON.stringify(body);
    const r = await worker.fetch(new Request(BASE + path, init), t.env);
    return { status: r.status, body: await r.text() };
  };
  t.host = (cands = '203.0.113.10:5000/stun', key = KEY, extra = {}) =>
    t.call('POST', '/v1/host', { key, body: { room: ROOM, cands, nat: 'eim', build: BUILD, protocol: 10, ...extra } });
  t.join = (nonce = '00000000000000aa', cands = `${CALLER}:6000/stun`, extra = {}, caller = CALLER) =>
    t.call('POST', '/v1/join', { caller, body: { room: ROOM, cands, nonce, build: BUILD, protocol: 10, ...extra } });
  t.wait = (after = 0, key = KEY) => t.call('GET', `/v1/host/wait?room=${ROOM}&after=${after}`, { key });
  t.sql = sql => t.env.DB.prepare(sql).all().then(r => r.results.map(row => ({ ...row })));
  return t;
}

const nonce = i => i.toString(16).padStart(16, '0');

test('helpers parse addresses and candidate strings', () => {
  assert.equal(parseIpv4('192.168.1.2'), 0xc0a80102);
  assert.equal(parseIpv4('01.2.3.4'), null);
  assert.equal(parseIpv4('256.1.1.1'), null);
  assert.ok(isPrivate(parseIpv4('10.1.2.3')) && isPrivate(parseIpv4('172.20.0.1')) && isPrivate(parseIpv4('192.168.0.9')));
  assert.ok(!isPrivate(parseIpv4('8.8.8.8')) && !isPrivate(parseIpv4('172.32.0.1')));
  assert.deepEqual(parseCands('-', ['lan']), []);
  assert.deepEqual(parseCands('1.2.3.4:5/stun', ['stun']), [{ ip: 0x01020304, port: 5, kind: 'stun' }]);
  assert.equal(parseCands('1.2.3.4:0/stun', ['stun']), null);
  assert.equal(parseCands('1.2.3.4:5/given', ['stun']), null);
});

test('a server registers, updates with its key, and another key is refused while the room is live', async () => {
  const t = setup();
  assert.deepEqual(await t.host(), { status: 200, body: 'ok registered\n' });
  assert.deepEqual(await t.host('203.0.113.11:5001/stun,192.168.1.5:5000/lan'), { status: 200, body: 'ok registered\n' });
  assert.deepEqual(await t.sql('SELECT cands FROM rooms'), [{ cands: '203.0.113.11:5001/stun,192.168.1.5:5000/lan' }]);
  assert.deepEqual(await t.host('203.0.113.12:5000/stun', OTHER_KEY), { status: 409, body: 'error room-held\n' });
  assert.deepEqual(await t.sql('SELECT cands FROM rooms'), [{ cands: '203.0.113.11:5001/stun,192.168.1.5:5000/lan' }]);
});

test('the stored key is the SHA-256 of its 16 bytes', async () => {
  const t = setup();
  assert.equal((await t.host()).status, 200);
  assert.deepEqual(await t.sql('SELECT host_key FROM rooms'), [{ host_key: KEY_HASH }]);
});

test('a room not seen for 120 s is absent', async () => {
  const t = setup();
  assert.equal((await t.host()).status, 200);
  assert.equal((await t.join()).status, 200);
  t.clock += 121;
  assert.deepEqual(await t.join(nonce(2)), { status: 404, body: 'error no-room\n' });
  assert.deepEqual(await t.wait(), { status: 404, body: 'error no-room\n' });
  assert.deepEqual(await t.host('203.0.113.12:5000/stun', OTHER_KEY), { status: 200, body: 'ok registered\n' });
  assert.deepEqual(await t.sql('SELECT COUNT(*) AS n FROM joins'), [{ n: 0 }]);
});

test('wait returns new joins at once, else an empty body, with at most 21 queries', async () => {
  const t = setup({ waitSteps: 20, waitStepMs: 1 });
  assert.equal((await t.host()).status, 200);
  assert.equal((await t.join('00000000000000aa', `${CALLER}:6000/stun`)).status, 200);
  assert.deepEqual(await t.wait(0), { status: 200, body: `join 1 00000000000000aa 1 ${CALLER}:6000/stun\n` });
  const before = t.env.DB.queries;
  assert.deepEqual(await t.wait(1), { status: 200, body: '\n' });
  assert.ok(t.env.DB.queries - before <= 21, `${t.env.DB.queries - before} queries`);
});

test('join: 404, 409 another build, 409 reused nonce, 429 after 4 in a minute', async () => {
  const t = setup();
  assert.deepEqual(await t.join(), { status: 404, body: 'error no-room\n' });
  assert.equal((await t.host()).status, 200);
  assert.deepEqual(await t.join(nonce(1), undefined, { build: '00000000000000cd' }), { status: 409, body: 'error another-build 00000000000000ab\n' });
  assert.deepEqual(await t.join(nonce(1), undefined, { protocol: 9 }), { status: 409, body: 'error another-build 00000000000000ab\n' });
  assert.deepEqual(await t.join(nonce(1)), { status: 200, body: 'server 1 0 eim 203.0.113.10:5000/stun 1\n' });
  assert.deepEqual(await t.join(nonce(1)), { status: 409, body: 'error nonce-used\n' });
  t.clock += 5;
  for (let i = 2; i <= 4; ++i) assert.equal((await t.join(nonce(i))).status, 200);
  assert.deepEqual(await t.join(nonce(5)), { status: 429, body: 'error too-many-joins\n' });
  t.clock += 61;
  assert.equal((await t.host()).status, 200);   // keeps the room live
  const r = await t.join(nonce(6));
  assert.equal(r.status, 200);
  assert.match(r.body, /^server 5 0 eim 203\.0\.113\.10:5000\/stun 1\n$/);
});

test('join: the reply names the age of the server\'s last_seen', async () => {
  const t = setup();
  assert.equal((await t.host()).status, 200);
  t.clock += 40;
  assert.deepEqual(await t.join(), { status: 200, body: 'server 1 40 eim 203.0.113.10:5000/stun 1\n' });
});

test('join: 403 names the caller when a stun candidate is another IPv4', async () => {
  const t = setup();
  assert.equal((await t.host()).status, 200);
  assert.deepEqual(await t.join(nonce(1), '203.0.113.20:6000/stun', {}, '198.51.100.7'),
                   { status: 403, body: 'error ip-mismatch 198.51.100.7\n' });
});

test('join: an IPv6 caller is stored unverified', async () => {
  const t = setup();
  assert.equal((await t.host()).status, 200);
  const r = await t.join(nonce(1), '203.0.113.20:6000/stun', {}, '2001:db8::5');
  assert.equal(r.status, 200);
  assert.match(r.body, / 0\n$/);
  assert.deepEqual(await t.wait(0), { status: 200, body: 'join 1 0000000000000001 0 203.0.113.20:6000/stun\n' });
});

test('join: only lan and at most 2 stun, lan private, the string returned byte for byte', async () => {
  const t = setup();
  assert.equal((await t.host()).status, 200);
  const bad = { status: 400, body: 'error bad-request cands\n' };
  assert.deepEqual(await t.join(nonce(1), `${CALLER}:1/stun,${CALLER}:2/stun,${CALLER}:3/stun`), bad);
  assert.deepEqual(await t.join(nonce(1), `${CALLER}:1/given`), bad);
  assert.deepEqual(await t.join(nonce(1), '8.8.8.8:1/lan'), bad);
  const good = `192.168.1.20:6000/lan,10.0.0.2:6000/lan,${CALLER}:6000/stun,${CALLER}:6001/stun`;
  assert.equal((await t.join(nonce(1), good)).status, 200);
  assert.deepEqual(await t.wait(0), { status: 200, body: `join 1 0000000000000001 1 ${good}\n` });
});

test('validation', async () => {
  const t = setup();
  assert.deepEqual(await t.call('POST', '/v1/join', { raw: 'x'.repeat(2049) }), { status: 413, body: 'error too-large\n' });
  assert.deepEqual(await t.call('POST', '/v1/join', { raw: '{"room":' }), { status: 400, body: 'error bad-request json\n' });
  assert.deepEqual(await t.call('POST', '/v1/join', { raw: '[1]' }), { status: 400, body: 'error bad-request json\n' });
  assert.deepEqual(await t.join(nonce(1), undefined, { room: ROOM.slice(1) }), { status: 400, body: 'error bad-request room\n' });
  assert.deepEqual(await t.join('0000000000000000'), { status: 400, body: 'error bad-request nonce\n' });
  assert.deepEqual(await t.join(nonce(1), undefined, { build: 'xyz' }), { status: 400, body: 'error bad-request build\n' });
  assert.deepEqual(await t.join(nonce(1), undefined, { protocol: 0 }), { status: 400, body: 'error bad-request protocol\n' });
  assert.deepEqual(await t.call('POST', '/v1/host', { body: { room: ROOM } }), { status: 401, body: 'error no-key\n' });
  assert.deepEqual(await t.host(undefined, KEY, { nat: 'cone' }), { status: 400, body: 'error bad-request nat\n' });
  assert.deepEqual(await t.host('1.2.3.4/stun'), { status: 400, body: 'error bad-request cands\n' });
  assert.deepEqual(await t.call('GET', `/v1/host/wait?room=${ROOM}&after=01`, { key: KEY }), { status: 400, body: 'error bad-request after\n' });
  assert.deepEqual(await t.call('GET', '/v1/rooms'), { status: 404, body: 'error not-found\n' });
  assert.deepEqual(await t.call('PUT', '/v1/host', { key: KEY, body: {} }), { status: 404, body: 'error not-found\n' });
});

test('a thrown storage error is a 503', async () => {
  const t = setup();
  t.env.DB.prepare = () => { throw new Error('D1 down'); };
  assert.deepEqual(await t.host(), { status: 503, body: 'error storage\n' });
});

test('delete needs the key and removes the room and its joins', async () => {
  const t = setup();
  assert.equal((await t.host()).status, 200);
  assert.equal((await t.join()).status, 200);
  assert.deepEqual(await t.call('DELETE', '/v1/host', { key: OTHER_KEY, body: { room: ROOM } }), { status: 404, body: 'error no-room\n' });
  assert.deepEqual(await t.call('DELETE', '/v1/host', { key: KEY, body: { room: ROOM } }), { status: 200, body: 'ok deleted\n' });
  assert.deepEqual(await t.join(nonce(2)), { status: 404, body: 'error no-room\n' });
  assert.deepEqual(await t.sql('SELECT COUNT(*) AS n FROM joins'), [{ n: 0 }]);
});

test('a room keeps at most 16 joins and joins older than 60 s are dropped', async () => {
  const t = setup();
  // One join every 16 s (4 a minute, under the cap) for 20 joins: the room is refreshed by the host each time.
  for (let n = 1; n <= 20; ++n) {
    assert.equal((await t.host()).status, 200);
    assert.equal((await t.join(nonce(n))).status, 200);
    const [{ count }] = await t.sql('SELECT COUNT(*) AS count FROM joins');
    assert.ok(count <= 16, `${count} joins`);
    const [{ old }] = await t.sql(`SELECT COUNT(*) AS old FROM joins WHERE created < ${t.clock - 60}`);
    assert.equal(old, 0);
    t.clock += 16;
  }
});

test('the cron removes rooms absent for 120 s', async () => {
  const t = setup();
  assert.equal((await t.host()).status, 200);
  assert.equal((await t.join()).status, 200);
  t.clock += 121;
  await worker.scheduled({}, t.env);
  assert.deepEqual(await t.sql('SELECT COUNT(*) AS n FROM rooms'), [{ n: 0 }]);
  assert.deepEqual(await t.sql('SELECT COUNT(*) AS n FROM joins'), [{ n: 0 }]);
});
