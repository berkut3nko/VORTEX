// docs/design/rendezvous.md "Tests": the real worker.mjs served over node:http on 127.0.0.1, with a D1 stand-in on
// node:sqlite. For test.mjs (makeD1, makeEnv) and gate G20 (the program). No package, no dependency.
//   node local.mjs --port N [--db FILE] [--caller-ip A.B.C.D] [--server-box 127.0.0.1:C] [--client-box 127.0.0.1:C]
//                  [--wait-steps N] [--wait-step-ms N] [--parent-pid PID]
// --server-box / --client-box (G20 only, docs/llm/nat-r1.md decision 13): each wait line / the join line gets one more
// field, the route: the peer's candidates as aliases in the asker's vortex_natbox. Real Workers never send a route.
import { DatabaseSync } from 'node:sqlite';
import { readFileSync } from 'node:fs';
import { createServer } from 'node:http';
import { createSocket } from 'node:dgram';
import { fileURLToPath, pathToFileURL } from 'node:url';
import { dirname, join } from 'node:path';
import worker from './worker.mjs';

const here = dirname(fileURLToPath(import.meta.url));

/// D1's prepare / bind / first / all / run over node:sqlite; `queries` counts the statements run.
export function makeD1(path = ':memory:') {
  const db = new DatabaseSync(path);
  db.exec(readFileSync(join(here, 'schema.sql'), 'utf8'));
  const d1 = {
    queries: 0,
    prepare(sql) {
      const statement = db.prepare(sql);
      let args = [];
      const bound = {
        bind(...values) { args = values; return bound; },
        async first() { ++d1.queries; return statement.get(...args) ?? null; },
        async all() { ++d1.queries; return { results: statement.all(...args), success: true }; },
        async run() {
          ++d1.queries;
          const r = statement.run(...args);
          return { success: true, meta: { changes: Number(r.changes), last_row_id: Number(r.lastInsertRowid) } };
        },
      };
      return bound;
    },
  };
  return d1;
}

export function makeEnv({ db, waitSteps, waitStepMs, now } = {}) {
  return { DB: makeD1(db), WAIT_STEPS: waitSteps, WAIT_STEP_MS: waitStepMs, NOW: now };
}

function usage() {
  console.error('usage: node local.mjs --port N [--db FILE] [--caller-ip A.B.C.D] [--server-box 127.0.0.1:C] ' +
                '[--client-box 127.0.0.1:C] [--wait-steps N] [--wait-step-ms N] [--parent-pid PID]');
  process.exit(2);
}

function parseArgs(argv) {
  const o = {};
  const flags = ['--port', '--db', '--caller-ip', '--server-box', '--client-box', '--wait-steps', '--wait-step-ms', '--parent-pid'];
  for (let i = 0; i < argv.length; ++i) {
    let a = argv[i], v;
    const eq = a.indexOf('=');
    if (eq > 0) { v = a.slice(eq + 1); a = a.slice(0, eq); } else { v = argv[++i]; }
    if (!flags.includes(a) || v === undefined) usage();
    o[a.slice(2)] = v;
  }
  const int = (s, lo, hi) => (/^[0-9]+$/.test(s) && +s >= lo && +s <= hi ? +s : usage());
  const box = s => {
    const m = /^127\.0\.0\.1:([0-9]+)$/.exec(s ?? '');
    return m && +m[1] >= 1 && +m[1] <= 65535 ? { host: '127.0.0.1', port: +m[1] } : usage();
  };
  if (o.port === undefined) usage();
  return {
    port: int(o.port, 1024, 65535),
    db: o.db,
    callerIp: o['caller-ip'] === undefined ? undefined
      : (/^[0-9]{1,3}(\.[0-9]{1,3}){3}$/.test(o['caller-ip']) ? o['caller-ip'] : usage()),
    serverBox: o['server-box'] === undefined ? undefined : box(o['server-box']),
    clientBox: o['client-box'] === undefined ? undefined : box(o['client-box']),
    waitSteps: o['wait-steps'] === undefined ? undefined : int(o['wait-steps'], 1, 100),
    waitStepMs: o['wait-step-ms'] === undefined ? undefined : int(o['wait-step-ms'], 1, 60000),
    parentPid: o['parent-pid'] === undefined ? undefined : int(o['parent-pid'], 1, 2 ** 31),
  };
}

