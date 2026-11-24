module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <string>
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
                    if (other == entity || scene.TryGet<scene::Mechanism>(other)) continue;
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

    bool FindUseTarget(const Simulation& /*simulation*/, const glm::dvec3& /*eye*/, const glm::vec3& /*dir*/, uint32_t /*tick*/,
                       UseTarget& /*out*/) {
        // [mech-1] card 4 fills this
        return false;
    }

    bool RayMeetsUseBox(const Simulation& /*simulation*/, scene::Entity /*entity*/, const glm::dvec3& /*eye*/, const glm::vec3& /*dir*/,
                        uint32_t /*tick*/, float /*inflate*/) {
        // [mech-1] card 4 fills this
        return false;
    }

    UseResult ApplyUse(Simulation& /*simulation*/, const UseTarget& /*target*/, const glm::dvec3& /*eye*/, uint32_t /*tick*/,
                       uint32_t /*leadTicks*/, std::vector<MechanismChange>& /*changes*/) {
        // [mech-1] card 4 fills this
        return UseResult::NoTarget;
    }

    std::string UsePrompt(const Simulation& /*simulation*/, const UseTarget& /*target*/, uint32_t /*tick*/, bool& greyed) {
        // [mech-1] card 4 fills this
        greyed = false;
        return {};
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
}
