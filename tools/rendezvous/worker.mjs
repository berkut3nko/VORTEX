// docs/design/rendezvous.md "Worker API and D1 schema": the rendezvous Worker. It swaps the candidate strings of a
// home server (its room) and a joining client; game traffic never passes here. Deployed by the owner with wrangler
// (a copy of wrangler.example.toml), served locally by local.mjs over node:sqlite, tested by test.mjs. The exact replies:
// docs/llm/nat-r1.md card 1 "Wire of the Worker". Replies are text lines; requests are JSON.

const ROOM_RE = /^[0-9a-f]{32}$/, HEX16_RE = /^[0-9a-f]{16}$/;
const ABSENT_AFTER = 120, JOIN_LIFE = 60, JOINS_PER_MINUTE = 4, JOINS_KEPT = 16, MAX_BODY = 2048;
const reply = (status, body) => new Response(body + '\n', { status, headers: { 'content-type': 'text/plain' } });
const err = (status, word, detail) => reply(status, detail === undefined ? `error ${word}` : `error ${word} ${detail}`);
const nowOf = env => (env.NOW ? env.NOW() : Math.floor(Date.now() / 1000));

export function parseIpv4(s) {           // dotted quad, no leading zeros -> uint32, else null
  const p = String(s).split('.'); if (p.length !== 4) return null;
  let v = 0; for (const x of p) { if (!/^(0|[1-9][0-9]{0,2})$/.test(x) || +x > 255) return null; v = v * 256 + +x; }
  return v;
}

export const isPrivate = v => Math.floor(v / 2 ** 24) === 10 || Math.floor(v / 2 ** 20) === 0xac1 || Math.floor(v / 2 ** 16) === 0xc0a8;

export function parseCands(s, kinds) {   // the candidate string -> [{ip, port, kind}] or null
  if (typeof s !== 'string' || s.length > 256) return null;
  if (s === '-') return [];
  const out = [];
  for (const item of s.split(',')) {
    const m = /^([0-9.]+):([1-9][0-9]{0,4})\/([a-z]+)$/.exec(item); if (!m) return null;
    const ip = parseIpv4(m[1]), port = +m[2];
    if (ip === null || port > 65535 || !kinds.includes(m[3])) return null;
    out.push({ ip, port, kind: m[3] });
  }
  return out.length <= 8 ? out : null;
}

async function sha256Hex(hex) {          // SHA-256 over the raw bytes of `hex`, as 64 lower-case hex
  const bytes = new Uint8Array(hex.length / 2);
  for (let i = 0; i < bytes.length; ++i) bytes[i] = parseInt(hex.slice(2 * i, 2 * i + 2), 16);
  const digest = new Uint8Array(await crypto.subtle.digest('SHA-256', bytes));
  return Array.from(digest, b => b.toString(16).padStart(2, '0')).join('');
}

const bearer = r => (/^Bearer ([0-9a-f]{32})$/.exec(r.headers.get('authorization') ?? '') ?? [])[1] ?? null;

async function readJson(r) {             // {value} or {error: Response}
  if (+(r.headers.get('content-length') ?? 0) > MAX_BODY) return { error: err(413, 'too-large') };
  const text = await r.text(); if (text.length > MAX_BODY) return { error: err(413, 'too-large') };
  try { const v = JSON.parse(text); if (v && typeof v === 'object' && !Array.isArray(v)) return { value: v }; } catch {}
  return { error: err(400, 'bad-request', 'json') };
}

const badProtocol = p => !Number.isInteger(p) || p < 1 || p > 65535;
const NATS = ['eim', 'symmetric', 'unknown'];

