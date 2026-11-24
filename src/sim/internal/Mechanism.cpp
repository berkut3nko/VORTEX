module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

module vortex.sim;

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.physics;
import vortex.scene;

namespace vortex::sim {

    const char* ToString(UseResult result) {
        switch (result) {
            case UseResult::None: return "accepted";
            case UseResult::Stale: return "stale";
            case UseResult::NoTarget: return "out of reach";
            case UseResult::Locked: return "locked";
            case UseResult::Busy: return "busy";
            case UseResult::Broken: return "broken";
            case UseResult::NotUsable: return "not usable";
        }
        return "unknown";
    }

    namespace {
        constexpr std::array<uint32_t, 65> MakeEaseOut() {
            std::array<uint32_t, 65> t{};
            for (int64_t i = 0; i < 65; ++i) t[static_cast<size_t>(i)] = static_cast<uint32_t>(16 * (4096 - (64 - i) * (64 - i)));
            return t;
        }

        constexpr std::array<uint32_t, 65> MakeEaseSmooth() {
            std::array<uint32_t, 65> t{};
            for (int64_t i = 0; i < 65; ++i) t[static_cast<size_t>(i)] = static_cast<uint32_t>((96 * i * i - i * i * i) / 2);
            return t;
        }

        constexpr bool EaseTableIsValid(const std::array<uint32_t, 65>& t) {
            if (t[0] != 0 || t[64] != 65536) return false;
            for (size_t i = 1; i < t.size(); ++i)
                if (t[i] < t[i - 1]) return false;
            return true;
        }

        static_assert(EaseTableIsValid(MakeEaseOut()));
        static_assert(EaseTableIsValid(MakeEaseSmooth()));
        static_assert(MakeEaseOut()[1] == 2032 && MakeEaseOut()[32] == 49152 && MakeEaseOut()[63] == 65520);
        static_assert(MakeEaseSmooth()[1] == 47 && MakeEaseSmooth()[32] == 32768 && MakeEaseSmooth()[63] == 65488);

        int32_t AbsOf(int32_t v) { return v < 0 ? -v : v; }

        /// The newest segment with startTick <= tick, or nullptr when every segment starts later.
        const MechanismSegment* SegmentAt(const MechanismTrack& track, uint32_t tick) {
            for (uint32_t i = track.count; i > 0; --i)
                if (track.ring[i - 1].startTick <= tick) return &track.ring[i - 1];
            return nullptr;
        }

        bool BoxIsValid(const scene::MechanismBox& box) {
            for (int a = 0; a < 3; ++a)
                if (box.min[a] < 0 || box.min[a] >= box.max[a] || box.max[a] > 255) return false;
            return true;
        }

        bool Refuse(std::string* why, const char* reason) {
            if (why) *why = reason;
            return false;
        }

        glm::vec3 AxisVec(uint8_t axis) {
            switch (axis) {
                case 0: return {1.0f, 0.0f, 0.0f};
                case 1: return {-1.0f, 0.0f, 0.0f};
                case 2: return {0.0f, 1.0f, 0.0f};
                case 3: return {0.0f, -1.0f, 0.0f};
                case 4: return {0.0f, 0.0f, 1.0f};
                default: return {0.0f, 0.0f, -1.0f};
            }
        }

        bool IsHinged(MechanismKind kind) { return kind == MechanismKind::Door || kind == MechanismKind::Lever; }

        /// Every mechanism in the visit order of every loop over them: ascending NetId value when
        /// the entity has one, else the entity id. Jolt and the registry order must never decide.
        std::vector<scene::Entity> MechanismsInOrder(const scene::Scene& scene) {
            const entt::registry& registry = scene.Registry();
            std::vector<std::pair<uint32_t, scene::Entity>> keyed;
            for (const scene::Entity entity : registry.view<const scene::Mechanism>()) {
                const scene::NetId* id = registry.try_get<scene::NetId>(entity);
                keyed.emplace_back(id ? id->value : static_cast<uint32_t>(entity), entity);
            }
            std::sort(keyed.begin(), keyed.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            std::vector<scene::Entity> out;
            out.reserve(keyed.size());
            for (const auto& [key, entity] : keyed) out.push_back(entity);
            return out;
        }

        glm::vec3 AngularVelocity(const glm::quat& q0, const glm::quat& q1, float dt) {
            glm::quat d = q1 * glm::conjugate(q0);
            if (d.w < 0.0f) d = -d;
            const float angle = 2.0f * std::acos(std::min(d.w, 1.0f));
            if (angle < 1.0e-7f) return glm::vec3(0.0f);
            return glm::normalize(glm::vec3(d.x, d.y, d.z)) * angle / dt;
        }

        /// The world point of current model voxel coordinates `local` (voxels, not metres) at `pose`.
        glm::dvec3 PosedPoint(const MechanismPose& pose, const glm::vec3& local, float voxelSize) {
            return pose.position + glm::dvec3(pose.rotation * (local * voxelSize));
        }

        /// World bounds of the current model voxel box [min, max) at `pose`.
        math::AABB PosedBox(const MechanismPose& pose, const glm::vec3& min, const glm::vec3& max, float voxelSize) {
            math::AABB box;
            for (int corner = 0; corner < 8; ++corner) {
                const glm::vec3 c((corner & 1) ? max.x : min.x, (corner & 2) ? max.y : min.y, (corner & 4) ? max.z : min.z);
                box.Grow(glm::vec3(PosedPoint(pose, c, voxelSize)));
            }
            return box;
        }

        /// The voxel box (in current model voxels, as floats) that holds the world box `world` at `pose`.
        math::AABB WorldBoxInModel(const MechanismPose& pose, const math::AABB& world, float voxelSize) {
            const glm::quat inverse = glm::conjugate(pose.rotation);
            math::AABB local;
            for (int corner = 0; corner < 8; ++corner) {
                const glm::dvec3 c((corner & 1) ? world.max.x : world.min.x, (corner & 2) ? world.max.y : world.min.y,
                                   (corner & 4) ? world.max.z : world.min.z);
                local.Grow(inverse * glm::vec3(c - pose.position) / voxelSize);
            }
            return local;
        }

        /// True when a solid world voxel lies in the voxel box of `box`.
        bool TerrainInBox(const voxel::World& world, const math::AABB& box) {
            const float vs = world.VoxelSize();
            const glm::ivec3 lo(glm::floor(box.min / vs));
            const glm::ivec3 hi(glm::ceil(box.max / vs));
            for (int z = lo.z; z < hi.z; ++z)
                for (int y = lo.y; y < hi.y; ++y)
                    for (int x = lo.x; x < hi.x; ++x)
                        if (world.GetVoxel({x, y, z}) != 0) return true;
            return false;
        }

        glm::ivec3 QuantizeSixteenth(const glm::dvec3& p, float voxelSize) {
            const double scale = 16.0 / static_cast<double>(voxelSize);
            return {static_cast<int>(std::llround(p.x * scale)), static_cast<int>(std::llround(p.y * scale)),
                    static_cast<int>(std::llround(p.z * scale))};
        }
    }

    const std::array<uint32_t, 65> kEaseOut = MakeEaseOut();
    const std::array<uint32_t, 65> kEaseSmooth = MakeEaseSmooth();

    uint32_t Ease(const std::array<uint32_t, 65>& table, uint32_t k, uint32_t D) {
        const uint64_t f = static_cast<uint64_t>(k) * 64u;
        const uint64_t i = f / D;
        const uint64_t r = f % D;
        const int64_t lo = table[i];
        const int64_t hi = table[i + 1];
        return static_cast<uint32_t>(lo + (hi - lo) * static_cast<int64_t>(r) / static_cast<int64_t>(D));
    }

    const std::array<uint32_t, 65>& EaseTableFor(const MechanismDesc& desc, const MechanismSegment& segment) {
        if (desc.kind == MechanismKind::Portcullis && segment.targetPos > segment.startPos) return kEaseSmooth;
        return kEaseOut;
    }

    bool IsOpening(const MechanismDesc& desc, int16_t from, int16_t to) {
        if (desc.kind == MechanismKind::Door) return AbsOf(to) > AbsOf(from);
        return to > from;
    }

