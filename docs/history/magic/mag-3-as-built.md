# [mag-3] as built (2026-11-26)

What [mag-3] built, and where it differs from [design/magic.md](../../design/magic.md) as it stood before the stage.
There is no task pack: the stage followed [mag-1]'s ([llm/mag-1.md](../../llm/mag-1.md), cards 2 and 5).

## What it builds

- Core: palette ids appended after `knit_iron` (44): `ice` 45, `floe` 46, `knit_iron_rimed` 47, `stone_rimed` 48,
  `stone_dark_rimed` 49, `brick_rimed` 50, `iron_rimed` 51, and `MaterialFlag_Slick` (bit 5). Ice and floe are
  `Conjured | Slick`, floe also `Indestructible` (an anchor); each rimed id has half its base's hardness and its base's
  bonds. The Turning maps Rime (9), Thaw (10), Freeze (11), FreezeWater (12) and Shatter (13) get their rows; Unknit
  (14) gains rimed knit and ice (to air) and floe (to water). `World::CarveCapsuleQ`, `World::Materials()`.
- Sim: `WorldOpKind::CarveCapsule` (9, `WorldOp::Bore`), `sim::CapsuleSite`, the Pierce operator (`sim::Pierce`,
  `kMaxBoreVoxels` 64, `kMaxPierceRadiusVoxels` 4, `kBoreProofMargin` 24 with the design's `static_assert`), the
  bore cost table (`SpellTable::boreCost`, `sim::BoreCostTable`, `kBoreStop`), the Shatter flood (`sim::ShatterFlood`),
  the full Plume operator (`sim::PlumeOperator`) and the Burst disc (`BurstParams::discRadius`, Front and Path roles).
  The registry records the turn layer (every voxel a Rime op changed) and expires it with Thaw.
- Players (`sim::kPlayerRuleRevision` 4): Chill, Root and Release zones, the control guard (`kControlGuardTicks` 90),
  `Fresh` (`sim::StartFreshLife`, 120 ticks; the server's respawn calls it), the statuses' wish cuts (rooted: no wish,
  no jump, no dash; pinned 6/16; chilled `(16 - slowQ) / 16`) and slick ground (`sim::GroundVoxel`,
  `sim::OnSlickGround`).
- Spells: Permafrost Climb (7), Shatter Spike (11), Flash Freeze (15). Punch (14) gains its ally Release and the
  Shatter flood at T_z.
- Wire: kind 9 is decoded with its reserved layout (29 B) and the capsule limits; no protocol bump. A client opens a
  presentation group for a bore.
- Game: `--spell-demo shatter-spike,flash-freeze,permafrost-climb` in a Frost yard south of the pad; the offline hand
  cast offers the three spells (the table says they are built).

## Where it differs from the design

1. **Slick and guard values are build constants**, not `PlayerTuning` fields in `Welcome`: protocol 14 did not reserve
   them, and the stage takes no bump. They are covered by `kPlayerRuleRevision` 4 in `buildHash`. The slope term of
   slick ground (3 m/s^2 downhill over 10 degrees) is not built: the player step has no ground normal.
2. **A rooted player does not slide on ice** (the slick rule is skipped while rooted) and a root ends a running dash.
3. **Shatter Spike's Chill** is a y-cylinder of r 1 m and half height 2 m round the hit, not a sphere: the zone tests
   the feet, and a hit at eye height on a wall would otherwise miss everyone beside it. A direct hit on an enemy in front
   of the terrain (the ray against enemy capsules) deals 25 and bores nothing; a body hit is not special-cased (the
   terrain ray is used).
4. **Permafrost Climb has no ledges** (Rising Spire's ledges and ghosts wait for [mag-4]); its Root and Chill zones
   cover the plume's box grown 0.5 m sideways and 2 m down, so players at the foot of the face are inside.
5. **Flash Freeze's film** covers the disc's Path cells over dry ground (the leading half-rim is Front and is left);
   the floe slab is the surface water voxel and the one under it over a cylinder of 3 m.
6. **Bore depth depends on how the axis meets the grid**: the spike's 2 688 budget bores 1.0 m of stone with the axis
   through voxel centres and up to 1.75 m with it on voxel corners (fewer voxels per slice).
7. **Not built in this stage**: `Movement.IceFilmNoCorrectionAbove2cm` (a server cannot be given an ice ground in a
   net test without a generator hook), the `--siege-matrix` rows for Frost (G23 keeps its four spells and lines), the
   attune menu's Frost entry and every session presentation (sessions' presentation is not in scope). The Heat map
   rows for ice, floe and rime wait for [mag-5].

8. **Flash Freeze's visit cap follows the disc's area** (`16 R^2`, 9 216 at 0.125 m): the design's 6 000 cut a disc of
   R 24 at about 1 500 of its 1 800 cells, since the sweep tests about four cells per popped cell.
9. **The per-player conjured cap skips scripts** (player 0): a script is no player, and the 0.1 m Frost demo's floe
   slab (5 640 voxels) was crumbled by the next cast's room-making.

## Measured

On the Windows PC (not the UHD 620), `--spell-demo shatter-spike,flash-freeze,permafrost-climb`, at 0.125 / 0.1 /
0.15 m:
- Shatter Spike: 16 / 13 / 10 slices into stone, 1 882 / 2 564 / 772 rimed voxels; work 9 228 / 11 000 / 6 644,
  0.7-1.0 ms; 122 B. The combo: the left-click blast carves 1.25 / 1.20 / 1.20 m into plain stone and 3.00 / 2.30 /
  2.40 m into the spiked block.
- Flash Freeze: a floe slab of 3 584 / 5 640 / 2 514 voxels on the moat, 0 islands; work 8 872 / 13 149 / 6 938,
  0.4-0.8 ms; 115-138 B.
- Permafrost Climb: 343 / 549 / 205 ice voxels up a wall (a strip 7 columns wide, 6 m tall) and 728 / 1 170 / 430 rimed
  voxels; work 1 833 / 3 859 / 1 258, 0.4-1.8 ms; 150-177 B.
- Tests (`Propagate.PierceStopsAtBedrockAndWhenTheBudgetIsSpent`): the spike bores 14 voxels of stone and 22 of rimed
  stone with its axis on voxel corners, none of iron or bedrock; `Combo.ShatterSpikeDeepensABlast` 1.25 m against
  2.25 m.
