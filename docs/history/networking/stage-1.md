# Networking history: stage 1 as built

Moved verbatim from `docs/design/networking.md` lines 338-360 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

---

### Stage 1 as built

Built: library `VortexSim` (`Vortex::Sim`: modules `vortex.scene` and the new `vortex.sim`; links Core, Physics, EnTT),
`sim::Simulation`, `sim::SimulationConfig`, `sim::ServerConfig`, `SceneConfig::bakeModels` / `markDebris`, the free
functions `vortex::GatherInstances(scene, out)` / `GatherLights(scene, out)` in `vortex.engine`, `Engine::Simulation()`
and `Engine::Tick()`, the test binary `VortexSimTests` (links `VortexSim` only) and a configure-time check
(`vortex_assert_gpu_free`) that stops the build when Core, Physics, Sim or `VortexSimTests` link a renderer, window or
UI library. Where the design was open or disagreed with the code, this was chosen:

| Question | Choice | Why, and what later stages must know |
|---|---|---|
| What are the `focusPoints` of `Step(focusPoints)`? `World` has one window around one focus; the server window never moves, a client window follows the camera. | `focusPoints` are **collider** focus points only (the players; may be empty). `Step` and `UpdateStreaming(focusPoints)` advance loading **without moving the window**; `UpdateStreaming(windowFocus, focusPoints)` centres the window first. Nothing is remembered between calls. | The server places its window once with `World().LoadBlocking(centre)` and then only calls `Step`. A world whose window was never placed gets it with the minimum corner at chunk (0, 0, 0). |
| `Engine::Run` streams once per frame, runs 0-4 ticks per frame and advances lifetimes per frame with the frame time; the server order does all of it per tick. | `Step` = `UpdateStreaming` + the physics of `StepPhysics` + `UpdateLifetimes(fixedTimeStep)`, in the server's order. The three phases are public; the engine calls them where it called the parts before, so the game is unchanged. Only a physics phase counts a tick: `Tick()` = ticks simulated = the number of the next one. | **Server and offline / client still differ in when lifetimes run** (per tick vs per rendered frame, with the frame time). Nothing that both sides must agree on may hang on `scene::Lifetime` or on rubble clean-up; such removals are server results (`EntityDespawn`, stage 6). Game code of a tick (stage 4 `StepPlayer`, stage 2 `Blast`) runs between the streaming phase and the physics phase, where `OnFixedUpdate` is today. |
| "Its world never has a `saveDirectory`", but `EngineConfig::world` is a `WorldConfig` and the editor has a "Save world" button. | The rule is a property of a session, not of every `Simulation`: `ServerConfig` clears the directory; any other configuration keeps it. (A first version cleared it for every simulation, which silently disabled the map editor's "Save world"; review rejected that.) | An offline game or the map editor keeps its edits when `EngineConfig::world.saveDirectory` is set; the game sets none. **Stage 3:** a network client must run memory-only too - `net::Client` should refuse (or clear, with a warning) a world that has a `saveDirectory`. |
| "No `Debris` component" on a server scene. | New `SceneConfig::markDebris` (default true); false = the scene never adds the component (pieces split off, remnants, `VoxelObjectDesc::debris` objects). `sim::ServerConfig(config)` sets it together with `bakeModels = false`, `debrisRemnants = false`, `maxParticles = 0`. | Until `Scene::Damage` takes the replicated-piece threshold (stage 2 / 6), a server scene keeps every piece of 16 voxels and more as a body for good (in `Layer::Debris` below 48 voxels). A zero-capacity particle pool no longer logs "pool is full". |
| `ParticleSystem` is presentation, but `Scene` is constructed from one. | `Simulation` owns it (capacity 0 on a server). `Step` does not advance particles and does not call `Scene::BeginFrame()`; the owner that draws does both. | `Transform::previous` is meaningless in a simulation nobody draws. `sim::Blast` / `PresentBlast` (stage 2) is where particle spawning leaves the simulation path. |
| "`Engine` gains only a tick number and a tick-rate scale." | The tick number is there (`Engine::Tick()`, `sim::Simulation::Tick()`, `uint32_t`: 2.2 years at 60 Hz). The tick-rate scale is not built. | It belongs to time sync (stage 4), where its consumer is. |
| Where do the scene tests live now that the scene is in `VortexSim`? | `tests/engine/SceneTests.cpp` stays in `VortexEngineTests` (it also tests the gather functions, which need `vortex.engine`). `tests/sim` holds the `Simulation` tests and the server-scene tests. | `tests/net` (stage 3) links `VortexNet` only, as planned. |

Not part of stage 1 and untouched: `Engine::Explode` is still in the engine (stage 2 turns it into `sim::Blast` +
`Engine::PresentBlast`); the loop still drops time after 4 ticks per frame and a headless run still creates a Vulkan
device; `Simulation` has no notion of players, operations or a session.