    uint16_t SegmentDuration(int16_t from, int16_t to, const MechanismDesc& desc) {
        if (from == to) return 0;
        const int64_t range = static_cast<int64_t>(desc.maxPos) - desc.minPos;
        if (range <= 0) return 0;
        const int64_t full = IsOpening(desc, from, to) ? desc.openTicks : desc.closeTicks;
        const int64_t delta = AbsOf(static_cast<int32_t>(to) - static_cast<int32_t>(from));
        const int64_t d = (delta * full + range - 1) / range;
        return static_cast<uint16_t>(std::min<int64_t>(d, 65535));
    }

    int16_t PositionAt(const Mechanism& m, uint32_t tick) {
        const MechanismSegment* s = SegmentAt(m.track, tick);
        if (!s) return m.track.ring[0].startPos;
        const uint32_t k = tick - s->startTick;
        if (k >= s->durationTicks) return s->targetPos;
        const int64_t e = Ease(EaseTableFor(m.desc, *s), k, s->durationTicks);
        return static_cast<int16_t>(s->startPos + (static_cast<int64_t>(s->targetPos) - s->startPos) * e / 65536);
    }

    bool IsMoving(const MechanismTrack& track, uint32_t tick) {
        const MechanismSegment* s = SegmentAt(track, tick);
        return s && s->durationTicks > 0 && tick < s->startTick + s->durationTicks;
    }

    uint32_t ArrivalTick(const MechanismTrack& track) {
        if (track.count == 0) return 0;
        return track.Newest().startTick + track.Newest().durationTicks;
    }

    void PushSegment(MechanismTrack& track, const MechanismSegment& segment) {
        constexpr uint8_t kSticky = scene::MechanismFlag_Locked | scene::MechanismFlag_Jammed | scene::MechanismFlag_Freed |
                                    scene::MechanismFlag_Broken;
        MechanismSegment s = segment;
        if (track.count > 0) s.flags = static_cast<uint8_t>(s.flags | (track.Newest().flags & kSticky));
        if (track.count == kMechanismSegments) {
            for (uint32_t i = 1; i < kMechanismSegments; ++i) track.ring[i - 1] = track.ring[i];
            track.ring[kMechanismSegments - 1] = s;
        } else {
            track.ring[track.count++] = s;
        }
    }

    bool ValidateMechanismDesc(const MechanismDesc& desc, const glm::ivec3& /*modelSize*/, std::string* why) {
        const uint8_t kind = static_cast<uint8_t>(desc.kind);
        if (kind < static_cast<uint8_t>(MechanismKind::Door) || kind > static_cast<uint8_t>(MechanismKind::Lever))
            return Refuse(why, "unknown kind");
        if (desc.minPos >= desc.maxPos) return Refuse(why, "minPos is not below maxPos");
        if (desc.axis > 5) return Refuse(why, "axis is not 0-5");
        if (desc.pushAxis > 5) return Refuse(why, "pushAxis is not 0-5");
        if (desc.autoReturnTicks != 0) return Refuse(why, "autoReturnTicks is not 0");
        if (desc.openTicks == 0 || desc.closeTicks == 0) return Refuse(why, "a full-travel time is 0");
        if (!BoxIsValid(desc.useBox)) return Refuse(why, "use box is empty or out of 0-255");
        if (!BoxIsValid(desc.blockBox)) return Refuse(why, "block box is empty or out of 0-255");
        if (!BoxIsValid(desc.hingeBox)) return Refuse(why, "hinge box is empty or out of 0-255");
        const bool usable = (desc.flags & scene::MechanismDesc_Usable) != 0;
        if (desc.kind == MechanismKind::Portcullis && usable) return Refuse(why, "a portcullis is not usable");
        if (desc.kind != MechanismKind::Portcullis && !usable) return Refuse(why, "a door or a lever must be usable");
        if (desc.anchorCount != 0 && desc.anchorCount != 2) return Refuse(why, "anchorCount is not 0 or 2");
        return true;
    }

    MechanismPose PoseAt(const Mechanism& m, int16_t p, float voxelSize) {
        constexpr double kPi = 3.14159265358979323846;
        const glm::quat r0 = glm::angleAxis(static_cast<float>(m.quarterTurns * (kPi / 2.0)), glm::vec3(0.0f, 1.0f, 0.0f));
        const glm::vec3 o(m.modelOffset);
        glm::vec3 k;
        MechanismPose pose;
        if (IsHinged(m.desc.kind)) {
            const glm::vec3 pivot = glm::vec3(m.desc.pivotQ) / 16.0f;
            const glm::quat rp = glm::angleAxis(static_cast<float>(p * (2.0 * kPi / 4096.0)), AxisVec(m.desc.axis));
            k = pivot + rp * (o - pivot);
            pose.rotation = r0 * rp;
        } else {
            k = o + AxisVec(m.desc.axis) * (static_cast<float>(p) / 16.0f);
            pose.rotation = r0;
        }
        pose.position = glm::dvec3(m.spawnVoxel) * static_cast<double>(voxelSize) + glm::dvec3(r0 * (k * voxelSize));
        return pose;
    }

    physics::Layer MechanismLayer(const Mechanism& m, uint32_t tick) {
        if (!IsMoving(m.track, tick)) return physics::Layer::Dynamic;
        // A rising portcullis leaves the passage: it stays solid so nothing walks into its foot.
        const MechanismSegment* s = SegmentAt(m.track, tick);
        if (m.desc.kind == MechanismKind::Portcullis && s && s->targetPos > s->startPos) return physics::Layer::Dynamic;
        return physics::Layer::Rubble;
    }

    math::AABB SweptBounds(const Mechanism& m, const glm::ivec3& modelSize, float voxelSize) {
        math::AABB box;
        const glm::vec3 size(modelSize);
        const int32_t range = static_cast<int32_t>(m.desc.maxPos) - m.desc.minPos;
        for (int32_t j = 0; j <= 8; ++j) {
            const int16_t p = static_cast<int16_t>(m.desc.minPos + range * j / 8);
            box.Grow(PosedBox(PoseAt(m, p, voxelSize), glm::vec3(0.0f), size, voxelSize));
        }
        box.Expand(voxelSize);
        return box;
    }

    bool MakeMechanism(Simulation& simulation, scene::Entity entity, const MechanismDesc& desc, const glm::ivec3& spawnVoxel,
                       uint8_t quarterTurns, int16_t initialPos, uint32_t tick) {
        scene::Scene& scene = simulation.Scene();
        if (!scene.IsValid(entity)) return false;
        const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(entity);
        scene::RigidBody* rigid = scene.TryGet<scene::RigidBody>(entity);
        if (!voxels || !voxels->model || !rigid) {
            Log::Warn("Mechanisms: an entity without a voxel body and a rigid body cannot be a mechanism");
            return false;
        }
        const glm::ivec3 size = voxels->model->Volume().Size();
        std::string why;
        if (!ValidateMechanismDesc(desc, size, &why)) {
            Log::Warn("Mechanisms: '{}' refused: {}", voxels->model->Name(), why);
            return false;
        }

        scene::Mechanism m;
        m.desc = desc;
        m.spawnVoxel = spawnVoxel;
        m.quarterTurns = quarterTurns;
        const uint8_t flags = (desc.flags & scene::MechanismDesc_Locked) != 0 ? uint8_t{scene::MechanismFlag_Locked} : uint8_t{0};
        PushSegment(m.track, {tick, initialPos, initialPos, 0, flags});
        const float vs = simulation.World().VoxelSize();
        m.swept = SweptBounds(m, size, vs);
        const MechanismPose pose = PoseAt(m, initialPos, vs);
        const physics::Layer layer = MechanismLayer(m, tick);

        const physics::BodyId body = rigid->body;
        rigid->motion = physics::MotionType::Kinematic;
        physics::PhysicsWorld& physics = simulation.Physics();
        physics.SetMotionType(body, physics::MotionType::Kinematic);
        scene.Registry().emplace_or_replace<scene::Mechanism>(entity, std::move(m));
        scene.SetTransform(entity, glm::vec3(pose.position), pose.rotation, true);
        physics.SetLayer(body, layer);
        return true;
    }

