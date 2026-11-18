# Spread history: text replaced or changed by the WP3 rewrite

Moved verbatim from `docs/design/spread.md` lines 1-21, 580-603 and 637-656 at engine `430fde0` on 2026-11-18 (WP3
document rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

The original text of every section of the current document that the rewrite replaced or changed, in source order. The
current document holds the rewritten version.

---

# Spread: fire that climbs and liquid that runs down, in lockstep, with no randomness

Status: **design, reviewed by two critics (2026-11-17) and amended; nothing built or measured for it.** Owner's request
(2026-11-17), for the magic system (ROADMAP M5, setting.md's "Kindling"): a deterministic spreading system. Fire
spreads up and sideways, never down; a liquid spell spreads the other way, down and sideways, never up. Written
read-only against engine `e21a84d` / game `ad0fe69` (networking stages 0-7b committed, stage 8 being built in the same
tree, so `Server.cpp` / `Sync.cpp` line numbers may move). Numbers are marked **(m)** measured (quoted), **(c)**
computed from sizes, or **(e)** estimate. Every (e) names the measurement that replaces it. The critics' findings and
what was done with each are in "Review of the design" at the end.

Relation to other designs:
- [shred.md](shred.md) Part 2 (R1-R7; this design adds one exception to R1 and amends R2's ChunkClip wording, see
  "Contract changes"), Part 4b (the load check), the SI plan (site checks and the structure credit).
- [networking.md](networking.md) stages 3-8 (op stream, cast credit, chunk sync, HashCheck, terrain mode, stage 8's
  `OpRing::CoversChunk`).
- [fx-budget.md](fx-budget.md) (FxDirector, the 4 ms effects cap).
- [fog.md](fog.md) (the dust grid that smoke feeds).
- [worldgen.md](worldgen.md) (W4's appended ids; C7's 1-voxel party gaps, which fire now jumps).
- The game's `docs/world/setting.md`: conjured matter does not last (Knitting); burned timber quarters are the
  setting's ruins.

<!-- source lines 580-603 -->

### Wire format (`kProtocolVersion` takes the next number after stage 8's; 11 if stage 8 leaves it at 10)

| Kind | Layout | Size |
|---|---|---|
| 7 `IgniteSphere` | as CarveSphere: kind u8, centre i32 × 3 (1/16 voxel), radius u16, power u16 = 0 | 17 B |
| 9 `DouseSphere` | the same | 17 B |
| 8 `SpreadStep` (one part) | kind u8; step varint; box origin chunk zigzag-varint i32 × 3 (x, y, z); box extent varint × 3 (each >= 1); chunkCount varint; per chunk, in (y, z, x) order: index delta varint, overrideCount varint, then per override: index delta varint, id u8 | see below |

- **Chunk index** = the chunk's linear index in (y, z, x) order inside the part's box; the delta is from the previous
  chunk's index + 1 (the first from 0), so chunks are strictly increasing by construction. Coordinates are i32, as
  every other chunk coordinate on the wire (within ±`kMaxSyncChunkCoordinate`).
- **Override index** = `x | y << 5 | z << 10` (local); the delta is from the previous override's index + 1. Ascending
  index is EditRegion's (z, y, x) voxel order.
- Each part is **self-contained** (no delta against an earlier op), because ring replay into one chunk needs each op to
  stand alone.
- **Size (c from e counts):** one house, about 12 chunks and 60 overrides: header about 14 B, chunks about 2-3 B each,
  overrides 2-3 B each, about 190 B per step (the i16 layout of the first draft was 283 B).
- **Decoder limits:**
  - at most 512 chunks per part; box extent product at most 2^22; box inside ±`kMaxSyncChunkCoordinate`;
  - chunk indices inside the box; override indices < 32 768;
  - ids < the palette's count;
  - at most 16 384 overrides per part, charged against a budget like `maskVoxelBudget`.
- **Other wire values:** `PlayerInput::spell`, `CastRejection` 4-5, `SpreadTableHash` in `buildHash`.
- Event kinds 1-6 and 32-36 are unchanged.

<!-- source lines 637-656 -->

### ChunkSync and the ring on clients (membership, never the box)

`ChunkSync` and `OpRing` handle ops by their chunk box (`OpChunkRange`) today. A `SpreadStep` listing two fires at
opposite ends of arena250 has a box of about 69 × rows × 69 chunks, so every box-based path must test **list
membership** for kind 8:
- `OpRing::Entry` keeps the part's sorted chunk list (or a view into its decoded form) next to `lo / hi`.
- `OpRing::Push`'s drop tracking (stage 8: `m_DroppedTouch`, `m_DroppedUnknown`) records each listed chunk in
  `m_DroppedTouch`, and **never** sets `m_DroppedUnknown` for kind 8. Otherwise a box over 4096 chunks would make
  `CoversChunk` fail for every chunk with an older base: stage 8's "every pristine chunk still loading was asked for
  whole" failure. (`m_DroppedTouch` grows by distinct chunks only, bounded by the region; stage 8 owns its pruning.)
- `Walk` / `needs` (CatchUp) and `ForEachAfter` test membership; `TouchesNear` tests membership of the chunk or one of
  its 26 neighbours, so a map-wide box never makes a terrain-mode hidden chunk install as Content.
- `ChunkSync::Apply` iterates the list, not `ForEachChunk(lo, hi)`, for `passes`, `blocked`, the clipped path and the
  deferred-check drop.
- `OpTouchesChunk` = binary search in the list.
- A `SpreadStep` always takes the per-chunk path. A listed chunk that is current with base < seq steps; the others are
  skipped and counted as today.

This lands in S2 on top of whatever stage 8 commits (its names may move).