// POST /v1/host: create or update the room (the key's hash must match, or the room must be absent).
async function host(request, env) {
  const key = bearer(request);
  if (key === null) return err(401, 'no-key');
  const { value: b, error } = await readJson(request);
  if (error) return error;
  if (typeof b.room !== 'string' || !ROOM_RE.test(b.room)) return err(400, 'bad-request', 'room');
  if (parseCands(b.cands, ['lan', 'stun', 'given']) === null) return err(400, 'bad-request', 'cands');
  if (!NATS.includes(b.nat)) return err(400, 'bad-request', 'nat');
  if (typeof b.build !== 'string' || !HEX16_RE.test(b.build)) return err(400, 'bad-request', 'build');
  if (badProtocol(b.protocol)) return err(400, 'bad-request', 'protocol');
  const hash = await sha256Hex(key), now = nowOf(env);
  const row = await env.DB.prepare('SELECT host_key, last_seen FROM rooms WHERE room = ?').bind(b.room).first();
  if (row && row.host_key !== hash && row.last_seen >= now - ABSENT_AFTER) return err(409, 'room-held');
  if (row && row.host_key !== hash) await env.DB.prepare('DELETE FROM joins WHERE room = ?').bind(b.room).run();
  await env.DB.prepare('INSERT INTO rooms (room, host_key, cands, nat, build, protocol, last_seen) VALUES (?, ?, ?, ?, ?, ?, ?) ' +
    'ON CONFLICT (room) DO UPDATE SET host_key = excluded.host_key, cands = excluded.cands, nat = excluded.nat, ' +
    'build = excluded.build, protocol = excluded.protocol, last_seen = excluded.last_seen')
    .bind(b.room, hash, b.cands, b.nat, b.build, b.protocol, now).run();
  return reply(200, 'ok registered');
}

// GET /v1/host/wait?room=..&after=..: the long poll; refreshes last_seen, then returns the joins after `after`.
async function wait(request, env, url) {
  const key = bearer(request);
  if (key === null) return err(401, 'no-key');
  const room = url.searchParams.get('room') ?? '';
  if (!ROOM_RE.test(room)) return err(400, 'bad-request', 'room');
  const after = url.searchParams.get('after') ?? '';
  if (!/^(0|[1-9][0-9]{0,9})$/.test(after)) return err(400, 'bad-request', 'after');
  const hash = await sha256Hex(key), now = nowOf(env);
  const touched = await env.DB.prepare('UPDATE rooms SET last_seen = ? WHERE room = ? AND host_key = ? AND last_seen >= ?')
    .bind(now, room, hash, now - ABSENT_AFTER).run();
  if (touched.meta.changes === 0) return err(404, 'no-room');
  const steps = +(env.WAIT_STEPS ?? 20), stepMs = +(env.WAIT_STEP_MS ?? 1000);
  for (let i = 0; i < steps; ++i) {
    const { results } = await env.DB.prepare('SELECT id, nonce, verified, cands FROM joins WHERE room = ? AND id > ? AND created >= ? ORDER BY id LIMIT 16')
      .bind(room, +after, nowOf(env) - JOIN_LIFE).all();
    if (results.length > 0) return reply(200, results.map(j => `join ${j.id} ${j.nonce} ${j.verified} ${j.cands}`).join('\n'));
    if (i + 1 < steps) await new Promise(r => setTimeout(r, stepMs));
  }
  return reply(200, '');
}

