-- docs/design/rendezvous.md "Worker API and D1 schema": the D1 database of the rendezvous Worker (worker.mjs).
-- The owner applies it once: wrangler d1 execute vortex-rendezvous --remote --file schema.sql
CREATE TABLE rooms (
  room       TEXT PRIMARY KEY,      -- 32 hex: SHA-256("vortex-room-v1" || token bytes)[0..16)
  host_key   TEXT NOT NULL,         -- 64 hex: SHA-256 of the host key
  cands      TEXT NOT NULL,         -- the candidate string (`a.b.c.d:port/kind,...` or `-`; rooms: lan, stun, given), returned byte for byte
  nat        TEXT NOT NULL,         -- "eim" | "symmetric" | "unknown"
  build      TEXT NOT NULL,         -- buildHash, 16 hex
  protocol   INTEGER NOT NULL,
  last_seen  INTEGER NOT NULL       -- unix seconds
);
CREATE TABLE joins (
  id         INTEGER PRIMARY KEY AUTOINCREMENT,
  room       TEXT NOT NULL,
  cands      TEXT NOT NULL,         -- the candidate string (`a.b.c.d:port/kind,...` or `-`; joins: lan and at most 2 stun), returned byte for byte
  nonce      TEXT NOT NULL,         -- 16 hex, the client's punch nonce
  verified   INTEGER NOT NULL,      -- 1 when every stun candidate's IP is the caller's IPv4
  created    INTEGER NOT NULL,
  UNIQUE (room, nonce)
);
CREATE INDEX joins_by_room ON joins (room, id);