    void PoseMechanisms(Simulation& simulation, uint32_t tick, const math::AABB& reach) {
        scene::Scene& scene = simulation.Scene();
        physics::PhysicsWorld& physics = simulation.Physics();
        const float vs = simulation.World().VoxelSize();
        const float dt = simulation.Config().fixedTimeStep;
        for (const scene::Entity entity : MechanismsInOrder(scene)) {
            const scene::Mechanism* m = scene.TryGet<scene::Mechanism>(entity);
            const scene::RigidBody* rigid = scene.TryGet<scene::RigidBody>(entity);
            if (!m || !rigid || m->track.count == 0 || !m->swept.Overlaps(reach)) continue;
            const MechanismPose a = PoseAt(*m, PositionAt(*m, tick), vs);
            const MechanismPose b = PoseAt(*m, PositionAt(*m, tick + 1), vs);
            const physics::BodyId body = rigid->body;
            const physics::Layer layer = MechanismLayer(*m, tick);
            scene.SetTransform(entity, glm::vec3(a.position), a.rotation, true);
            physics.SetVelocity(body, glm::vec3(b.position - a.position) / dt, AngularVelocity(a.rotation, b.rotation, dt));
            physics.SetLayer(body, layer);
        }
    }

    bool PushOutOfMechanisms(Simulation& simulation, uint32_t tick, PlayerState& state, physics::CharacterId character,
                             const PlayerTuning& tuning) {
        static std::atomic<bool> warnedVertical{false};
        scene::Scene& scene = simulation.Scene();
        const voxel::World& world = simulation.World();
        const float vs = world.VoxelSize();
        for (const scene::Entity entity : MechanismsInOrder(scene)) {
            const scene::Mechanism* m = scene.TryGet<scene::Mechanism>(entity);
            const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(entity);
            if (!m || !voxels || !voxels->model || m->track.count == 0) continue;
            const uint32_t arrival = ArrivalTick(m->track);
            if (m->track.Newest().durationTicks == 0 || (arrival != tick && arrival + 1 != tick)) continue;

            // 1. The capsule's box, 2 cm inside on every side (a touch is not an overlap).
            math::AABB bounds = PlayerBounds(state.position, tuning);
            bounds.min += glm::vec3(0.02f);
            bounds.max -= glm::vec3(0.02f);
            if (!m->swept.Overlaps(bounds)) continue;

            // 2. A solid model voxel whose world centre is inside the box.
            const MechanismPose pose = PoseAt(*m, PositionAt(*m, tick), vs);
            const voxel::VoxelVolume& volume = voxels->model->Volume();
            const math::AABB local = WorldBoxInModel(pose, bounds, vs);
            const glm::ivec3 lo = glm::max(glm::ivec3(glm::floor(local.min)), glm::ivec3(0));
            const glm::ivec3 hi = glm::min(glm::ivec3(glm::ceil(local.max)), volume.Size());
            bool overlap = false;
            for (int z = lo.z; z < hi.z && !overlap; ++z)
                for (int y = lo.y; y < hi.y && !overlap; ++y)
                    for (int x = lo.x; x < hi.x && !overlap; ++x)
                        if (volume.Get(x, y, z) != 0 &&
                            bounds.Contains(glm::vec3(PosedPoint(pose, glm::vec3(x, y, z) + glm::vec3(0.5f), vs))))
                            overlap = true;
            if (!overlap) continue;

            // 3. The push direction, horizontal.
            glm::vec3 n = pose.rotation * AxisVec(m->desc.pushAxis);
            n.y = 0.0f;
            const float length = glm::length(n);
            if (length < 0.5f) {
                if (!warnedVertical.exchange(true)) {
                    const scene::NetId* id = scene.TryGet<scene::NetId>(entity);
                    Log::Warn("Mechanisms: push axis of {} is vertical", id ? id->value : static_cast<uint32_t>(entity));
                }
                continue;
            }
            n /= length;

            // 4. The block box's posed centre and half depth along n.
            const glm::vec3 blockMin(m->desc.blockBox.min - m->modelOffset);
            const glm::vec3 blockMax(m->desc.blockBox.max - m->modelOffset);
            const glm::dvec3 mid = PosedPoint(pose, (blockMin + blockMax) * 0.5f, vs);
            float half = 0.0f;
            for (int corner = 0; corner < 8; ++corner) {
                const glm::vec3 c((corner & 1) ? blockMax.x : blockMin.x, (corner & 2) ? blockMax.y : blockMin.y,
                                  (corner & 4) ? blockMax.z : blockMin.z);
                half = std::max(half, std::abs(glm::dot(glm::vec3(PosedPoint(pose, c, vs) - mid), n)));
            }

            // 5. The side the feet are on, decided by quantised positions.
            const glm::dvec3 feet(state.position);
            const glm::vec3 quantised(QuantizeSixteenth(feet, vs) - QuantizeSixteenth(mid, vs));
            const float s = glm::dot(quantised, n) >= 0.0f ? 1.0f : -1.0f;

            // 6. That side first, then the other; terrain in the way blocks a side.
            for (const float side : {s, -s}) {
                const float d = glm::dot(glm::vec3(feet - mid), n) * side;
                const glm::vec3 target = state.position + n * (side * (half - d + tuning.radius + 0.02f));
                if (TerrainInBox(world, PlayerBounds(target, tuning))) continue;
                state.position = target;
                simulation.Physics().SetCharacterState(character, target, state.velocity);
                return true;
            }
            // 7. Both sides blocked: Jolt's penetration recovery.
        }
        return false;
    }

    MechanismStepResult StepMechanisms(Simulation& simulation, uint32_t tick, bool authority) {
        MechanismStepResult result;
        scene::Scene& scene = simulation.Scene();
        physics::PhysicsWorld& physics = simulation.Physics();
        const voxel::World& world = simulation.World();
        const float vs = world.VoxelSize();
        const float dt = simulation.Config().fixedTimeStep;
        for (const scene::Entity entity : MechanismsInOrder(scene)) {
            if (!scene.IsValid(entity)) continue;
            const scene::Mechanism* m = scene.TryGet<scene::Mechanism>(entity);
            const scene::RigidBody* rigid = scene.TryGet<scene::RigidBody>(entity);
            if (!m || !rigid || m->track.count == 0) continue;
            const physics::BodyId body = rigid->body;

            // 1. The motion of the coming physics step, from the timetable.
            const MechanismPose b = PoseAt(*m, PositionAt(*m, tick + 1), vs);
            if (IsMoving(m->track, tick) || ArrivalTick(m->track) == tick + 1) {
                const MechanismPose a = PoseAt(*m, PositionAt(*m, tick), vs);
                scene.SetTransform(entity, glm::vec3(a.position), a.rotation, true);
                physics.MoveKinematic(body, glm::vec3(b.position), b.rotation, dt);
                ++result.moving;
            } else if (tick > 0 && IsMoving(m->track, tick - 1)) {
                // Jolt keeps a kinematic body's last velocity: stop it exactly at its arrival pose.
                const MechanismPose a = PoseAt(*m, PositionAt(*m, tick), vs);
                scene.SetTransform(entity, glm::vec3(a.position), a.rotation, true);
                physics.SetVelocity(body, glm::vec3(0.0f), glm::vec3(0.0f));
            }

            // 2. The layer the coming physics step uses.
            physics.SetLayer(body, MechanismLayer(*m, tick + 1));

            // 3. The crush rule: the last ticks of a close remove the dynamic bodies in the block box.
            const MechanismSegment newest = m->track.Newest();
            const uint32_t arrival = newest.startTick + newest.durationTicks;
            if (authority && m->desc.kind != MechanismKind::Lever && newest.durationTicks > 0 &&
                !IsOpening(m->desc, newest.startPos, newest.targetPos) && tick + 1 + kMechanismCrushTicks >= arrival &&
                tick + 1 <= arrival) {
                const glm::vec3 blockMin(m->desc.blockBox.min - m->modelOffset);
                const glm::vec3 blockMax(m->desc.blockBox.max - m->modelOffset);
                std::vector<scene::Entity> found;
                scene.QueryBodies(PosedBox(b, blockMin, blockMax, vs), found);
                std::vector<std::pair<uint32_t, scene::Entity>> victims;
                for (const scene::Entity other : found) {
                    // A held body is a ghost posed by its holder: never crushed (released first, it can be).
                    if (other == entity || scene.TryGet<scene::Mechanism>(other) || scene.TryGet<scene::Held>(other)) continue;
                    const scene::RigidBody* otherBody = scene.TryGet<scene::RigidBody>(other);
                    if (!otherBody || otherBody->motion != physics::MotionType::Dynamic) continue;
                    const math::AABB bounds = physics.GetBounds(otherBody->body);
                    if (bounds.IsEmpty()) continue;
                    const math::AABB local = WorldBoxInModel(b, bounds, vs);
                    if (local.min.x < blockMax.x && local.max.x > blockMin.x && local.min.y < blockMax.y && local.max.y > blockMin.y &&
                        local.min.z < blockMax.z && local.max.z > blockMin.z) {
                        const scene::NetId* id = scene.TryGet<scene::NetId>(other);
                        victims.emplace_back(id ? id->value : static_cast<uint32_t>(other), other);
                    }
                }
                std::sort(victims.begin(), victims.end(), [](const auto& x, const auto& y) { return x.first < y.first; });
                for (const auto& [key, victim] : victims) {
                    const scene::RigidBody* victimBody = scene.TryGet<scene::RigidBody>(victim);
                    const glm::vec3 centre = victimBody ? physics.GetBounds(victimBody->body).Center() : glm::vec3(0.0f);
                    if (scene.TryGet<scene::NetId>(victim))
                        Despawn(simulation, victim, scene::DespawnReason::Broken);
                    else
                        scene.Destroy(victim);
                    ++result.crushed;
                    result.crushedAt.push_back(glm::dvec3(centre));
                }
            }

            // 4. Anchors: both in loaded air frees the mechanism.
            m = scene.TryGet<scene::Mechanism>(entity);
            if (authority && m && m->desc.anchorCount == 2) {
                bool air = true;
                for (const glm::ivec3& anchor : m->desc.anchors)
                    if (!world.IsChunkLoaded(voxel::VoxelToChunk(anchor)) || world.GetVoxel(anchor) != 0) air = false;
                if (air) ReleaseMechanism(simulation, entity, tick, scene::MechanismFlag_Freed, result.changes);
            }
        }
        return result;
    }