// POST /v1/join: a client's join; returns the server's candidates and the age of its last_seen.
async function join(request, env) {
  const { value: b, error } = await readJson(request);
  if (error) return error;
  if (typeof b.room !== 'string' || !ROOM_RE.test(b.room)) return err(400, 'bad-request', 'room');
  const cands = parseCands(b.cands, ['lan', 'stun']);
  if (cands === null || cands.filter(c => c.kind === 'stun').length > 2 || cands.some(c => c.kind === 'lan' && !isPrivate(c.ip)))
    return err(400, 'bad-request', 'cands');
  if (typeof b.nonce !== 'string' || !HEX16_RE.test(b.nonce) || /^0+$/.test(b.nonce)) return err(400, 'bad-request', 'nonce');
  if (typeof b.build !== 'string' || !HEX16_RE.test(b.build)) return err(400, 'bad-request', 'build');
  if (badProtocol(b.protocol)) return err(400, 'bad-request', 'protocol');
  const now = nowOf(env);
  const room = await env.DB.prepare('SELECT cands, nat, build, protocol, last_seen FROM rooms WHERE room = ?').bind(b.room).first();
  if (!room || room.last_seen < now - ABSENT_AFTER) return err(404, 'no-room');
  if (room.build !== b.build || room.protocol !== b.protocol) return err(409, 'another-build', room.build);
  const caller = request.headers.get('cf-connecting-ip') ?? '', callerIp = parseIpv4(caller);
  let verified = 0;
  if (callerIp !== null) {
    if (cands.some(c => c.kind === 'stun' && c.ip !== callerIp)) return err(403, 'ip-mismatch', caller);
    verified = 1;
  }
  const used = await env.DB.prepare('SELECT 1 AS used FROM joins WHERE room = ? AND nonce = ?').bind(b.room, b.nonce).first();
  if (used) return err(409, 'nonce-used');
  const recent = await env.DB.prepare('SELECT COUNT(*) AS n FROM joins WHERE room = ? AND created >= ?').bind(b.room, now - 60).first();
  if (recent.n >= JOINS_PER_MINUTE) return err(429, 'too-many-joins');
  await env.DB.prepare('DELETE FROM joins WHERE room = ? AND (created < ? OR id NOT IN (SELECT id FROM joins WHERE room = ? ORDER BY id DESC LIMIT ?))')
    .bind(b.room, now - JOIN_LIFE, b.room, JOINS_KEPT - 1).run();
  const { meta } = await env.DB.prepare('INSERT INTO joins (room, cands, nonce, verified, created) VALUES (?, ?, ?, ?, ?)')
    .bind(b.room, b.cands, b.nonce, verified, now).run();
  return reply(200, `server ${meta.last_row_id} ${now - room.last_seen} ${room.nat} ${room.cands} ${verified}`);
}

// DELETE /v1/host: the server's clean exit; removes the room and its joins.
async function remove(request, env) {
  const key = bearer(request);
  if (key === null) return err(401, 'no-key');
  const { value: b, error } = await readJson(request);
  if (error) return error;
  if (typeof b.room !== 'string' || !ROOM_RE.test(b.room)) return err(400, 'bad-request', 'room');
  const hash = await sha256Hex(key);
  const { meta } = await env.DB.prepare('DELETE FROM rooms WHERE room = ? AND host_key = ?').bind(b.room, hash).run();
  if (meta.changes === 0) return err(404, 'no-room');
  await env.DB.prepare('DELETE FROM joins WHERE room = ?').bind(b.room).run();
  return reply(200, 'ok deleted');
}

async function fetch(request, env) {
  try {
    const url = new URL(request.url), path = url.pathname, method = request.method;
    if (path === '/v1/host' && method === 'POST') return await host(request, env);
    if (path === '/v1/host' && method === 'DELETE') return await remove(request, env);
    if (path === '/v1/host/wait' && method === 'GET') return await wait(request, env, url);
    if (path === '/v1/join' && method === 'POST') return await join(request, env);
    return err(404, 'not-found');
  } catch {
    return err(503, 'storage');
  }
}

// The hourly cron (wrangler.example.toml): rooms absent for 120 s and their joins, and joins older than 60 s.
async function scheduled(event, env) {
  const now = nowOf(env), cutoff = now - ABSENT_AFTER;
  await env.DB.prepare('DELETE FROM joins WHERE room IN (SELECT room FROM rooms WHERE last_seen < ?)').bind(cutoff).run();
  await env.DB.prepare('DELETE FROM rooms WHERE last_seen < ?').bind(cutoff).run();
  await env.DB.prepare('DELETE FROM joins WHERE created < ?').bind(now - JOIN_LIFE).run();
}

export default { fetch, scheduled };
