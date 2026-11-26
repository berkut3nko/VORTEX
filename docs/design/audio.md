# Audio

[[../../../00 Docs Map|Docs map]] · [[mechanisms|mechanisms]] · [[shred|shred]] · [[magic|magic]] · [[game-ui|game UI]] · [[../../../docs/product/sound-list|sound list]]

| | |
|---|---|
| Status | `[audio-1]` built (2026-11-26): the `vortex.audio` module (manifest, mixer, miniaudio output), the shipped sound set (`assets/audio`, 16.53 MB from 1694.5 MB of WAV takes), the engine's sound presentation (`vortex::SoundEffects`) and the game's own sounds (`src/GameAudio.cpp`). Only headless runs are verified; the owner listens (docs/owner-checks.md check 30). |
| Decided | Us, 2026-11-26 (the owner may veto): every answer in "Decision" and the open questions of the brief (library, module, buses, voices, budgets, variation, settings). The owner, 2026-11-25 (sound-list.md header): variants with runtime variation. |
| Next | `[audio-2]`: the fight's sounds in a session (remote steps, hurts, kills, round stingers). |
| Open | "Open questions" |
| History | - |
| Marks | Code names from engine `a67ec57` plus `[audio-1]`. "Measured" = this machine's headless runs (`Audio:` exit line); the device cost is an estimate until the owner's run. |

Related: [mechanisms.md](mechanisms.md) (doors, levers, ropes, holds: the events), [shred.md](shred.md) (blasts and
islands), [magic.md](magic.md) `[mag-6]` (spell sounds, later), [game-ui.md](game-ui.md) (the settings page), the game's
`docs/product/sound-list.md` (the sound IDs, the file rules, the runtime variation rules) and
`docs/product/roadmap-and-launch.md` item C5.

## Decision

- **Library: miniaudio 0.11.25**, one C header, pinned by tag in `engine/cmake/Dependencies.cmake`. Ogg Vorbis comes
  from the `stb_vorbis.c` of its `extras`. Both compile once in the plain C file
  `engine/src/audio/internal/MiniaudioImpl.c` (the library `vortex_miniaudio`, warnings off), as VMA and tinygltf.
  MP3, FLAC and the encoders are compiled out (`MA_NO_MP3 MA_NO_FLAC MA_NO_ENCODING`).
- **Module: `vortex.audio`**, the library `Vortex::Audio` (`VortexAudio`), linked by `Vortex::Engine` only. It needs
  no GPU and no window, but it is built with `VORTEX_GRAPHICS`, because only the engine plays sound.
- **Presentation only.** No sound decides anything in the simulation. The random numbers of variation come from the
  mixer's own generator, never from `vortex.sim`.
- **Output never stops the game.** The default device plays. A headless run, or a machine without a device, mixes
  offline: the whole path runs and nothing is heard. `--no-audio` turns sound off. A missing manifest logs a warning
  and the game runs silent.
- **The shipped set is Ogg Vorbis**, made from the generated WAV takes by `tools/audio/build_sounds.py`, under a size
  budget (30 MB). The takes stay out of git.

## Layering

`VortexAudio` links `VortexCore` (log, random) and, privately, `vortex_miniaudio`. The configure step fails
(`vortex_assert_no_audio`) if `VortexCore`, `VortexPhysics`, `VortexSim`, `VortexNet`, `vortex_server`,
`vortex_bot`, `VortexSimTests` or `VortexNetTests` link it. A session server has no device, and the simulation
must not depend on what is heard.