// Asks a vortex_natbox for its alias of one endpoint: "alias ip:port" -> "alias ip:port 127.0.0.1:Q" (1 s limit).
function alias(box, endpoint) {
  return new Promise((resolve, reject) => {
    const socket = createSocket('udp4');
    const timer = setTimeout(() => { socket.close(); reject(new Error('box timeout')); }, 1000);
    socket.on('message', msg => {
      const m = /^alias (\S+) (127\.0\.0\.1:[0-9]+)$/.exec(msg.toString().trim());
      if (!m || m[1] !== endpoint) return;
      clearTimeout(timer); socket.close(); resolve(m[2]);
    });
    socket.on('error', e => { clearTimeout(timer); socket.close(); reject(e); });
    socket.bind(0, '127.0.0.1', () => socket.send(`alias ${endpoint}`, box.port, box.host));
  });
}

async function route(box, cands) {
  if (cands === '-') return '-';
  const out = [];
  for (const item of cands.split(',')) {
    const slash = item.lastIndexOf('/');
    out.push(`${await alias(box, item.slice(0, slash))}${item.slice(slash)}`);
  }
  return out.join(',');
}

// With the boxes: each wait line gets the route of its join's cands through the server's box, the join line the
// route of the server's cands through the client's box.
async function addRoutes(options, method, path, body) {
  const lines = body.replace(/\n$/, '').split('\n');
  if (method === 'GET' && path === '/v1/host/wait' && options.serverBox) {
    for (let i = 0; i < lines.length; ++i) {
      const f = lines[i].split(' ');
      if (f[0] === 'join' && f.length === 5) lines[i] += ` ${await route(options.serverBox, f[4])}`;
    }
  } else if (method === 'POST' && path === '/v1/join' && options.clientBox) {
    const f = lines[0].split(' ');
    if (f[0] === 'server' && f.length === 6) lines[0] += ` ${await route(options.clientBox, f[4])}`;
  }
  return lines.join('\n') + '\n';
}

async function serve(options) {
  const env = makeEnv({ db: options.db, waitSteps: options.waitSteps, waitStepMs: options.waitStepMs });
  const server = createServer((req, res) => {
    const chunks = [];
    req.on('data', c => chunks.push(c));
    req.on('end', async () => {
      const path = new URL(req.url, 'http://127.0.0.1').pathname;
      let status = 503, body = 'error storage\n';
      try {
        const headers = new Headers();
        for (const [k, v] of Object.entries(req.headers)) if (typeof v === 'string') headers.set(k, v);
        headers.set('cf-connecting-ip', options.callerIp ?? String(req.socket.remoteAddress ?? '').replace(/^::ffff:/, ''));
        const init = { method: req.method, headers };
        if (req.method !== 'GET' && req.method !== 'HEAD') init.body = Buffer.concat(chunks);
        const response = await worker.fetch(new Request(`http://127.0.0.1:${options.port}${req.url}`, init), env);
        status = response.status;
        body = await response.text();
        if (status === 200) {
          try { body = await addRoutes(options, req.method, path, body); } catch { status = 503; body = 'error storage box\n'; }
        }
      } catch { status = 503; body = 'error storage\n'; }
      console.log(`Rendezvous (local): ${req.method} ${path} -> ${status}`);
      res.writeHead(status, { 'content-type': 'text/plain' });
      res.end(body);
    });
  });
  server.listen(options.port, '127.0.0.1', () => console.log(`Rendezvous (local): listening on http://127.0.0.1:${options.port}`));
  if (options.parentPid !== undefined) {
    setInterval(() => {
      try { process.kill(options.parentPid, 0); } catch { process.exit(0); }
    }, 1000).unref();
  }
  for (const s of ['SIGTERM', 'SIGINT']) process.on(s, () => process.exit(0));
}

if (process.argv[1] && import.meta.url === pathToFileURL(process.argv[1]).href) serve(parseArgs(process.argv.slice(2)));