    namespace {
        /// The ray eye + dir * t against the voxel box [min, max) of the original model, posed at
        /// `pose` and grown by `inflate` metres. True with the entry distance in metres (0 when the
        /// eye is inside); false when the ray misses or the box lies behind the eye.
        bool RayPosedBox(const MechanismPose& pose, const glm::vec3& boxMin, const glm::vec3& boxMax, float inflate,
                         const glm::dvec3& eye, const glm::vec3& dir, float voxelSize, float& tHit) {
            const glm::vec3 lo = boxMin - glm::vec3(inflate / voxelSize);
            const glm::vec3 hi = boxMax + glm::vec3(inflate / voxelSize);
            const glm::quat inverse = glm::conjugate(pose.rotation);
            const glm::vec3 o = inverse * glm::vec3(eye - pose.position) / voxelSize;
            const glm::vec3 d = inverse * dir;
            float tNear = -std::numeric_limits<float>::max();
            float tFar = std::numeric_limits<float>::max();
            for (int a = 0; a < 3; ++a) {
                if (std::abs(d[a]) < 1.0e-12f) {
                    if (o[a] < lo[a] || o[a] > hi[a]) return false;
                    continue;
                }
                float t0 = (lo[a] - o[a]) / d[a];
                float t1 = (hi[a] - o[a]) / d[a];
                if (t0 > t1) std::swap(t0, t1);
                tNear = std::max(tNear, t0);
                tFar = std::min(tFar, t1);
            }
            if (tNear > tFar || tFar < 0.0f) return false;
            tHit = std::max(0.0f, tNear) * voxelSize;
            return true;
        }

        bool RayBox(const Mechanism& m, const MechanismPose& pose, const scene::MechanismBox& box, float inflate,
                    const glm::dvec3& eye, const glm::vec3& dir, float voxelSize, float& tHit) {
            return RayPosedBox(pose, glm::vec3(box.min - m.modelOffset), glm::vec3(box.max - m.modelOffset), inflate, eye, dir,
                               voxelSize, tHit);
        }

        /// The pose and model size (voxels) of a voxel body; false without a model or a transform.
        bool BodyBox(const scene::Scene& scene, scene::Entity entity, MechanismPose& pose, glm::vec3& size) {
            const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(entity);
            const scene::Transform* transform = scene.TryGet<scene::Transform>(entity);
            if (!voxels || !voxels->model || !transform) return false;
            pose.position = glm::dvec3(transform->current.position);
            pose.rotation = transform->current.rotation;
            size = glm::vec3(voxels->model->Volume().Size());
            return true;
        }

        /// True when the segment eye .. eye + dir * length meets `box` (its world AABB test).
        bool SegmentMeets(const math::AABB& box, const glm::dvec3& eye, const glm::vec3& dir, float length) {
            math::AABB segment;
            segment.Grow(glm::vec3(eye));
            segment.Grow(glm::vec3(eye + glm::dvec3(dir) * static_cast<double>(length)));
            return box.Overlaps(segment);
        }

        bool IsUsable(const Mechanism& m) { return (m.desc.flags & scene::MechanismDesc_Usable) != 0; }

        bool IsLockedOrJammed(const Mechanism& m) {
            return m.track.count > 0 && (m.track.Newest().flags & (scene::MechanismFlag_Locked | scene::MechanismFlag_Jammed)) != 0;
        }

        uint32_t MechanismKey(const scene::Scene& scene, scene::Entity entity) {
            const scene::NetId* id = scene.TryGet<scene::NetId>(entity);
            return id ? id->value : static_cast<uint32_t>(entity);
        }

        /// The centre of a box of original model voxels, in those voxels.
        glm::vec3 BoxCentre(const scene::MechanismBox& box) { return glm::vec3(box.min + box.max) * 0.5f; }
    }

