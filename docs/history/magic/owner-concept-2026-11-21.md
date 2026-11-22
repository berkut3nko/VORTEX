# The owner's magic concept (2026-11-21), verbatim

The owner sent this concept on 2026-11-21 ("Have some concepts about magic system:"). It is the input to
[../../design/magic.md](../../design/magic.md) and is kept here unchanged.

---

Game Design Document: Voxel Siege Magic System


1. Core Concept

Magic in this siege system is physical and structural rather than purely statistical. Every spell is constructed by combining two components:
1. Cast Type (Form & Propagation): Dictates how the spell travels, interacts with voxel geometry, and spreads upon impact.
2. Element (Matter & Effect): Dictates the physical properties, damage type, and utility applied to voxels and players.

2. Part I: Cast Types (Propagation & Physics)

Cast types determine the trajectory and spatial behavior of the voxel effect upon collision.

Cast Type	Trajectory & Spread Behavior	Siege Tactical Role
1. Liquid (or Flow)	Downward & Pooling. Flows over edges, cascades down castle walls, and fills trenches or moats via gravity.	Flushing defenders off battlements; flooding lower courtyard chokepoints.
2. Flame (or Plume)	Upward & Rising. Rises along vertical surfaces, climbs wall faces, and fills overhead ceilings or under-archways.	Attacking high towers from below; smoking out defenders on elevated walkways.
3. Solid (or Pierce)	Linear Penetration. Fires a concentrated projectile that punches straight through voxel blocks, losing Structural Health with every voxel destroyed until depleted.	Breaching thick castle walls; sniping defenders behind cover.
4. Torrent (or Burst / Gale)	Lateral / Radial. Expands outward sideways upon collision, sweeping across flat floors or wall surfaces in a wide shockwave.	Clearing wide bridges; knocking attackers off siege ladders; sweeping debris.


3. Part II: Elements (Matter & Interaction)

Elements define what the spell is and how it alters the voxel battlefield.

1. Metal (or Ferrum / Iron)

• Core Mechanic: Creation & Reinforcement. Spawns physical voxel blocks or coats existing ones.
• Progression: Leveling up increases the Structural Health (HP) of the spawned voxels, resisting enemy siege weapons and Solid casts.
• Siege Utility: Building instant cover, patching breached walls, or creating offensive ramps.

2. Thermal (or Blaze / Ember)

• Core Mechanic: Damage-over-Time & Ignition. Deals high direct damage on contact, ignites flammable voxel materials (wood, oil, thatch), and heats up Metal voxels to damage nearby players.
• Siege Utility: Area denial, burning down wooden gates/siege towers, and forcing defenders out of tight spaces.

3. Pressure (or Kinetic / Force)

• Core Mechanic: Physics Impulse & Shatter. Applies heavy knockback to players, launches physics objects, and deals bonus Structural Damage to brittle/cracked voxels without leaving residue.
• Siege Utility: Crowd control, launching allies onto walls, and collapsing weakened structures.

4. Frost (or Cryo / Gelid) — Recommended 4th Element

• Why it fits: In a voxel siege game, controlling movement and modifying existing matter completes the tactical loop alongside building (Metal), destroying (Thermal), and moving (Pressure).
• Core Mechanic: Slow, Freeze & Brittleness. Chills players (reducing movement/cast speed), freezes Liquid spells into walkable solid ice, and makes enemy Metal/stone voxels Brittle (taking 2x structural damage from Pressure or Solid casts).
• Siege Utility: Creating temporary ice bridges across moats, locking down choke points, and setting up shatter combos with teammates.
(Alternative 4th Element option if you prefer a darker/chemical theme: Acid / Blight, which slowly dissolves high-HP Metal voxels over time rather than shattering them instantly.)

4. The 16 Spell Combinations (Siege Matrix)

How each Cast Type + Element behaves in multiplayer siege combat:

Metal Combinations (Construction & Defense)

• Liquid + Metal (Molten Pour): Flows down a wall and hardens into a thick, reinforced metal shell—great for rapidly plating a crumbling gate.
• Flame + Metal (Rising Spire): Grows a vertical pillar of metal upward from the ground—used by attackers to build instant scaling towers or block arrow slits.
• Solid + Metal (Rebar Lance): Fires a heavy metal spike that lodges into walls, creating climbable footholds or impaling targets.
• Torrent + Metal (Barricade Wave): Spreads sideways across the ground on impact, instantly erecting a wide, chest-high defensive wall.

Thermal Combinations (Area Denial & Anti-Personnel)

• Liquid + Thermal (Napalm / Lava Flow): Pours burning liquid down battlements and staircases, creating a deadly hazard floor.
• Flame + Thermal (Wildfire): Classic rising inferno that climbs wooden siege towers and roasts defenders standing above the impact point.
• Solid + Thermal (Meltdown Beam): High-velocity superheated bolt that bores straight through reinforced walls while igniting anything inside.
• Torrent + Thermal (Firestorm Sweep): Explodes outward horizontally on impact, clearing an entire bridge or courtyard platform of defenders.

Pressure Combinations (Mobility & Demolition)

• Liquid + Pressure (Crushing Weight): Creates a downward gravity/pressure zone that pins players to the ground and collapses unstable floors beneath them.
• Flame + Pressure (Updraft / Geyser): Creates a rising column of air that launches allies up onto castle walls or juggles enemies into the air.
• Solid + Pressure (Siege Cannon): Pure kinetic penetrator—punches deep holes through stone and metal without burning or leaving blocks behind.
• Torrent + Pressure (Shockwave): Omnidirectional lateral blast that knocks defenders off walls and deflects incoming projectiles.

Frost Combinations (Control & Combo Setup)

• Liquid + Frost (Glacier Slide): Cascades down slopes and freezes into slick ice, letting attackers slide rapidly or causing defenders to lose footing.
• Flame + Frost (Permafrost Climb): Freezes upward along vertical surfaces, encasing ladders, archers, and murder holes in ice.
• Solid + Frost (Shatter Spike): Pierces targets and deeply chills surrounding voxels, priming a wall section to be shattered by a teammate's Pressure spell.
• Torrent + Frost (Flash Freeze): Spreads across a floor or moat, instantly freezing water into a walkable bridge or rooting enemies in place.
