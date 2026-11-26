module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

module vortex.engine;

namespace vortex {

    SoundMaterial SoundMaterialOf(std::string_view name) {
        struct Entry {
            std::string_view name;
            SoundMaterial material;
        };
        static constexpr Entry kTable[] = {
            {"grass", SoundMaterial::Grass},       {"grass_dry", SoundMaterial::Grass},     {"moss", SoundMaterial::Grass},
            {"dirt", SoundMaterial::Dirt},         {"mud", SoundMaterial::Dirt},            {"gravel", SoundMaterial::Dirt},
            {"litter", SoundMaterial::Dirt},       {"stone", SoundMaterial::Stone},         {"stone_dark", SoundMaterial::Stone},
            {"bedrock", SoundMaterial::Stone},     {"granite", SoundMaterial::Stone},       {"granite_quoin", SoundMaterial::Stone},
            {"ashlar", SoundMaterial::Stone},      {"rubble_core", SoundMaterial::Stone},   {"crystal", SoundMaterial::Glass},
            {"brick", SoundMaterial::Brick},       {"wood", SoundMaterial::Wood},           {"planks", SoundMaterial::Wood},
            {"deadwood", SoundMaterial::Wood},     {"bark_pale", SoundMaterial::Wood},      {"torch", SoundMaterial::Wood},
            {"sand", SoundMaterial::Sand},         {"snow", SoundMaterial::Snow},           {"glass", SoundMaterial::Glass},
            {"iron", SoundMaterial::Iron},         {"gold", SoundMaterial::Iron},           {"leaves", SoundMaterial::Foliage},
            {"leaves_autumn", SoundMaterial::Foliage}, {"needles", SoundMaterial::Foliage}, {"reed", SoundMaterial::Foliage},
            {"fern", SoundMaterial::Foliage},      {"flower_yellow", SoundMaterial::Foliage}, {"flower_violet", SoundMaterial::Foliage},
            {"mushroom", SoundMaterial::Foliage},  {"water", SoundMaterial::Water},         {"lava", SoundMaterial::Lava},
        };
        for (const Entry& e : kTable)
            if (e.name == name) return e.material;
        return SoundMaterial::None;
    }

    namespace {
        uint32_t EntityKey(scene::Entity e) { return static_cast<uint32_t>(entt::to_integral(e)); }

        /// The most common sound material of a set of voxel edits (by their old id), and the edits' centre.
        std::pair<SoundMaterial, glm::dvec3> MainMaterial(const std::vector<voxel::VoxelEdit>& edits, const SoundEffects& effects,
                                                          const voxel::World& world) {
            std::array<uint32_t, kSoundMaterialCount> count{};
            glm::dvec3 sum(0.0);
            // At most 4096 samples: a big island is tallied evenly, not wholly (presentation, not a count).
            const size_t stride = std::max<size_t>(1, edits.size() / 4096);
            size_t n = 0;
            for (size_t i = 0; i < edits.size(); i += stride, ++n) {
                ++count[static_cast<size_t>(effects.MaterialOf(edits[i].oldId))];
                sum += glm::dvec3(edits[i].voxel);
            }
            size_t best = 0;   // None only when nothing else was seen
            for (size_t m = 1; m < count.size(); ++m)
                if (count[m] > 0 && (best == 0 || count[m] > count[best])) best = m;
            const double vs = static_cast<double>(world.VoxelSize());
            const glm::dvec3 centre = n > 0 ? world.VoxelToWorld(glm::ivec3(sum / static_cast<double>(n))) + glm::dvec3(0.5 * vs) : glm::dvec3(0.0);
            return {static_cast<SoundMaterial>(best), centre};
        }

        /// The Size rule of sound-list.md section 2: +2 dB and -1 semitone per doubling above the class's smallest, at most 3 doublings.
        glm::vec2 SizeRule(double cubicMetres, double smallest) {
            const float doublings = std::clamp(static_cast<float>(std::log2(std::max(cubicMetres, 1.0e-9) / smallest)), 0.0f, 3.0f);
            return {2.0f * doublings, -doublings};
        }
    }

    SoundEffects::SoundEffects() {
        for (auto* table : {&m_Foot, &m_BreakSmall, &m_BreakLarge, &m_ImpactLight, &m_ImpactHeavy}) table->fill(audio::kNoSound);
    }