    bool FindUseTarget(const Simulation& simulation, const glm::dvec3& eye, const glm::vec3& dir, uint32_t tick, UseTarget& out,
                       const UseTargetHooks* hooks) {
        const scene::Scene& scene = simulation.Scene();
        const float vs = simulation.World().VoxelSize();
        const voxel::WorldRayHit terrain = simulation.World().Raycast(eye, dir, kUseReach);
        const float limit = terrain.hit ? terrain.distance : kUseReach;
        const auto keyOf = [&](scene::Entity e) { return hooks && hooks->key ? hooks->key(e) : MechanismKey(scene, e); };

        // Targets (a usable mechanism's use box, a holdable body) and occluders (every block box, every held body).
        struct Hit {
            scene::Entity entity;
            UseTargetKind kind;
            uint32_t key;
            float t;
        };
        const auto rank = [](const Hit& h) { return std::make_tuple(h.t, static_cast<uint8_t>(h.kind), h.key); };
        std::vector<Hit> targets;
        std::vector<Hit> occluders;   // kind: Mechanism (usable), Info (not usable) or HeldByOther
        for (const scene::Entity entity : MechanismsInOrder(scene)) {
            const scene::Mechanism* m = scene.TryGet<scene::Mechanism>(entity);
            if (!m || m->track.count == 0 || !SegmentMeets(m->swept, eye, dir, kUseReach)) continue;
            const MechanismPose pose = PoseAt(*m, PositionAt(*m, tick), vs);
            const uint32_t key = keyOf(entity);
            float t = 0.0f;
            if (RayBox(*m, pose, m->desc.blockBox, 0.0f, eye, dir, vs, t))
                occluders.push_back({entity, IsUsable(*m) ? UseTargetKind::Mechanism : UseTargetKind::Info, key, t});
            if (IsUsable(*m) && RayBox(*m, pose, m->desc.useBox, kUseBoxInflate, eye, dir, vs, t))
                targets.push_back({entity, UseTargetKind::Mechanism, key, t});
        }

        math::AABB reach;
        reach.Grow(glm::vec3(eye));
        reach.Grow(glm::vec3(eye + glm::dvec3(dir) * static_cast<double>(kUseReach)));
        std::vector<scene::Entity> bodies;
        scene.QueryBodies(reach, bodies);
        const glm::dvec3 anchor = eye - glm::dvec3(0.0, kHoldDrop, 0.0);
        for (const scene::Entity entity : bodies) {
            MechanismPose pose;
            glm::vec3 size;
            if (scene.TryGet<scene::Mechanism>(entity) || !BodyBox(scene, entity, pose, size)) continue;
            float t = 0.0f;
            if (scene.TryGet<scene::Held>(entity)) {
                if (RayPosedBox(pose, glm::vec3(0.0f), size, 0.0f, eye, dir, vs, t))
                    occluders.push_back({entity, UseTargetKind::HeldByOther, keyOf(entity), t});
                continue;
            }
            if (!IsHoldableShape(simulation, entity)) continue;
            const bool owner = hooks && hooks->holdable ? hooks->holdable(entity) : IsHoldable(simulation, entity, false);
            if (!owner || !PickupPathClear(simulation, entity, anchor)) continue;
            if (RayPosedBox(pose, glm::vec3(0.0f), size, kUseBoxInflate, eye, dir, vs, t))
                targets.push_back({entity, UseTargetKind::Holdable, keyOf(entity), t});
        }
        // [mech-2] adds the rope source here (cleats and climbable ropes), under the same order rule.

        // The winner: the smallest distance in front of the terrain and of every other occluder; ties go to
        // the lower kind, then the lower key. A mechanism's own block box does not hide its own use box.
        const Hit* best = nullptr;
        for (const Hit& c : targets) {
            if (c.t >= limit) continue;
            bool hidden = false;
            for (const Hit& o : occluders)
                if (o.entity != c.entity && o.t <= c.t) hidden = true;
            if (hidden) continue;
            if (!best || rank(c) < rank(*best)) best = &c;
        }
        if (best) {
            out = {best->entity, false, best->t, best->kind};
            return true;
        }

        // Else the nearest occluder within reach answers: a non-usable mechanism (Info) or a held body.
        const Hit* nearest = nullptr;
        for (const Hit& o : occluders) {
            if (o.t >= limit) continue;
            if (!nearest || rank(o) < rank(*nearest)) nearest = &o;
        }
        if (nearest && (nearest->kind == UseTargetKind::Info || nearest->kind == UseTargetKind::HeldByOther)) {
            out = {nearest->entity, nearest->kind == UseTargetKind::Info, nearest->t, nearest->kind};
            return true;
        }
        out = {};
        return false;
    }

    bool RayMeetsUseBox(const Simulation& simulation, scene::Entity entity, const glm::dvec3& eye, const glm::vec3& dir,
                        uint32_t tick, float inflate) {
        const scene::Mechanism* m = simulation.Scene().TryGet<scene::Mechanism>(entity);
        if (!m || m->track.count == 0) return false;
        const float vs = simulation.World().VoxelSize();
        float t = 0.0f;
        return RayBox(*m, PoseAt(*m, PositionAt(*m, tick), vs), m->desc.useBox, inflate, eye, dir, vs, t) &&
               t <= kUseReach + inflate;
    }

    UseResult ApplyUse(Simulation& simulation, const UseTarget& target, const glm::dvec3& eye, uint32_t tick, uint32_t leadTicks,
                       std::vector<MechanismChange>& changes) {
        scene::Scene& scene = simulation.Scene();
        if (!scene.IsValid(target.entity)) return UseResult::NoTarget;
        scene::Mechanism* m = scene.TryGet<scene::Mechanism>(target.entity);
        if (!m || m->track.count == 0) return UseResult::NoTarget;

        // 1. An Info target answers only.
        if (target.info || !IsUsable(*m)) return IsLockedOrJammed(*m) ? UseResult::Locked : UseResult::NotUsable;

        // 2. A lever's target, while it is still a mechanism.
        scene::Entity gEntity = scene::kNoEntity;
        scene::Mechanism* g = nullptr;
        if (m->desc.kind == MechanismKind::Lever && m->linked != scene::kNoEntity && scene.IsValid(m->linked)) {
            g = scene.TryGet<scene::Mechanism>(m->linked);
            if (g && g->track.count > 0) gEntity = m->linked;
            else g = nullptr;
        }

        // 3. Locked or jammed, 4. busy.
        if (IsLockedOrJammed(*m) || (g && IsLockedOrJammed(*g))) return UseResult::Locked;
        const auto busy = [tick](const Mechanism& x) { return x.anyDecision && tick - x.lastDecisionTick < kMechanismBusyTicks; };
        if (busy(*m) || (g && busy(*g))) return UseResult::Busy;

        // 5. Where each part starts: never before its newest segment.
        const auto startOf = [tick, leadTicks](const Mechanism& x) { return std::max(tick + leadTicks, x.track.Newest().startTick + 1); };
        const uint32_t sm = startOf(*m);
        const int16_t pm = PositionAt(*m, sm);

        // 6. What each part moves to.
        int16_t to = 0;
        int16_t gTo = 0;
        if (m->desc.kind == MechanismKind::Door) {
            if (m->track.Newest().targetPos != 0) {
                to = 0;
            } else if ((m->desc.flags & scene::MechanismDesc_OneWay) != 0) {
                to = m->desc.maxPos;
            } else {
                constexpr double kPi = 3.14159265358979323846;
                const float vs = simulation.World().VoxelSize();
                const glm::quat r0 = glm::angleAxis(static_cast<float>(m->quarterTurns * (kPi / 2.0)), glm::vec3(0.0f, 1.0f, 0.0f));
                const glm::dvec3 origin = glm::dvec3(m->spawnVoxel) * static_cast<double>(vs);
                const glm::vec3 eyeO = glm::conjugate(r0) * glm::vec3(eye - origin) / vs;
                const glm::vec3 pivot = glm::vec3(m->desc.pivotQ) / 16.0f;
                const glm::vec3 v = BoxCentre(m->desc.useBox) - pivot;
                const glm::vec3 sdir = glm::cross(AxisVec(m->desc.axis), v);
                to = glm::dot(eyeO - pivot, sdir) > 0.0f ? m->desc.minPos : m->desc.maxPos;
            }
        } else {
            to = m->track.Newest().targetPos == m->desc.maxPos ? m->desc.minPos : m->desc.maxPos;
            if (g) gTo = (to == m->desc.maxPos) ? g->desc.maxPos : g->desc.minPos;
        }

        // 7. Closing wins on the moving part: an open request waits until a close has settled.
        const Mechanism* moving = m->desc.kind == MechanismKind::Lever ? g : m;
        const bool openRequest = m->desc.kind == MechanismKind::Lever ? (g && gTo == g->desc.maxPos) : to != 0;
        if (moving && openRequest) {
            const MechanismSegment& newest = moving->track.Newest();
            if (newest.durationTicks > 0 && !IsOpening(moving->desc, newest.startPos, newest.targetPos) &&
                tick < ArrivalTick(moving->track) + kMechanismReopenTicks)
                return UseResult::Busy;
        }

        // 8-9. Accept: the lever (or door) first, then the lever's target. A lever without one still throws.
        const auto push = [&](scene::Entity entity, scene::Mechanism& x, uint32_t start, int16_t p0, int16_t target) {
            const MechanismSegment seg{start, p0, target, SegmentDuration(p0, target, x.desc), x.track.Newest().flags};
            PushSegment(x.track, seg);
            x.lastDecisionTick = tick;
            x.anyDecision = true;
            changes.push_back({entity, x.track.Newest()});
        };
        push(target.entity, *m, sm, pm, to);
        if (m->desc.kind == MechanismKind::Lever && !g) return UseResult::Broken;
        if (g) {
            const uint32_t sg = startOf(*g);
            push(gEntity, *g, sg, PositionAt(*g, sg), gTo);
        }
        return UseResult::None;
    }