| Part | File | What it holds |
|---|---|---|
| `vortex.audio:manifest` | `engine/src/audio/Manifest.cppm` | `Manifest` (the parsed `manifest.txt`), `SoundDesc`, `Bus`, `Distance`, `DistanceGain` |
| `vortex.audio:mixer` | `engine/src/audio/Mixer.cppm` | `Mixer` (the voice policy), `Backend` (what a device implements), `MixerConfig`, `MixerStats` |
| `vortex.audio:system` | `engine/src/audio/System.cppm` | `AudioSystem` (manifest + mixer + the miniaudio backend), `Ambience` (bed fades) |
| `vortex.engine:sound` | `engine/src/engine/Sound.cppm` | `SoundEffects` (blasts, impacts, mechanisms, ropes, holds), `SoundMaterialOf` |
| the game | `src/GameAudio.cpp` | steps, jumps, landings, casts, use, menus, ambience beds and spots |

## Assets

`tools/audio/build_sounds.py --takes <folder>` reads `docs/product/sound-list.md` (each ID's Length, Variants and
Runtime fields) and the takes `<kind>/<category>/<id>_v<N>_t<T>.wav`. For each variant it:

1. measures every take with ffmpeg: peak, true peak, the loudest momentary loudness, the integrated loudness and the
   silence at the start;
2. keeps one take: never a clipped one (peak at or above -0.05 dBFS), never one that starts more than 0.5 s late, else
   the loudest. The generator's `log.jsonl` records no choice, so loudness decides;
3. sets the level to the class target of sound-list.md section 16 (one-shots: momentary -16 LUFS, true peak -1 dBTP;
   UI: -20 LUFS, -3 dBTP; loops and beds: integrated -20 LUFS, -3 dBTP; music: integrated -16 LUFS, -1 dBTP);
4. cuts a one-shot's silence at both ends (-50 dB at the start, -60 dB at the end), caps its length at the larger of
   1.6 x and +0.4 s of its listed length, and fades the end over 80 ms;
5. makes a loop seamless: a bed is cut to at most 30 s, and its last 2 s are crossfaded into its start (equal
   power); a short loop takes its listed length and a 0.25 s seam;
6. writes Ogg Vorbis at 48 kHz: mono for every `mono` row (all `sfx` and `ui`, the ambience spots), stereo for beds
   and music. Quality 4 for one-shots and UI, 3 for music, 1 for beds (noise-like and long).

It writes `assets/audio/<kind>/<category>/<id>_v<N>.ogg` (variants numbered 1..n with no gaps),
`assets/audio/manifest.txt` and the licence log `docs/product/sound-licence-log.csv` (ID, variant, file, source take,
model, licence, date, seed, prompt; one row per kept file). It prints the size before and after, and exits 1 over the
budget. The game's build copies `assets/audio` to `<exe dir>/assets/audio`.