    void SoundEffects::Bind(audio::AudioSystem& audio, const voxel::MaterialLibrary& materials) {
        m_Audio = &audio;
        for (uint32_t id = 0; id < 256; ++id) m_ByMaterial[id] = id == 0 ? SoundMaterial::None : SoundMaterialOf(materials.Name(voxel::kWorldPalette, static_cast<uint8_t>(id)));
        const auto find = [&audio](std::string_view id) { return audio.Find(id); };
        const auto either = [&](std::string_view a, std::string_view b) {
            const audio::SoundId s = find(a);
            return s != audio::kNoSound ? s : find(b);
        };
        using M = SoundMaterial;
        const auto at = [](auto& table, M m) -> audio::SoundId& { return table[static_cast<size_t>(m)]; };
        m_Foot.fill(audio::kNoSound);
        m_BreakSmall.fill(audio::kNoSound);
        m_BreakLarge.fill(audio::kNoSound);
        m_ImpactLight.fill(audio::kNoSound);
        m_ImpactHeavy.fill(audio::kNoSound);
        at(m_Foot, M::Grass) = find("sfx.foot.grass");
        at(m_Foot, M::Foliage) = find("sfx.foot.grass");
        at(m_Foot, M::Dirt) = find("sfx.foot.dirt");
        at(m_Foot, M::Stone) = find("sfx.foot.stone");
        at(m_Foot, M::Brick) = find("sfx.foot.stone");
        at(m_Foot, M::Glass) = find("sfx.foot.stone");
        at(m_Foot, M::None) = find("sfx.foot.stone");
        at(m_Foot, M::Wood) = find("sfx.foot.wood");
        at(m_Foot, M::Sand) = find("sfx.foot.sand");
        at(m_Foot, M::Iron) = find("sfx.foot.iron");
        at(m_Foot, M::Snow) = either("sfx.foot.snow", "sfx.foot.dirt");
        at(m_Foot, M::Water) = find("sfx.foot.water");
        at(m_Foot, M::Lava) = find("sfx.foot.stone");
        struct Family {
            M material;
            std::string_view breakSmall, breakLarge, impactLight, impactHeavy;
        };
        const Family families[] = {
            {M::Stone, "sfx.break.stone.small", "sfx.break.stone.large", "sfx.impact.stone.light", "sfx.impact.stone.heavy"},
            {M::None, "sfx.break.stone.small", "sfx.break.stone.large", "sfx.impact.stone.light", "sfx.impact.stone.heavy"},
            {M::Lava, "sfx.break.stone.small", "sfx.break.stone.large", "sfx.impact.stone.light", "sfx.impact.stone.heavy"},
            {M::Brick, "sfx.break.brick.small", "sfx.break.brick.large", "sfx.impact.brick.light", "sfx.impact.brick.heavy"},
            {M::Wood, "sfx.break.wood.small", "sfx.break.wood.large", "sfx.impact.wood.light", "sfx.impact.wood.heavy"},
            {M::Dirt, "sfx.break.dirt.small", "sfx.break.dirt.large", "sfx.impact.dirt", "sfx.impact.dirt"},
            {M::Grass, "sfx.break.dirt.small", "sfx.break.dirt.large", "sfx.impact.dirt", "sfx.impact.dirt"},
            {M::Sand, "sfx.break.sand.small", "sfx.break.sand.large", "sfx.impact.sand", "sfx.impact.sand"},
            {M::Snow, "sfx.break.snow.small", "sfx.break.snow.large", "sfx.impact.dirt", "sfx.impact.dirt"},
            {M::Glass, "sfx.break.glass.small", "sfx.break.glass.large", "sfx.impact.glass", "sfx.impact.glass"},
            {M::Iron, "sfx.break.iron.hit", "sfx.break.iron.large", "sfx.impact.iron.light", "sfx.impact.iron.heavy"},
            {M::Foliage, "sfx.break.foliage", "sfx.break.foliage", "sfx.impact.dirt", "sfx.impact.dirt"},
            {M::Water, "sfx.break.liquid", "sfx.break.liquid", "sfx.impact.dirt", "sfx.impact.dirt"},
        };
        for (const Family& f : families) {
            at(m_BreakSmall, f.material) = either(f.breakSmall, "sfx.break.stone.small");
            at(m_BreakLarge, f.material) = either(f.breakLarge, f.breakSmall);
            at(m_ImpactLight, f.material) = either(f.impactLight, "sfx.impact.stone.light");
            at(m_ImpactHeavy, f.material) = either(f.impactHeavy, f.impactLight);
        }
        m_CoreSmall = find("sfx.blast.core.small");
        m_CoreMedium = find("sfx.blast.core.medium");
        m_CoreLarge = find("sfx.blast.core.large");
        m_Patter = find("sfx.chaff.patter");
        m_CrackStone = find("sfx.collapse.crack.stone");
        m_CrackWood = find("sfx.collapse.crack.wood");
        m_Groan = find("sfx.collapse.groan");
        m_Rumble = find("sfx.collapse.rumble");
        m_Land = find("sfx.collapse.land");
        m_Slump = find("sfx.collapse.slump");
        m_Settle = find("sfx.rubble.settle");
        m_CrateImpact = find("sfx.obj.crate.impact");
        m_CrateBreak = find("sfx.obj.crate.break");
        m_DoorOpen = find("sfx.mech.door.open");
        m_DoorClose = find("sfx.mech.door.close");
        m_DoorUnlatch = find("sfx.mech.door.unlatch");
        m_DoorFreed = find("sfx.mech.door.freed");
        m_DoorJammed = find("sfx.mech.door.jammed");
        m_PortRaise = find("sfx.mech.portcullis.raise");
        m_PortDrop = find("sfx.mech.portcullis.drop");
        m_PortLand = find("sfx.mech.portcullis.land");
        m_LeverThrow = find("sfx.mech.lever.throw");
        m_LeverReturn = find("sfx.mech.lever.return");
        m_LeverBreak = find("sfx.mech.lever.break");
        m_RopeSnap = find("sfx.rope.snap");
        m_ChainSnap = find("sfx.rope.chain.snap");
        m_Pickup = find("sfx.obj.pickup");
    }