    std::string UsePrompt(const Simulation& simulation, const UseTarget& target, uint32_t /*tick*/, bool& greyed) {
        greyed = false;
        if (target.kind == UseTargetKind::Holdable) return "[E] Pick up";
        if (target.kind == UseTargetKind::HeldByOther) {
            greyed = true;
            return "Held";
        }
        const scene::Mechanism* m = simulation.Scene().TryGet<scene::Mechanism>(target.entity);
        if (!m || m->track.count == 0) return {};
        if (target.info || !IsUsable(*m)) {
            greyed = true;
            return IsLockedOrJammed(*m) ? "Locked" : "Gate - opened by a lever";
        }
        if (IsLockedOrJammed(*m)) {
            greyed = true;
            return "Locked";
        }
        if (m->desc.kind == MechanismKind::Door) return m->track.Newest().targetPos != 0 ? "[E] Close" : "[E] Open";
        return m->track.Newest().targetPos == m->desc.maxPos ? "[E] Lower gate" : "[E] Raise gate";
    }

    MechanismHit DamageMechanism(Simulation& /*simulation*/, scene::Entity /*entity*/, const glm::dvec3& /*centre*/, uint16_t /*powerQ*/,
                                 BlastResult& /*into*/) {
        // [mech-1] card 5 fills this
        return {};
    }

    BlastResult MechanismBlast(Simulation& /*simulation*/, scene::Entity /*target*/, const voxel::SphereQ& /*sphere*/, uint16_t /*power*/) {
        // [mech-1] card 5 fills this
        return {};
    }

    float DistanceToMechanism(const Simulation& /*simulation*/, scene::Entity /*entity*/, const glm::dvec3& /*point*/) {
        // [mech-1] card 5 fills this
        return 0.0f;
    }

    void ReleaseMechanism(Simulation& simulation, scene::Entity entity, uint32_t tick, uint8_t flag, std::vector<MechanismChange>& changes) {
        scene::Scene& scene = simulation.Scene();
        scene::Mechanism* m = scene.TryGet<scene::Mechanism>(entity);
        if (!m || m->track.count == 0) return;
        const int16_t p = PositionAt(*m, tick);
        const MechanismSegment newest = m->track.Newest();
        PushSegment(m->track, {std::max(tick, newest.startTick + 1), p, p, 0, static_cast<uint8_t>(newest.flags | flag)});
        changes.push_back({entity, m->track.Newest()});

        if (m->desc.kind == MechanismKind::Lever) {
            if (scene.TryGet<scene::NetId>(entity))
                Despawn(simulation, entity, scene::DespawnReason::Broken);
            else
                scene.Destroy(entity);
            return;
        }

        // A door or a portcullis off its hinge: its levers lose their link, and it is an ordinary body from here.
        entt::registry& registry = scene.Registry();
        for (auto [other, lever] : registry.view<scene::Mechanism>().each()) {
            if (lever.linked != entity) continue;
            lever.linked = scene::kNoEntity;
            lever.linkedNetId = 0;
        }
        registry.remove<scene::Mechanism>(entity);
        if (scene::RigidBody* rigid = scene.TryGet<scene::RigidBody>(entity)) {
            physics::PhysicsWorld& physics = simulation.Physics();
            rigid->motion = physics::MotionType::Dynamic;
            physics.SetMotionType(rigid->body, physics::MotionType::Dynamic);
            physics.SetLayer(rigid->body, physics::Layer::Dynamic);
            physics.Activate(rigid->body);
        }
    }

    // ---------------------------------------------------------------------------------------------
    //  Holding a small object with E (docs/design/mechanisms.md "One use key: E").
    // ---------------------------------------------------------------------------------------------

    namespace {
        /// The radius of the sphere a held or picked-up object is swept as: half its smallest side less half a voxel.
        double SweepRadius(const glm::vec3& size, float voxelSize) {
            const float smallest = std::min(size.x, std::min(size.y, size.z));
            return std::max(0.0, 0.5 * (static_cast<double>(smallest) - 1.0) * static_cast<double>(voxelSize));
        }

        /// The first distance along `dir` (unit) from `from`, up to `maxT`, at which a sphere of radius `r`
        /// meets a solid world voxel or a mechanism's block box posed at `tick`; `maxT` when it meets none.
        double SweepSphere(const Simulation& simulation, const glm::dvec3& from, const glm::dvec3& dir, double r, double maxT,
                           uint32_t tick) {
            const voxel::World& world = simulation.World();
            const double vs = world.VoxelSize();
            double best = maxT;
            const glm::dvec3 to = from + dir * maxT;
            const glm::dvec3 lo = glm::min(from, to) - glm::dvec3(r);
            const glm::dvec3 hi = glm::max(from, to) + glm::dvec3(r);
            const glm::ivec3 vlo(glm::floor(lo / vs));
            const glm::ivec3 vhi(glm::floor(hi / vs));
            const double r2 = r * r;
            for (int z = vlo.z; z <= vhi.z; ++z)
                for (int y = vlo.y; y <= vhi.y; ++y)
                    for (int x = vlo.x; x <= vhi.x; ++x) {
                        if (world.GetVoxel({x, y, z}) == 0) continue;
                        const glm::dvec3 c = (glm::dvec3(x, y, z) + glm::dvec3(0.5)) * vs;
                        const glm::dvec3 w = c - from;
                        const double b = glm::dot(w, dir);
                        const double q = glm::dot(w, w) - r2;
                        if (q <= 0.0) return 0.0;
                        const double disc = b * b - q;
                        if (b > 0.0 && disc >= 0.0) best = std::min(best, b - std::sqrt(disc));
                    }

            const scene::Scene& scene = simulation.Scene();
            const float fvs = world.VoxelSize();
            for (const scene::Entity entity : MechanismsInOrder(scene)) {
                const scene::Mechanism* m = scene.TryGet<scene::Mechanism>(entity);
                if (!m || m->track.count == 0) continue;
                math::AABB grown = m->swept;
                grown.Expand(static_cast<float>(r));
                math::AABB segment;
                segment.Grow(glm::vec3(from));
                segment.Grow(glm::vec3(to));
                if (!grown.Overlaps(segment)) continue;
                const MechanismPose pose = PoseAt(*m, PositionAt(*m, tick), fvs);
                float t = 0.0f;
                if (RayBox(*m, pose, m->desc.blockBox, static_cast<float>(r), from, glm::vec3(dir), fvs, t))
                    best = std::min(best, static_cast<double>(t));
            }
            return best;
        }

        /// The world centre of a voxel body's model box at `pose`.
        glm::dvec3 BoxCentreOf(const MechanismPose& pose, const glm::vec3& size, float voxelSize) {
            return pose.position + glm::dvec3(pose.rotation * (size * (0.5f * voxelSize)));
        }

        /// World bounds of the model box [0, size) at `pose`.
        math::AABB BodyWorldBox(const MechanismPose& pose, const glm::vec3& size, float voxelSize) {
            return PosedBox(pose, glm::vec3(0.0f), size, voxelSize);
        }

        /// The separating-axis test of two oriented boxes, each given as a centre, three unit axes and half extents.
        bool BoxesOverlap(const glm::dvec3& ca, const std::array<glm::dvec3, 3>& ua, const glm::dvec3& ha, const glm::dvec3& cb,
                          const std::array<glm::dvec3, 3>& ub, const glm::dvec3& hb) {
            const glm::dvec3 d = cb - ca;
            const auto separated = [&](const glm::dvec3& axis) {
                const double len = glm::length(axis);
                if (len < 1.0e-9) return false;
                const glm::dvec3 n = axis / len;
                double ra = 0.0, rb = 0.0;
                for (int i = 0; i < 3; ++i) {
                    ra += ha[i] * std::abs(glm::dot(ua[static_cast<size_t>(i)], n));
                    rb += hb[i] * std::abs(glm::dot(ub[static_cast<size_t>(i)], n));
                }
                return std::abs(glm::dot(d, n)) > ra + rb;
            };
            for (const glm::dvec3& a : ua)
                if (separated(a)) return false;
            for (const glm::dvec3& b : ub)
                if (separated(b)) return false;
            for (const glm::dvec3& a : ua)
                for (const glm::dvec3& b : ub)
                    if (separated(glm::cross(a, b))) return false;
            return true;
        }