Measured on 2026-11-26 (after the owner's first listen): 256 sounds, 636 files, 1528 s of sound. 1694.5 MB of WAV
takes (1272) became 16.53 MB: beds and ambience 7.94, effects 7.04, music 1.22, UI 0.32. The manifest is 24.4 kB.

Two rules came from the owner's first listen (footsteps "like hooves"):
- **One hit for one event.** For a footstep (`sfx.foot.*` but the ice slide) the tool finds the separate hits of each
  take (a 5 ms peak envelope: a rise of 12 dB within 30 ms to within 20 dB of the peak; a hit within 120 ms of the first
  is the same step, heel and sole). It prefers a take with one step, and keeps only the first step: from 10 ms before
  its hit to 1.3 x the listed length, never into the next step.
- **Headroom for the encoder.** Ogg Vorbis at these qualities overshoots transients by up to 3 dB: with a -1 dBTP
  target, mechanism files decoded to +2 dBFS and clipped. One-shots now aim at -4 dBTP and music at -3 dBTP; no
  shipped file decodes over 0 dBFS.

The takes themselves are the larger fault: the generator gave footsteps 2-5 hits and the gate raise two bursts with
silence between them. sound-list.md section 3.1 has the reasons and the rewritten prompts.

### The manifest

One line per sound: `<id> bus=sfx variants=5 loop=0 gain=-12 pitch=1 vol=2 dist=S far=0 priority=0`. `#` starts a
comment line. `bus` and `variants` are required; an unknown key is refused (a typo would change the mix in silence).

| Key | Meaning |
|---|---|
| `bus` | `sfx`, `ambience`, `ui`, `music` |
| `variants` | files `_v1` .. `_vN` (1-32) |
| `loop` | 1: plays until stopped (beds, winch); never charged to the start budget |
| `gain` | the sound's level in the mix, dB (the files are all at one loudness) |
| `pitch`, `vol` | per-play jitter ranges: semitones and dB (the Runtime codes `P±n` and `V±n`, else the defaults of sound-list.md section 18) |
| `dist` | `2D` (not placed), `S`, `M`, `L`, `XL` (sound-list.md section 18) |
| `far` | 1: past 80 m the far layer `sfx.blast.distant` plays in its place |
| `priority` | 0-3, for voice stealing: 0 footsteps, patter, spots; 1 breaks, impacts, mechanisms; 2 blasts, casts, collapses, the player, beds; 3 UI and music |

The level table (`BASE_GAIN` in the tool) is the mix: footsteps -12 dB, blasts 0, breaks -5, impacts -7, collapses -1,
mechanisms -2, UI -8, music -6, beds -8, wind -10.

## Mixer

Five buses: master, and sfx, ambience, ui, music under it. Each is a volume 0..1; master scales the other four. A
bus is a miniaudio sound group; master is the engine's volume.

### Voices

`Mixer::Play(sound, params)` decides, in this order:

1. **Far layer.** A `far` sound past 80 m plays `sfx.blast.distant` (class XL) in its place.
2. **One play per burst.** The same ID within 1 m and 80 ms of a play that started plays once. Within 50 ms that
   voice becomes 2 dB louder, once (sound-list.md "Collapse budget").
3. **Start budget.** Each ID has a bucket of 6 starts that refills at 12 per second. A collapse that fires 40 breaks
   in one frame starts 6.
4. **Variation.** Pitch jitter (uniform, semitones) and volume jitter (uniform, dB) from the manifest's ranges.
5. **Audibility.** Level x distance gain x bus volume x master volume. Under -40 dB at the listener a one-shot does
   not start (a loop always starts: a bed fades in from nothing).
6. **Caps.** At most 4 voices of one ID: when full, the quietest of them stops if the new one is louder, else the new
   one does not start. At most 32 voices in all: when full, a voice of lower priority stops first (the quietest of
   them), then the quietest of the same priority if the new one is louder. A loop gives way only to a higher priority.
   Nothing takes a voice of a higher priority.
7. **Variant and readiness.** A variant at random, never the one played last for this ID; with 4 or more variants,
   not the one before it either. The backend must have it ready (`Backend::Ready`: the file is there and a one-shot
   is decoded). A variant still decoding on the job thread is not ready: the play is skipped (counted `failed`) and
   no voice is stolen for it, so no voice ever waits on a decode. The variant counts as played only once it starts.

Distance gain: 1 up to the inner distance, then `(inner / d) x (outer - d) / (outer - inner)`, 0 from the outer
distance on (S 2 / 25 m, M 3 / 60 m, L 5 / 150 m, XL 10 / 300 m). The mixer computes it and the pan itself
(miniaudio's spatializer is off): pan = the side of the source on the listener's right axis, x 0.85. Every frame
`Mixer::Update` frees finished voices and places the others again for the moved listener.

### Devices

`AudioSystem` holds one miniaudio engine at 48 kHz stereo with its resource manager (decoded data in 16-bit, one job
thread for streams). A one-shot variant is decoded into memory on its first play and kept (a prototype `ma_sound`); a
voice is a copy of it (a copy of a variant still loading starts once its data is there). A loop streams from its
file. With a device, `Engine::Initialize` starts decoding the sounds of every fight on miniaudio's job thread
(`AudioSystem::Preload`: steps, blasts, casts, breaks, impacts, patter, rubble, jump, landings, the menu; about 18 MB
of 16-bit PCM), so their first play never decodes on the main thread. Every other one-shot decodes on its first play,
on the main thread (2.5-4.6 ms in the gate runs, once 29.5 ms; measured offline). Offline, `AudioSystem::Update` pulls the frames a device would have pulled
(`ma_engine_read_pcm_frames`) and times it: the `Audio:` exit line reports the mixing cost.

## Variation

As sound-list.md section 18: a random variant (never the last one, and with 4 or more variants not the one before it),
pitch jitter and volume jitter from the manifest. `Size` and `Speed` are not random: the caller adds them to
`PlayParams::volumeDb` and `pitchSemitones` (`SoundEffects`: +2 dB and -1 semitone per doubling of the volume over the
class's smallest, at most 3 doublings; -12 dB at an impact speed of 3 m/s to 0 dB at 9 m/s).

## Events

Sounds play where pictures are made. `vortex.sim` calls nothing.

| Event | Where | Sounds |
|---|---|---|
| A presented blast | `Engine::PresentBlast` -> `SoundEffects::Blast` | `sfx.blast.core.small/medium/large` by radius (under 1 m, under 2 m, else); the break of the carved voxels' main material (`small` under 2 m^3, `large` above, with Size); `sfx.chaff.patter` 0.35 s later from 0.3 m^3; for the 3 largest islands a crack (`sfx.collapse.crack.stone/wood`, `slump` for soil), from 8 m^3 also `groan` and, 0.8 s later, `rumble`; `sfx.obj.crate.break` when objects lost voxels |
| A body stops | `SoundEffects::Watch` (every frame, from the pose at each tick; a held body, a tick gap over 4 or a tick that went back is skipped, and so is a replicated copy whose pose did not change: no sample came) | `sfx.impact.<material>.light/heavy` (crates: `sfx.obj.crate.impact`) with Speed; from 2 m^3 `sfx.collapse.land`; from 0.25 m^3 `sfx.rubble.settle` 0.3 s later |
| A mechanism moves or changes | `SoundEffects::Watch` (its newest segment) | door `unlatch` + `open` / `close`; portcullis `tension` + `raise` / `drop`, and `land` at the end of a drop; lever `throw` / `return`; flags: `freed`, `jammed`, lever `break` |
| A rope is cut | `SoundEffects::Watch` | `sfx.rope.snap`, chains `sfx.rope.chain.snap` |
| A body is picked up | `SoundEffects::Watch` (a new `scene::Held`) | `sfx.obj.pickup` (a client's predicted pick-up that the server refuses still sounded: `[audio-2]`) |
| The own player steps | the game (`GameAudio::PlayerStep`, after `sim::StepPlayer`) | `sfx.foot.<material>` at a person's cadence: `clamp(1.6 + 0.23 x speed, 1.8, 3.2)` steps a second, so one step per 1.5 m at the 3.8 m/s walk and per 2.2 m at the 7 m/s run (running: +2 dB, +0.5 semitone); `sfx.player.jump`; landing `land.soft` from 3 m/s, `land.hard` from 9 m/s |
| The own cast | the game | `sfx.cast.blast`; `sfx.blast.miss` into the sky |
| The use key | the game | a refusal `ui.use.refused`, a locked door `sfx.mech.door.locked`; a throw `sfx.obj.throw`; a hold lost `ui.hold.dropped` |
| The menus | the game | `ui.menu.open/close/move/select/back/value` |

The material of a voxel is its world palette name (`SoundMaterialOf`): grass, dirt, stone (with the castle's
granite, ashlar and core), brick, wood (planks, deadwood, bark), sand, snow, glass, iron, foliage, water, lava. A model
of another palette sounds as wood.

`SoundEffects::Watch` reads only the scene, so a session client hears the same blasts, impacts, mechanisms, ropes and
pick-ups from the replicated events as the offline game. Audio never travels over the network.

### Ambience

`audio::Ambience` fades beds in and out over 2.5 s (equal power); a bed starts when its weight leaves 0 and stops when
it returns there. Twice a second the game chooses the targets from where the camera is: under cover (rock or a roof
within 10 m overhead) `amb.cave.bed` and the outdoor beds at 0.2; the world map's biome at the camera (grassland,
forest, dark forest, desert, mountain, snow above 90 m, rock isle); sea, lake or river within 40 m (`amb.coast.waves`,
`amb.lake.lap`, `amb.river.stream`); and wind by the height above the ground (`low`, `mid` above 20 m, `high` above 60 m). Without a world
map (the playground) the bed is grassland. Every 5-14 s a spot of a bed at weight 0.5 or more plays 8-20 m away. A
bed that cannot start (a missing file, no voice) is tried again after 2 s.

## Settings

The settings page has five volumes in percent: Volume (master), Effects, Ambience, Interface, Music. Enter steps by
20 and wraps to 0. The file keys are `master_volume`, `sfx_volume`, `ambience_volume`, `ui_volume`, `music_volume`
(0-100; defaults 100, 100, 80, 80, 70). `ApplyPlayerSettings` gives them to `EngineConfig::audioVolumes` at the start;
`Engine::ApplySettings` sets a changed one.

## Cost

Measured on 2026-11-26 (the Windows PC, the 13 headless game runs of `verify.sh quick`, offline output; ROADMAP
"Performance and measured values"): mixing 0.14-0.54 % of one core with up to 23 voices; `AudioSystem::Update`
0.008-0.028 ms on the main thread; a first play that decodes, 2.5-4.6 ms. The device
thread does the same mixing work as offline. Memory: decoded one-shots are 16-bit (96 kB per second of mono sound): 18
MB preloaded, at most 91 MB if every one-shot played once; the `Audio:` line lists the variants decoded and their MB.

## Stages

| ID | Stage | State | Commits (engine / game) | History |
|---|---|---|---|---|
| `[audio-1]` | Module, mixer, manifest, the asset tool and the shipped set; blasts, impacts, mechanisms, ropes, holds, steps, casts, menus, ambience offline (and the engine's events in a session) | **Built** | this stage's commits | - |
| `[audio-2]` | The fight in a session: remote players' steps (`3D-S`), hurts, deaths, kills, the hit marker, round stingers (`mus.*`), the own steps of a predicting client | **Not started** | - | - |
| `[audio-3]` | Occlusion by the voxel ray (-6 dB and a low-pass), the far layer's distance delay, cave reverb | **Not started** | - | - |
| `[audio-4]` | Spells (`[mag-6]`), the castle's sounds, fire, weather, music beds | **Not started** | - | - |

## Rejected alternatives

- **OpenAL Soft**: a shared library to ship and its own 3D model; miniaudio is one header and a policy we own.
- **FMOD or Wwise**: licences and a closed runtime for a first pass that needs a mixer and a voice cap.
- **miniaudio's spatializer and its attenuation models**: its inverse model never reaches silence at the outer
  distance, and the policy would not be testable without a device. The mixer computes gain and pan.
- **Opus files**: smaller, but miniaudio needs libopus for them; stb_vorbis is one file.
- **git LFS for the sounds**: the repository does not use LFS, and the set is 17 MB.
- **Decoding every one-shot at the start**: 91 MB of 16-bit PCM (beds stream). Only the sounds of every fight are
  preloaded (18 MB); the rest decode on first play.

## Open questions

1. Occlusion: a voxel ray per voice per frame, or per new voice only? Recommended: per new voice and then every
   250 ms, at most 8 rays a frame (`[audio-3]`).
2. Remote players' footsteps in a session: from their snapshot motion, or from a step event? Recommended: from the
   motion of the remote record (no wire change).
3. The licence of the generator (Stability AI Community Licence, attribution and a revenue cap; see the licence log):
   the owner must confirm it before a release.