    void SoundEffects::Play(audio::SoundId sound, const glm::dvec3& at, float volumeDb, float pitch) {
        if (!m_Audio || sound == audio::kNoSound) return;
        audio::PlayParams p;
        p.position = at;
        p.volumeDb = volumeDb;
        p.pitchSemitones = pitch;
        m_Audio->Play(sound, p);
    }

    void SoundEffects::Later(double seconds, audio::SoundId sound, const glm::dvec3& at, float volumeDb) {
        if (!m_Audio || sound == audio::kNoSound || m_Pending.size() >= 64) return;
        audio::PlayParams p;
        p.position = at;
        p.volumeDb = volumeDb;
        m_Pending.push_back(Pending{m_Now + seconds, sound, p});
    }

    void SoundEffects::Blast(const sim::BlastResult& blast, sim::Simulation& simulation) {
        if (!m_Audio) return;
        ++m_Stats.blasts;
        const voxel::World& world = simulation.World();
        const double vs = static_cast<double>(world.VoxelSize());
        const double voxelVolume = vs * vs * vs;
        // A late island search (deferred) has no explosion of its own: only what breaks off now sounds.
        if (!blast.deferred) {
            const audio::SoundId core = blast.radius < 1.0f ? m_CoreSmall : (blast.radius < 2.0f ? m_CoreMedium : m_CoreLarge);
            Play(core, blast.center);
        }
        std::vector<std::pair<size_t, size_t>> islands;   // (voxels, op index)
        for (size_t i = 0; i < blast.ops.size(); ++i) {
            const sim::AppliedOp& applied = blast.ops[i];
            if (applied.edits.empty()) continue;
            if (applied.op.kind == sim::WorldOpKind::CarveSphere) {
                // The break of the carved material: small up to 2 m^3, large above; Size within the class.
                const auto [material, centre] = MainMaterial(applied.edits, *this, world);
                const double m3 = static_cast<double>(applied.edits.size()) * voxelVolume;
                const bool large = m3 >= 2.0;
                const glm::vec2 size = SizeRule(m3, large ? 2.0 : 0.1);
                Play((large ? m_BreakLarge : m_BreakSmall)[static_cast<size_t>(material)], blast.center, size.x, size.y);
                ++m_Stats.breaks;
                // Chaff falls back a moment later (sound-list.md section 18, layering).
                if (m3 >= 0.3) Later(0.35, m_Patter, blast.center, -3.0f);
            } else if (applied.op.kind == sim::WorldOpKind::Detach) {
                islands.emplace_back(applied.edits.size(), i);
            }
        }
        // The three largest islands: a crack when one breaks off, and for a big one the groan, then the rumble.
        std::sort(islands.begin(), islands.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        for (size_t k = 0; k < islands.size() && k < 3; ++k) {
            const auto [material, centre] = MainMaterial(blast.ops[islands[k].second].edits, *this, world);
            const double m3 = static_cast<double>(islands[k].first) * voxelVolume;
            ++m_Stats.islands;
            if (m3 >= 1.0) {
                const bool soft = material == SoundMaterial::Dirt || material == SoundMaterial::Grass || material == SoundMaterial::Sand ||
                                  material == SoundMaterial::Snow;
                const audio::SoundId crack = soft ? m_Slump : (material == SoundMaterial::Wood ? m_CrackWood : m_CrackStone);
                const glm::vec2 size = SizeRule(m3, 1.0);
                Play(crack, centre, size.x, size.y);
                if (m3 >= 8.0) {
                    Play(m_Groan, centre, size.x - 2.0f);
                    Later(0.8, m_Rumble, centre, size.x - 2.0f);
                }
            } else {
                Play(m_BreakSmall[static_cast<size_t>(material)], centre, -3.0f);
            }
        }
        // Damaged objects (crates, furniture): a crate breaking.
        if (blast.objectVoxels >= 8) Play(m_CrateBreak != audio::kNoSound ? m_CrateBreak : m_BreakSmall[static_cast<size_t>(SoundMaterial::Wood)], blast.center);
    }

    SoundEffects::ModelInfo SoundEffects::InfoOf(const std::shared_ptr<voxel::VoxelModel>& model, const voxel::MaterialLibrary&) {
        const voxel::VoxelModel* key = model.get();
        if (auto it = m_Models.find(key); it != m_Models.end()) {
            if (it->second.model.lock() == model) return it->second;
            m_Models.erase(it);   // another model at a freed one's address
        }
        if (m_Models.size() > 4096) m_Models.clear();   // models long gone; a recount is cheap
        ModelInfo info;
        info.model = model;
        info.voxels = model->SolidCount();
        const std::string& name = model->Name();
        info.crate = name.find("crate") != std::string::npos || name.find("barrel") != std::string::npos;
        if (model->Palette() == voxel::kWorldPalette) {
            std::array<uint32_t, kSoundMaterialCount> count{};
            const std::vector<uint8_t>& data = model->Volume().Data();
            const size_t stride = std::max<size_t>(1, data.size() / 8192);
            for (size_t i = 0; i < data.size(); i += stride)
                if (data[i] != 0) ++count[static_cast<size_t>(m_ByMaterial[data[i]])];
            size_t best = 0;
            for (size_t m = 1; m < count.size(); ++m)
                if (count[m] > 0 && (best == 0 || count[m] > count[best])) best = m;
            info.material = static_cast<SoundMaterial>(best);
        } else {
            info.material = SoundMaterial::Wood;   // imported models: furniture, props
        }
        m_Models.emplace(key, info);
        return info;
    }

    void SoundEffects::Watch(sim::Simulation& simulation, float dt) {
        if (!m_Audio) return;
        m_Now += static_cast<double>(dt);
        for (size_t i = 0; i < m_Pending.size();) {
            if (m_Pending[i].at <= m_Now) {
                m_Audio->Play(m_Pending[i].sound, m_Pending[i].params);
                m_Pending[i] = m_Pending.back();
                m_Pending.pop_back();
            } else {
                ++i;
            }
        }
        WatchBodies(simulation);
        WatchMechanisms(simulation);
        WatchRopes(simulation);
        WatchHolds(simulation);
    }

    void SoundEffects::WatchBodies(sim::Simulation& simulation) {
        // A held body moves with its holder: its stops are the player's, not impacts (the view below leaves it out).
        // Velocities come from the pose at each tick (offline bodies and a client's replicated copies alike); an
        // impact is a speed that falls from above 3 m/s to under 60 % of it in one step.
        const uint32_t tick = simulation.Tick();
        const double dtTick = static_cast<double>(simulation.FixedTimeStep());
        const double vs = static_cast<double>(simulation.World().VoxelSize());
        entt::registry& registry = simulation.Scene().Registry();
        for (auto& [key, body] : m_Bodies) body.seen = false;
        auto view = registry.view<const scene::VoxelBody, const scene::Transform, const scene::RigidBody>(entt::exclude<scene::Mechanism, scene::Held>);
        for (const scene::Entity entity : view) {
            const scene::RigidBody& rb = view.get<const scene::RigidBody>(entity);
            if (rb.motion == physics::MotionType::Static) continue;
            if (rb.motion == physics::MotionType::Kinematic && !registry.all_of<scene::NetId>(entity)) continue;
            const glm::vec3 position = view.get<const scene::Transform>(entity).current.position;
            auto [it, inserted] = m_Bodies.try_emplace(EntityKey(entity));
            Body& b = it->second;
            b.seen = true;
            if (inserted) {
                b.position = position;
                b.tick = tick;
                continue;
            }
            if (tick == b.tick) continue;
            // A tick that went back (a client's clock) or a gap of more than 4 ticks: start the speed again.
            if (tick < b.tick || tick - b.tick > 4) {
                b.position = position;
                b.tick = tick;
                b.hasVelocity = false;
                continue;
            }
            // A replicated copy whose pose did not change got no sample (a starved buffer holds it in place): no
            // speed from that, or the hold reads as a stop and the catch-up as a second impact.
            if (position == b.position && registry.all_of<scene::NetId>(entity)) continue;
            const glm::vec3 velocity = (position - b.position) / static_cast<float>(static_cast<double>(tick - b.tick) * dtTick);
            if (b.hasVelocity) {
                const float before = glm::length(b.velocity);
                const float change = glm::length(b.velocity - velocity);
                // Above 40 m/s it was a teleport (a rest pose, a respawn), not a flight.
                if (before > 3.0f && before < 40.0f && change > 3.0f && glm::length(velocity) < 0.6f * before) {
                    const scene::VoxelBody& vb = view.get<const scene::VoxelBody>(entity);
                    if (vb.model) {
                        const ModelInfo info = InfoOf(vb.model, simulation.Materials());
                        const double m3 = static_cast<double>(info.voxels) * vs * vs * vs;
                        const bool heavy = m3 >= 0.5 || change > 9.0f;
                        const size_t mat = static_cast<size_t>(info.material);
                        const audio::SoundId sound =
                            info.crate && m_CrateImpact != audio::kNoSound ? m_CrateImpact : (heavy ? m_ImpactHeavy[mat] : m_ImpactLight[mat]);
                        // "Speed": -12 dB at 3 m/s, 0 dB at 9 m/s.
                        const float speedDb = std::clamp(-12.0f + 2.0f * (change - 3.0f), -12.0f, 0.0f);
                        Play(sound, glm::dvec3(position), speedDb);
                        if (m3 >= 2.0) Play(m_Land, glm::dvec3(position), speedDb + SizeRule(m3, 2.0).x);
                        else if (m3 >= 0.25 && !info.crate) Later(0.3, m_Settle, glm::dvec3(position), speedDb - 3.0f);
                        ++m_Stats.impacts;
                    }
                }
            }
            b.velocity = velocity;
            b.hasVelocity = true;
            b.position = position;
            b.tick = tick;
        }
        std::erase_if(m_Bodies, [](const auto& kv) { return !kv.second.seen; });
    }

    void SoundEffects::WatchMechanisms(sim::Simulation& simulation) {
        const uint32_t tick = simulation.Tick();
        entt::registry& registry = simulation.Scene().Registry();
        for (const scene::Entity entity : registry.view<const scene::Mechanism>()) {
            const scene::Mechanism& m = registry.get<const scene::Mechanism>(entity);
            if (m.track.count == 0) continue;
            const scene::MechanismSegment& segment = m.track.Newest();
            const glm::dvec3 at = glm::dvec3((m.swept.min + m.swept.max) * 0.5f);
            auto [it, inserted] = m_Mechanisms.try_emplace(EntityKey(entity));
            Track& t = it->second;
            if (inserted) {
                t.newest = segment;
                t.landed = true;
                continue;
            }
            if (!(segment == t.newest)) {
                const uint8_t added = static_cast<uint8_t>(segment.flags & ~t.newest.flags);
                if (added & scene::MechanismFlag_Broken)
                    Play(m.desc.kind == scene::MechanismKind::Lever ? m_LeverBreak : m_BreakLarge[static_cast<size_t>(SoundMaterial::Wood)], at);
                else if (added & scene::MechanismFlag_Freed)
                    Play(m_DoorFreed, at);
                else if (added & scene::MechanismFlag_Jammed)
                    Play(m_DoorJammed, at);
                const bool moves = segment.durationTicks > 0 && segment.startPos != segment.targetPos;
                if (moves) {
                    const bool opening = sim::IsOpening(m.desc, segment.startPos, segment.targetPos);
                    switch (m.desc.kind) {
                        case scene::MechanismKind::Door:
                            if (opening) Play(m_DoorUnlatch, at);
                            Play(opening ? m_DoorOpen : m_DoorClose, at);
                            break;
                        case scene::MechanismKind::Portcullis: Play(opening ? m_PortRaise : m_PortDrop, at); break;
                        case scene::MechanismKind::Lever: Play(opening ? m_LeverThrow : m_LeverReturn, at); break;
                    }
                    ++m_Stats.mechanisms;
                }
                t.newest = segment;
                t.landed = !moves;
            }
            if (!t.landed && tick >= segment.startTick + segment.durationTicks) {
                t.landed = true;
                if (m.desc.kind == scene::MechanismKind::Portcullis && !sim::IsOpening(m.desc, segment.startPos, segment.targetPos))
                    Play(m_PortLand, at);
            }
        }
        if (m_Mechanisms.size() > registry.view<const scene::Mechanism>().size()) {
            std::erase_if(m_Mechanisms, [&registry](const auto& kv) {
                const auto e = static_cast<scene::Entity>(kv.first);
                return !registry.valid(e) || !registry.all_of<scene::Mechanism>(e);
            });
        }
    }

    void SoundEffects::WatchRopes(sim::Simulation& simulation) {
        const voxel::World& world = simulation.World();
        const double vs = static_cast<double>(world.VoxelSize());
        entt::registry& registry = simulation.Scene().Registry();
        for (const scene::Entity entity : registry.view<const scene::Rope>()) {
            const scene::Rope& rope = registry.get<const scene::Rope>(entity);
            auto [it, inserted] = m_RopeCut.try_emplace(EntityKey(entity), rope.cut);
            if (inserted || it->second == rope.cut) continue;
            it->second = rope.cut;
            if (!rope.cut) continue;
            const glm::dvec3 at = world.VoxelToWorld(rope.anchorVoxel) + glm::dvec3(0.5 * vs, 0.0, 0.5 * vs);
            Play((rope.flags & scene::RopeFlag_Chain) ? m_ChainSnap : m_RopeSnap, at);
            ++m_Stats.ropes;
        }
        if (m_RopeCut.size() > registry.view<const scene::Rope>().size()) {
            std::erase_if(m_RopeCut, [&registry](const auto& kv) {
                const auto e = static_cast<scene::Entity>(kv.first);
                return !registry.valid(e) || !registry.all_of<scene::Rope>(e);
            });
        }
    }

    void SoundEffects::WatchHolds(sim::Simulation& simulation) {
        entt::registry& registry = simulation.Scene().Registry();
        for (auto& [key, held] : m_Held) held = false;
        for (const scene::Entity entity : registry.view<const scene::Held, const scene::Transform>()) {
            const glm::dvec3 at(registry.get<const scene::Transform>(entity).current.position);
            auto [it, inserted] = m_Held.try_emplace(EntityKey(entity), true);
            it->second = true;
            if (inserted) {
                Play(m_Pickup, at);
                ++m_Stats.holds;
            }
        }
        // Let go: forgotten (a throw is the game's sound; its landing is an impact).
        std::erase_if(m_Held, [](const auto& kv) { return !kv.second; });
    }
}