        std::array<glm::dvec3, 3> AxesOf(const glm::quat& rotation) {
            return {glm::dvec3(rotation * glm::vec3(1.0f, 0.0f, 0.0f)), glm::dvec3(rotation * glm::vec3(0.0f, 1.0f, 0.0f)),
                    glm::dvec3(rotation * glm::vec3(0.0f, 0.0f, 1.0f))};
        }

        /// The release fix-up's test: the model box of `entity` at `pose` holds no solid world voxel, meets no
        /// mechanism block box, no other resting body and no player capsule.
        bool ReleaseClear(const Simulation& simulation, scene::Entity entity, const MechanismPose& pose, const glm::vec3& size,
                          const HoldEnv& env, uint32_t tick) {
            const voxel::World& world = simulation.World();
            const float vs = world.VoxelSize();
            const math::AABB box = BodyWorldBox(pose, size, vs);

            // (a) Solid world voxels whose centres lie inside the oriented box.
            const glm::quat inverse = glm::conjugate(pose.rotation);
            const glm::ivec3 lo(glm::floor(box.min / vs));
            const glm::ivec3 hi(glm::floor(box.max / vs));
            for (int z = lo.z; z <= hi.z; ++z)
                for (int y = lo.y; y <= hi.y; ++y)
                    for (int x = lo.x; x <= hi.x; ++x) {
                        if (world.GetVoxel({x, y, z}) == 0) continue;
                        const glm::dvec3 c = (glm::dvec3(x, y, z) + glm::dvec3(0.5)) * static_cast<double>(vs);
                        const glm::vec3 local = inverse * glm::vec3(c - pose.position) / vs;
                        if (local.x > 0.0f && local.y > 0.0f && local.z > 0.0f && local.x < size.x && local.y < size.y && local.z < size.z)
                            return false;
                    }

            // (b) Mechanism block boxes at `tick`.
            const glm::dvec3 centre = BoxCentreOf(pose, size, vs);
            const std::array<glm::dvec3, 3> axes = AxesOf(pose.rotation);
            const glm::dvec3 half = glm::dvec3(size) * (0.5 * vs);
            const scene::Scene& scene = simulation.Scene();
            for (const scene::Entity other : MechanismsInOrder(scene)) {
                const scene::Mechanism* m = scene.TryGet<scene::Mechanism>(other);
                if (!m || m->track.count == 0 || !m->swept.Overlaps(box)) continue;
                const MechanismPose mp = PoseAt(*m, PositionAt(*m, tick), vs);
                const glm::vec3 bMin(m->desc.blockBox.min - m->modelOffset);
                const glm::vec3 bMax(m->desc.blockBox.max - m->modelOffset);
                const glm::dvec3 mc = PosedPoint(mp, (bMin + bMax) * 0.5f, vs);
                const glm::dvec3 mh = glm::dvec3(bMax - bMin) * (0.5 * vs);
                if (BoxesOverlap(centre, axes, half, mc, AxesOf(mp.rotation), mh)) return false;
            }

            // (c) Other resting bodies (Layer::Dynamic; moving Rubble is skipped).
            std::vector<scene::Entity> bodies;
            scene.QueryBodies(box, bodies);
            const physics::PhysicsWorld& physics = const_cast<Simulation&>(simulation).Physics();
            for (const scene::Entity other : bodies) {
                if (other == entity) continue;
                const scene::RigidBody* body = scene.TryGet<scene::RigidBody>(other);
                if (!body || physics.GetLayer(body->body) != physics::Layer::Dynamic) continue;
                if (physics.GetBounds(body->body).Overlaps(box)) return false;
            }

            // (d) Player capsules.
            for (const glm::vec3& feet : env.playerFeet)
                if (PlayerBounds(feet, env.tuning).Overlaps(box)) return false;
            return true;
        }
    }

    int8_t HoldPitch(int16_t pitch) {
        return static_cast<int8_t>(std::clamp<int32_t>(pitch, -kHoldPitchLimit, kHoldPitchLimit) / 256);
    }

    uint32_t HoldMaxVoxels(float voxelSize) {
        const double vs = static_cast<double>(voxelSize);
        return static_cast<uint32_t>(std::llround(static_cast<double>(kHoldMaxVolume) / (vs * vs * vs)));
    }

    uint16_t RelYawAtPickup(const glm::quat& rotation, uint16_t holderYaw) {
        const glm::vec3 f = rotation * glm::vec3(0.0f, 0.0f, -1.0f);
        float objectYaw = 0.0f;
        if (f.x * f.x + f.z * f.z < 0.25f) {
            const glm::vec3 r = rotation * glm::vec3(1.0f, 0.0f, 0.0f);
            objectYaw = std::atan2(-r.z, r.x);
        } else {
            objectYaw = std::atan2(-f.x, -f.z);
        }
        return static_cast<uint16_t>(QuantizeYaw(objectYaw) - holderYaw);
    }

    glm::dvec3 HoldAnchor(const glm::vec3& feet, const PlayerTuning& tuning) {
        return glm::dvec3(feet) + glm::dvec3(0.0, static_cast<double>(tuning.eyeHeight - kHoldDrop), 0.0);
    }

    bool IsHoldableShape(const Simulation& simulation, scene::Entity entity) {
        const scene::Scene& scene = simulation.Scene();
        if (!scene.IsValid(entity)) return false;
        const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(entity);
        if (!voxels || !voxels->model || !scene.TryGet<scene::RigidBody>(entity)) return false;
        if (scene.TryGet<scene::Mechanism>(entity) || scene.TryGet<scene::Held>(entity)) return false;
        const float vs = simulation.World().VoxelSize();
        const glm::ivec3 s = voxels->model->Volume().Size();
        for (int a = 0; a < 3; ++a)
            if (static_cast<float>(s[a]) * vs > kHoldMaxExtent) return false;
        const uint32_t n = voxels->model->SolidCount();
        return n >= scene.Config().debrisBodyVoxels && n <= HoldMaxVoxels(vs);
    }

    bool IsHoldable(const Simulation& simulation, scene::Entity entity, bool replicatedOnly) {
        if (!IsHoldableShape(simulation, entity)) return false;
        const scene::Scene& scene = simulation.Scene();
        const scene::RigidBody* rigid = scene.TryGet<scene::RigidBody>(entity);
        if (!rigid || rigid->motion != physics::MotionType::Dynamic) return false;
        if (replicatedOnly) {
            const scene::NetId* id = scene.TryGet<scene::NetId>(entity);
            if (!id || id->value == 0) return false;
        }
        return true;
    }

    bool PickupPathClear(const Simulation& simulation, scene::Entity entity, const glm::dvec3& anchor) {
        MechanismPose pose;
        glm::vec3 size;
        if (!BodyBox(simulation.Scene(), entity, pose, size)) return false;
        const float vs = simulation.World().VoxelSize();
        const glm::dvec3 centre = BoxCentreOf(pose, size, vs);
        const double maxT = glm::length(centre - anchor);
        if (maxT <= 0.0) return true;
        const glm::dvec3 dir = (centre - anchor) / maxT;
        return SweepSphere(simulation, anchor, dir, SweepRadius(size, vs), maxT, simulation.Tick()) >= maxT;
    }

    HeldPose HeldPoseAt(const Simulation& simulation, scene::Entity held, const glm::vec3& feet, uint16_t yaw, int8_t holdPitch,
                        const HoldEnv& env, uint32_t tick) {
        HeldPose out;
        const scene::Scene& scene = simulation.Scene();
        const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(held);
        if (!voxels || !voxels->model) return out;
        const float vs = simulation.World().VoxelSize();
        const glm::vec3 size(voxels->model->Volume().Size());
        const scene::Held* h = scene.TryGet<scene::Held>(held);
        const uint16_t relYaw = h ? h->relYaw : uint16_t{0};

        const glm::dvec3 a = HoldAnchor(feet, env.tuning);
        const glm::dvec3 dir(LookDirection(yaw, static_cast<int16_t>(holdPitch * 256)));
        const double halfDiagonal = 0.5 * static_cast<double>(glm::length(size)) * static_cast<double>(vs);
        const double dMin = halfDiagonal + static_cast<double>(kHoldClearance);
        const double t = SweepSphere(simulation, a, dir, SweepRadius(size, vs), static_cast<double>(kHoldDistance), tick);
        const double d = std::clamp(t, dMin, static_cast<double>(kHoldDistance));
        out.pinned = t < dMin;
        out.distance = static_cast<float>(d);

        glm::dvec3 centre = a + dir * d;
        centre.x = std::clamp(centre.x, static_cast<double>(env.area.min.x), static_cast<double>(env.area.max.x));
        centre.z = std::clamp(centre.z, static_cast<double>(env.area.min.y), static_cast<double>(env.area.max.y));
        const glm::quat rot = glm::angleAxis(YawRadians(static_cast<uint16_t>(yaw + relYaw)), glm::vec3(0.0f, 1.0f, 0.0f));
        out.pose.rotation = rot;
        out.pose.position = centre - glm::dvec3(rot * (size * (0.5f * vs)));
        return out;
    }

    UseResult BeginHold(Simulation& simulation, scene::Entity entity, uint8_t holder, uint16_t yaw, uint32_t tick,
                        uint32_t lastChangeTick, bool anyChange, bool stoodOn, bool replicated, HoldChange& out) {
        if (anyChange && tick - lastChangeTick < kHoldBusyTicks) return UseResult::Busy;
        if (stoodOn) return UseResult::Busy;
        scene::Scene& scene = simulation.Scene();
        if (!IsHoldable(simulation, entity, replicated))
            return scene.IsValid(entity) && scene.TryGet<scene::Held>(entity) ? UseResult::Busy : UseResult::NoTarget;
        const scene::Transform* transform = scene.TryGet<scene::Transform>(entity);
        const scene::RigidBody* rigid = scene.TryGet<scene::RigidBody>(entity);
        const uint16_t relYaw = RelYawAtPickup(transform ? transform->current.rotation : glm::quat(1.0f, 0.0f, 0.0f, 0.0f), yaw);
        scene.Registry().emplace_or_replace<scene::Held>(entity, scene::Held{holder, relYaw, tick, 0});
        physics::PhysicsWorld& physics = simulation.Physics();
        physics.SetMotionType(rigid->body, physics::MotionType::Kinematic);   // RigidBody::motion stays Dynamic: the motion after the release
        physics.SetLayer(rigid->body, physics::Layer::Held);
        physics.SetVelocity(rigid->body, glm::vec3(0.0f), glm::vec3(0.0f));
        out = HoldChange{};
        out.entity = entity;
        out.hold = true;
        out.holder = holder;
        out.relYaw = relYaw;
        out.tick = tick;
        return UseResult::None;
    }

    bool StepHeld(Simulation& simulation, scene::Entity entity, const PlayerState& holder, uint16_t yaw, int8_t holdPitch,
                  const HoldEnv& env, uint32_t tick, bool authority) {
        scene::Scene& scene = simulation.Scene();
        if (!scene.IsValid(entity)) return false;
        scene::Held* h = scene.TryGet<scene::Held>(entity);
        const scene::RigidBody* rigid = scene.TryGet<scene::RigidBody>(entity);
        if (!h || !rigid) return false;
        const HeldPose p = HeldPoseAt(simulation, entity, holder.position, yaw, holdPitch, env, tick);
        simulation.Physics().MoveKinematic(rigid->body, glm::vec3(p.pose.position), p.pose.rotation, simulation.Config().fixedTimeStep);
        if (!authority) return false;
        h = scene.TryGet<scene::Held>(entity);
        h->pinnedTicks = p.pinned ? static_cast<uint16_t>(std::min<uint32_t>(h->pinnedTicks + 1u, 65535u)) : uint16_t{0};
        return h->pinnedTicks >= kHoldPinnedTicks;
    }

    void EndHold(Simulation& simulation, scene::Entity entity, HoldEnd reason, const MechanismPose& pose, const glm::vec3& velocity,
                 const glm::dvec3* anchor, const HoldEnv& env, uint32_t tick, bool replicated, HoldChange& out) {
        scene::Scene& scene = simulation.Scene();
        out = HoldChange{};
        out.entity = entity;
        if (!scene.IsValid(entity)) return;
        const scene::Held* h = scene.TryGet<scene::Held>(entity);
        const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(entity);
        const scene::RigidBody* rigid = scene.TryGet<scene::RigidBody>(entity);
        if (h) {
            out.holder = h->holder;
            out.relYaw = h->relYaw;
        }
        const float vs = simulation.World().VoxelSize();

        // 1. The fix-up: step back toward the anchor, one voxel at a time, until the box is clear.
        MechanismPose at = pose;
        if (anchor && voxels && voxels->model) {
            const glm::vec3 size(voxels->model->Volume().Size());
            if (!ReleaseClear(simulation, entity, pose, size, env, tick)) {
                const glm::dvec3 c = BoxCentreOf(pose, size, vs);
                const double d = glm::length(c - *anchor);
                const glm::dvec3 u = d > 0.0 ? (c - *anchor) / d : glm::dvec3(0.0);
                const glm::dvec3 offset = c - pose.position;
                for (int step = 1;; ++step) {
                    const double dd = d - step * static_cast<double>(vs);
                    if (dd < static_cast<double>(kHoldReleaseMin)) break;
                    MechanismPose candidate = pose;
                    candidate.position = *anchor + u * dd - offset;
                    if (ReleaseClear(simulation, entity, candidate, size, env, tick)) {
                        at = candidate;
                        break;
                    }
                }
            }
        }

        // 2. The body: no longer held, dynamic again at the pose with the quantised velocity.
        scene.Registry().remove<scene::Held>(entity);
        const glm::vec3 vq = QuantizeVelocity(velocity);
        scene.SetTransform(entity, glm::vec3(at.position), at.rotation, true);
        if (rigid) {
            physics::PhysicsWorld& physics = simulation.Physics();
            physics.SetMotionType(rigid->body, physics::MotionType::Dynamic);
            physics.SetVelocity(rigid->body, vq, glm::vec3(0.0f));
            physics.SetLayer(rigid->body, replicated ? physics::Layer::Rubble : physics::Layer::Dynamic);
            physics.Activate(rigid->body);
        }

        // 3. The EntityRelease.
        out.hold = false;
        out.tick = tick;
        out.reason = static_cast<uint8_t>(reason);
        out.position = glm::vec3(at.position);
        out.rotation = at.rotation;
        out.velocity = vq;
    }

    glm::vec3 ReleaseVelocity(HoldEnd reason, const glm::vec3& holderVelocity, uint16_t yaw, int16_t pitch) {
        if (reason == HoldEnd::Throw) return holderVelocity + LookDirection(yaw, pitch) * kThrowSpeed;
        return holderVelocity;
    }

    void ApplyHoldChange(Simulation& simulation, scene::Entity entity, const HoldChange& change) {
        scene::Scene& scene = simulation.Scene();
        if (!scene.IsValid(entity)) return;
        const scene::RigidBody* rigid = scene.TryGet<scene::RigidBody>(entity);
        physics::PhysicsWorld& physics = simulation.Physics();
        if (change.hold) {
            scene.Registry().emplace_or_replace<scene::Held>(entity, scene::Held{change.holder, change.relYaw, change.tick, 0});
            if (rigid) {
                physics.SetLayer(rigid->body, physics::Layer::Held);
                physics.SetVelocity(rigid->body, glm::vec3(0.0f), glm::vec3(0.0f));
            }
            return;
        }
        scene.Registry().remove<scene::Held>(entity);
        scene.SetTransform(entity, change.position, change.rotation, true);
        if (rigid) physics.SetVelocity(rigid->body, glm::vec3(0.0f), glm::vec3(0.0f));
    }

    std::string HoldPrompt() { return "[E] Drop   [LMB] Throw"; }

    glm::vec3 QuantizeVelocity(const glm::vec3& velocity) {
        constexpr float kQuanta = 256.0f;   // 1/256 m/s, the wire's unit
        glm::vec3 q;
        for (int a = 0; a < 3; ++a) {
            const float x = std::isfinite(velocity[a]) ? std::clamp(velocity[a] * kQuanta, -32767.0f, 32767.0f) : 0.0f;
            q[a] = static_cast<float>(static_cast<int16_t>(std::lround(x))) / kQuanta;
        }
        return q;
    }
}
