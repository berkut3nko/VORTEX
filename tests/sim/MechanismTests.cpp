// Doors, gates and levers in the simulation ([mech-1], docs/design/mechanisms.md "Mechanisms",
// "Interaction: the use key", "Destruction interplay"): the integer timetable, the layer rule,
// posing and stepping, use targets, links and the damage rule. GPU-free; the session half is
// tests/net/MechanismNetTests.cpp.

#include <gtest/gtest.h>

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstdio>
#include <memory>
#include <span>
#include <string>
#include <vector>

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.physics;
import vortex.scene;
import vortex.sim;

using namespace vortex;
using scene::Entity;
using scene::kNoEntity;

namespace {
    constexpr float kVoxel = 0.125f;
    constexpr int kGroundVoxels = 96;                 // flat ground: top at y = 12 m
    constexpr float kGround = kGroundVoxels * kVoxel;

    // Model sizes in voxels of 0.125 m.
    constexpr glm::ivec3 kDoorSize{8, 16, 3};         // 1 x 2 x 0.375 m, hinge at x = 0
    constexpr glm::ivec3 kGateSize{16, 24, 1};        // 2 x 3 x 0.125 m, slides up 2.5 m
    constexpr glm::ivec3 kLeverSize{2, 6, 2};         // 24 voxels, turns about +x at its foot

    sim::SimulationConfig YardConfig() {
        sim::SimulationConfig config;
        config.chunkCapacity = 1024;
        config.maxParticles = 0;
        config.jobThreads = 2;
        config.world.voxelSize = kVoxel;
        config.world.windowSize = {8, 6, 8};
        config.physics.maxBodies = 1024;
        config.physics.maxBodyPairs = 2048;
        config.physics.maxContactConstraints = 2048;
        config.physics.threads = 1;
        return config;
    }

    std::shared_ptr<voxel::IWorldGenerator> YardGround() {
        voxel::MaterialLibrary scratch;
        const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(scratch);
        return voxel::CreateFlatGenerator(kGroundVoxels, ids.grass, ids.stone);
    }

    /// A simulation on flat ground with its window loaded around the origin.
    struct YardField {
        explicit YardField(const sim::SimulationConfig& config = YardConfig()) : simulation(config) {
            initialized = simulation.Initialize(YardGround());
            if (initialized) simulation.World().LoadBlocking(glm::dvec3(0.0, kGround, 0.0));
        }
        YardField(const YardField&) = delete;
        YardField& operator=(const YardField&) = delete;

        voxel::World& World() { return simulation.World(); }
        scene::Scene& Scene() { return simulation.Scene(); }
        const voxel::WorldMaterials& Ids() const { return simulation.WorldMaterialIds(); }
        float Dt() const { return simulation.Config().fixedTimeStep; }

        voxel::WorldHashResult Hash() const {
            const voxel::World& world = simulation.World();
            return voxel::WorldHash(world, world.WindowMinChunk(), world.WindowMinChunk() + world.WindowSize());
        }
        void Apply(const sim::WorldOp& op) { EXPECT_TRUE(sim::ApplyOp(simulation.World(), op).Complete()); }

        scene::Mechanism* MechanismOf(Entity e) { return simulation.Scene().TryGet<scene::Mechanism>(e); }
        physics::Layer LayerOf(Entity e) {
            const scene::RigidBody* body = simulation.Scene().TryGet<scene::RigidBody>(e);
            return body ? simulation.Physics().GetLayer(body->body) : physics::Layer::Static;
        }
        uint32_t SolidOf(Entity e) {
            const scene::VoxelBody* voxels = simulation.Scene().TryGet<scene::VoxelBody>(e);
            return voxels && voxels->model ? voxels->model->SolidCount() : 0u;
        }

        /// A player's character at `feet`.
        physics::CharacterId MakeCharacter(const glm::vec3& feet, const sim::PlayerTuning& tuning) {
            return simulation.Physics().CreateCharacter(sim::PlayerCharacterDesc(feet, tuning));
        }

        sim::Simulation simulation;
        bool initialized = false;
    };

    /// Everything, for PoseMechanisms calls that should reach every mechanism.
    math::AABB YardEverywhere() { return math::AABB(glm::vec3(-1000.0f), glm::vec3(1000.0f)); }

    std::shared_ptr<voxel::VoxelModel> MakeSolidModel(const glm::ivec3& size, uint8_t material, const char* name) {
        voxel::VoxelVolume volume(size);
        volume.FillBox(glm::ivec3(0), size, material);
        return std::make_shared<voxel::VoxelModel>(std::move(volume), voxel::kWorldPalette, name);
    }

    /// A 3-voxel-thick door hinged at x = 0 about +y (pivot in the middle of its thickness), both
    /// ways (-1024..1024, 72 ticks for the whole range) or one way (0..1024, 36 ticks). The hinge box
    /// is the middle metre of the hinge column.
    scene::MechanismDesc MakeDoorDesc(bool oneWay = false) {
        scene::MechanismDesc d;
        d.kind = scene::MechanismKind::Door;
        d.flags = scene::MechanismDesc_Usable | (oneWay ? scene::MechanismDesc_OneWay : 0);
        d.pivotQ = {0, 0, kDoorSize.z * 8};
        d.axis = 2;
        d.minPos = oneWay ? int16_t{0} : int16_t{-1024};
        d.maxPos = 1024;
        d.openTicks = oneWay ? 36 : 72;
        d.closeTicks = oneWay ? 36 : 72;
        d.useBox = {glm::ivec3(0), kDoorSize};
        d.blockBox = {glm::ivec3(0), kDoorSize};
        d.hingeBox = {glm::ivec3(0, 4, 0), glm::ivec3(1, 12, kDoorSize.z)};
        d.pushAxis = 4;
        return d;
    }

    /// A portcullis that slides up 20 voxels (320 sixteenths): raised in 180 ticks, dropped in 45.
    scene::MechanismDesc MakeGateDesc() {
        scene::MechanismDesc d;
        d.kind = scene::MechanismKind::Portcullis;
        d.flags = 0;
        d.axis = 2;
        d.minPos = 0;
        d.maxPos = 320;
        d.openTicks = 180;
        d.closeTicks = 45;
        d.useBox = {glm::ivec3(0), kGateSize};
        d.blockBox = {glm::ivec3(0), kGateSize};
        d.hingeBox = {glm::ivec3(0, kGateSize.y - 2, 0), kGateSize};
        d.pushAxis = 4;
        return d;
    }

    /// A lever that turns 45 deg (512) about +x at its foot in 12 ticks.
    scene::MechanismDesc MakeLeverDesc() {
        scene::MechanismDesc d;
        d.kind = scene::MechanismKind::Lever;
        d.flags = scene::MechanismDesc_Usable;
        d.pivotQ = {kLeverSize.x * 8, 0, kLeverSize.z * 8};
        d.axis = 0;
        d.minPos = 0;
        d.maxPos = 512;
        d.openTicks = 12;
        d.closeTicks = 12;
        d.useBox = {glm::ivec3(0), kLeverSize};
        d.blockBox = {glm::ivec3(0), kLeverSize};
        d.hingeBox = {glm::ivec3(0), glm::ivec3(kLeverSize.x, 2, kLeverSize.z)};
        d.pushAxis = 4;
        return d;
    }

    /// A kinematic voxel object made a mechanism (kNoEntity if either step failed).
    Entity MakeYardMechanism(YardField& f, const std::shared_ptr<voxel::VoxelModel>& model, const scene::MechanismDesc& desc,
                             const glm::ivec3& spawnVoxel, int16_t initialPos = 0, uint32_t tick = 0, uint8_t quarterTurns = 0) {
        const glm::quat r0 = glm::angleAxis(static_cast<float>(quarterTurns) * 1.5707963f, glm::vec3(0.0f, 1.0f, 0.0f));
        const Entity e = f.Scene().CreateVoxelObject(
            {.model = model, .position = glm::vec3(f.World().VoxelToWorld(spawnVoxel)), .rotation = r0, .motion = physics::MotionType::Kinematic});
        if (e == kNoEntity) return kNoEntity;
        if (!sim::MakeMechanism(f.simulation, e, desc, spawnVoxel, quarterTurns, initialPos, tick)) return kNoEntity;
        return e;
    }

    Entity MakeYardDoor(YardField& f, const glm::ivec3& spawnVoxel, bool oneWay = false) {
        return MakeYardMechanism(f, MakeSolidModel(kDoorSize, f.Ids().planks, "door"), MakeDoorDesc(oneWay), spawnVoxel);
    }
    Entity MakeYardGate(YardField& f, const glm::ivec3& spawnVoxel, int16_t initialPos = 0) {
        return MakeYardMechanism(f, MakeSolidModel(kGateSize, f.Ids().wood, "gate"), MakeGateDesc(), spawnVoxel, initialPos);
    }
    Entity MakeYardLever(YardField& f, const glm::ivec3& spawnVoxel, int16_t initialPos = 0) {
        return MakeYardMechanism(f, MakeSolidModel(kLeverSize, f.Ids().wood, "lever"), MakeLeverDesc(), spawnVoxel, initialPos);
    }

    /// A mechanism record without a simulation: only the timetable is read.
    scene::Mechanism MakeTimetable(const scene::MechanismDesc& desc, const scene::MechanismSegment& first) {
        scene::Mechanism m;
        m.desc = desc;
        sim::PushSegment(m.track, first);
        return m;
    }

    /// The timetable's rule over an unbounded list of segments (the reference of the ring).
    int16_t YardReferencePosition(const scene::MechanismDesc& desc, const std::vector<scene::MechanismSegment>& list, uint32_t tick) {
        const scene::MechanismSegment* s = nullptr;
        for (auto it = list.rbegin(); it != list.rend(); ++it)
            if (it->startTick <= tick) {
                s = &*it;
                break;
            }
        if (!s) return list.front().startPos;
        const uint32_t k = tick - s->startTick;
        if (k >= s->durationTicks) return s->targetPos;
        const uint32_t e = sim::Ease(sim::EaseTableFor(desc, *s), k, s->durationTicks);
        return static_cast<int16_t>(s->startPos + (static_cast<int64_t>(s->targetPos) - s->startPos) * e / 65536);
    }

    /// World position of the centre of original model voxel `v` at travel position `p`.
    glm::dvec3 YardVoxelCentre(const scene::Mechanism& m, const glm::vec3& v, int16_t p) {
        const sim::MechanismPose pose = sim::PoseAt(m, p, kVoxel);
        const glm::vec3 local = (v - glm::vec3(m.modelOffset) + glm::vec3(0.5f)) * kVoxel;
        return pose.position + glm::dvec3(pose.rotation * local);
    }

    /// The SphereQ of a hit of `radius` metres at `centre` (world metres).
    voxel::SphereQ YardSphere(const glm::dvec3& centre, double radius) {
        return voxel::QuantizeSphere(centre / static_cast<double>(kVoxel), radius / static_cast<double>(kVoxel));
    }
}

// =================================================================================================
//  The timetable
// =================================================================================================

TEST(Mechanism, PositionIsAFunctionOfTick) {
    const scene::MechanismDesc door = MakeDoorDesc();
    {
        const scene::Mechanism m = MakeTimetable(door, {100, 0, 1024, 36, 0});
        const std::array<uint32_t, 6> ks{1, 9, 18, 27, 35, 36};
        const std::array<int16_t, 6> want{56, 448, 768, 960, 1023, 1024};
        for (size_t i = 0; i < ks.size(); ++i) EXPECT_EQ(sim::PositionAt(m, 100 + ks[i]), want[i]) << "k = " << ks[i];
        for (uint32_t k = 0; k < 36; ++k) {
            const uint32_t e = sim::Ease(sim::kEaseOut, k, 36);
            EXPECT_EQ(sim::PositionAt(m, 100 + k), static_cast<int16_t>(0 + (1024 - 0) * static_cast<int64_t>(e) / 65536)) << "k = " << k;
        }
        for (uint32_t t = 136; t < 400; ++t) EXPECT_EQ(sim::PositionAt(m, t), 1024) << t;
        EXPECT_EQ(sim::PositionAt(m, 50), 0);
    }
    {
        const scene::Mechanism still = MakeTimetable(door, {10, 300, 300, 0, 0});
        for (uint32_t t = 0; t < 200; ++t) EXPECT_EQ(sim::PositionAt(still, t), 300) << t;
    }
    {
        const scene::Mechanism m = MakeTimetable(door, {100, 1024, 0, 36, 0});
        EXPECT_EQ(sim::PositionAt(m, 101), 968);
        EXPECT_EQ(sim::PositionAt(m, 118), 256);
        EXPECT_EQ(sim::PositionAt(m, 135), 1);
        EXPECT_EQ(sim::PositionAt(m, 136), 0);
    }
    {
        const scene::Mechanism m = MakeTimetable(MakeGateDesc(), {100, 0, 320, 180, 0});
        const std::array<uint32_t, 6> ks{1, 45, 90, 135, 179, 180};
        const std::array<int16_t, 6> want{0, 50, 160, 270, 319, 320};
        for (size_t i = 0; i < ks.size(); ++i) EXPECT_EQ(sim::PositionAt(m, 100 + ks[i]), want[i]) << "k = " << ks[i];
        for (uint32_t k = 0; k < 180; ++k) {
            const uint32_t e = sim::Ease(sim::kEaseSmooth, k, 180);
            EXPECT_EQ(sim::PositionAt(m, 100 + k), static_cast<int16_t>(320 * static_cast<int64_t>(e) / 65536)) << "k = " << k;
        }
    }
    {
        const scene::Mechanism m = MakeTimetable(MakeLeverDesc(), {100, 0, 512, 12, 0});
        EXPECT_EQ(sim::PositionAt(m, 101), 81);
        EXPECT_EQ(sim::PositionAt(m, 106), 384);
        EXPECT_EQ(sim::PositionAt(m, 111), 508);
        EXPECT_EQ(sim::PositionAt(m, 112), 512);
    }
    // The tables themselves (exact integer formulas).
    EXPECT_EQ(sim::kEaseOut[0], 0u);
    EXPECT_EQ(sim::kEaseOut[1], 2032u);
    EXPECT_EQ(sim::kEaseOut[32], 49152u);
    EXPECT_EQ(sim::kEaseOut[63], 65520u);
    EXPECT_EQ(sim::kEaseOut[64], 65536u);
    EXPECT_EQ(sim::kEaseSmooth[0], 0u);
    EXPECT_EQ(sim::kEaseSmooth[1], 47u);
    EXPECT_EQ(sim::kEaseSmooth[32], 32768u);
    EXPECT_EQ(sim::kEaseSmooth[63], 65488u);
    EXPECT_EQ(sim::kEaseSmooth[64], 65536u);
    for (size_t i = 1; i < 65; ++i) {
        EXPECT_GE(sim::kEaseOut[i], sim::kEaseOut[i - 1]) << i;
        EXPECT_GE(sim::kEaseSmooth[i], sim::kEaseSmooth[i - 1]) << i;
    }
}

TEST(Mechanism, DurationIsIntegerExact) {
    const scene::MechanismDesc door = MakeDoorDesc();
    EXPECT_EQ(sim::SegmentDuration(0, 1024, door), 36);
    EXPECT_EQ(sim::SegmentDuration(1024, 0, door), 36);
    EXPECT_EQ(sim::SegmentDuration(0, -1024, door), 36);
    EXPECT_EQ(sim::SegmentDuration(300, 0, door), 11);       // ceil(300 x 72 / 2048) = ceil(10.55)
    EXPECT_EQ(sim::SegmentDuration(500, 500, door), 0);
    EXPECT_EQ(sim::SegmentDuration(0, 512, MakeLeverDesc()), 12);
    EXPECT_EQ(sim::SegmentDuration(0, 320, MakeGateDesc()), 180);
    EXPECT_EQ(sim::SegmentDuration(320, 0, MakeGateDesc()), 45);
}

TEST(Mechanism, ReversalKeepsPositionContinuous) {
    const scene::MechanismDesc door = MakeDoorDesc();
    const scene::Mechanism opening = MakeTimetable(door, {100, 0, 1024, 36, 0});
    for (uint32_t start = 101; start <= 135; ++start) {
        scene::Mechanism m = opening;
        const int16_t p = sim::PositionAt(m, start);
        const uint16_t d = sim::SegmentDuration(p, 0, door);
        sim::PushSegment(m.track, {start, p, 0, d, 0});
        EXPECT_EQ(sim::PositionAt(m, start), sim::PositionAt(opening, start)) << "no jump at " << start;
        EXPECT_EQ(sim::PositionAt(m, start - 1), sim::PositionAt(opening, start - 1));
        ASSERT_GT(d, 0) << start;
        EXPECT_NE(sim::PositionAt(m, start + d - 1), 0) << start;
        EXPECT_EQ(sim::PositionAt(m, start + d), 0) << "reaches 0 exactly at its arrival, " << start;
        EXPECT_EQ(sim::ArrivalTick(m.track), start + d);
    }
}

TEST(Mechanism, RingHoldsTheLeadAndTheStaleWindow) {
    const scene::MechanismDesc door = MakeDoorDesc();
    const scene::MechanismSegment spawn{0, 0, 0, 0, 0};
    scene::Mechanism m = MakeTimetable(door, spawn);
    std::vector<scene::MechanismSegment> list{spawn};
    bool open = false;
    for (uint32_t decision = 100; decision < 100 + 12 * 40; decision += sim::kMechanismBusyTicks) {
        const uint32_t start = decision + sim::kMechanismLeadMax;
        const int16_t p = YardReferencePosition(door, list, start);
        open = !open;
        const int16_t target = open ? int16_t{1024} : int16_t{0};
        const scene::MechanismSegment s{start, p, target, sim::SegmentDuration(p, target, door), 0};
        sim::PushSegment(m.track, s);
        list.push_back(s);
        EXPECT_EQ(m.track.Newest(), s);
        for (uint32_t t = decision - sim::kUseStaleTicks; t <= decision + sim::kMechanismLeadMax; ++t)
            ASSERT_EQ(sim::PositionAt(m, t), YardReferencePosition(door, list, t)) << "decision " << decision << ", tick " << t;
    }
    EXPECT_EQ(static_cast<uint32_t>(m.track.count), scene::kMechanismSegments);
}

// =================================================================================================
//  The layer rule, posing and stepping
// =================================================================================================

TEST(Mechanism, LayerFollowsMotion) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    const Entity door = MakeYardDoor(f, {0, kGroundVoxels, 0});
    const Entity gate = MakeYardGate(f, {-40, kGroundVoxels, 0});
    const Entity lever = MakeYardLever(f, {40, kGroundVoxels, 0});
    ASSERT_NE(door, kNoEntity);
    ASSERT_NE(gate, kNoEntity);
    ASSERT_NE(lever, kNoEntity);
    const math::AABB all = YardEverywhere();

    sim::PoseMechanisms(f.simulation, 1, all);
    EXPECT_EQ(f.LayerOf(door), physics::Layer::Dynamic);
    EXPECT_EQ(f.LayerOf(gate), physics::Layer::Dynamic);
    EXPECT_EQ(f.LayerOf(lever), physics::Layer::Dynamic);

    // Door open 100..136, closed 200..236; gate raised 100..280, dropped 300..345; lever 100..112.
    sim::PushSegment(f.MechanismOf(door)->track, {100, 0, 1024, 36, 0});
    sim::PushSegment(f.MechanismOf(door)->track, {200, 1024, 0, 36, 0});
    sim::PushSegment(f.MechanismOf(gate)->track, {100, 0, 320, 180, 0});
    sim::PushSegment(f.MechanismOf(gate)->track, {300, 320, 0, 45, 0});
    sim::PushSegment(f.MechanismOf(lever)->track, {100, 0, 512, 12, 0});

    const auto doorMoving = [](uint32_t t) { return (t >= 100 && t < 136) || (t >= 200 && t < 236); };
    const auto gateDropping = [](uint32_t t) { return t >= 300 && t < 345; };
    for (uint32_t t = 90; t < 360; ++t) {
        sim::PoseMechanisms(f.simulation, t, all);
        EXPECT_EQ(f.LayerOf(door), doorMoving(t) ? physics::Layer::Rubble : physics::Layer::Dynamic) << "posed, tick " << t;
        EXPECT_EQ(f.LayerOf(gate), gateDropping(t) ? physics::Layer::Rubble : physics::Layer::Dynamic) << "posed, tick " << t;
        EXPECT_NE(f.LayerOf(lever), physics::Layer::Debris) << "posed, tick " << t;
        EXPECT_EQ(sim::MechanismLayer(*f.MechanismOf(door), t), f.LayerOf(door)) << t;

        sim::StepMechanisms(f.simulation, t, false);
        EXPECT_EQ(f.LayerOf(door), doorMoving(t + 1) ? physics::Layer::Rubble : physics::Layer::Dynamic) << "stepped, tick " << t;
        EXPECT_EQ(f.LayerOf(gate), gateDropping(t + 1) ? physics::Layer::Rubble : physics::Layer::Dynamic) << "stepped, tick " << t;
        EXPECT_NE(f.LayerOf(lever), physics::Layer::Debris) << "stepped, tick " << t;
    }
    // Arrival ticks are at rest.
    EXPECT_EQ(sim::MechanismLayer(*f.MechanismOf(door), 136), physics::Layer::Dynamic);
    EXPECT_EQ(sim::MechanismLayer(*f.MechanismOf(gate), 280), physics::Layer::Dynamic);
    EXPECT_EQ(sim::MechanismLayer(*f.MechanismOf(gate), 345), physics::Layer::Dynamic);
}

TEST(Mechanism, NotCountedAsAwake) {
    sim::SimulationConfig config = sim::ServerConfig(YardConfig());
    config.replication.maxAwakePieces = 1;    // one counted door would refuse every new piece
    YardField f(config);
    ASSERT_TRUE(f.initialized);
    ASSERT_TRUE(f.simulation.Config().replication.enabled);
    const Entity door = MakeYardDoor(f, {0, kGroundVoxels, 0});
    ASSERT_NE(door, kNoEntity);
    EXPECT_NE(sim::Replicate(f.simulation, door, false), 0u);

    const uint32_t start = f.simulation.Tick() + 2;
    sim::PushSegment(f.MechanismOf(door)->track, {start, 0, 1024, 36, 0});
    const std::array<glm::dvec3, 1> focus = {glm::dvec3(0.5, kGround, 0.5)};
    bool sawMoving = false;
    for (int i = 0; i < 40; ++i) {
        f.simulation.UpdateStreaming(focus);
        const uint32_t tick = f.simulation.Tick();
        const sim::MechanismStepResult r = sim::StepMechanisms(f.simulation, tick, true);
        f.simulation.StepPhysics();
        if (!sim::IsMoving(f.MechanismOf(door)->track, tick)) continue;
        sawMoving = true;
        EXPECT_EQ(r.moving, 1u) << tick;
        EXPECT_EQ(sim::CountPieces(f.simulation).awakeBodies, 0u) << tick;
        EXPECT_EQ(sim::CountPieces(f.simulation).live, 0u) << tick;
    }
    EXPECT_TRUE(sawMoving);
    sim::PushSegment(f.MechanismOf(door)->track, {f.simulation.Tick() + 1, 1024, 0, 36, 0});
    sim::StepMechanisms(f.simulation, f.simulation.Tick() + 1, true);
    EXPECT_EQ(sim::CountPieces(f.simulation).awakeBodies, 0u);
    EXPECT_EQ(sim::TakePieceSlot(f.simulation), sim::PieceSlot::Replicate);
}

namespace {
    /// A portcullis dropping (320 -> 0 from tick 100) onto a player standing in its passage at
    /// z = 0.1 m, optionally with terrain blocks on the +z and the -z side. Returns the feet after
    /// PushOutOfMechanisms at the arrival tick and whether it pushed.
    struct YardLanding {
        glm::vec3 feet{0.0f};
        bool pushed = false;
        bool ok = false;
    };
    YardLanding RunYardLanding(bool blockPlus, bool blockMinus) {
        YardLanding out;
        YardField f;
        if (!f.initialized) return out;
        const uint8_t stone = f.Ids().stone;
        if (blockPlus) f.Apply(sim::WorldOp::Box({-8, kGroundVoxels, 4}, {8, kGroundVoxels + 16, 8}, stone));
        if (blockMinus) f.Apply(sim::WorldOp::Box({-8, kGroundVoxels, -8}, {8, kGroundVoxels + 16, -4}, stone));
        const Entity gate = MakeYardGate(f, {-8, kGroundVoxels, 0}, 320);
        if (gate == kNoEntity) return out;
        const scene::MechanismDesc desc = MakeGateDesc();
        const uint16_t d = sim::SegmentDuration(320, 0, desc);
        sim::PushSegment(f.MechanismOf(gate)->track, {100, 320, 0, d, 0});
        const uint32_t arrival = 100 + d;

        sim::PlayerTuning tuning;
        sim::PlayerState state;
        state.position = {0.0f, kGround, 0.1f};
        state.flags = sim::PlayerFlag_OnGround;
        const physics::CharacterId character = f.MakeCharacter(state.position, tuning);
        sim::PoseMechanisms(f.simulation, arrival, sim::PlayerReach(state.position, tuning));
        out.pushed = sim::PushOutOfMechanisms(f.simulation, arrival, state, character, tuning);
        out.feet = state.position;
        out.ok = true;
        return out;
    }
}

TEST(Mechanism, LandingPushesThePlayerOut) {
    const sim::PlayerTuning tuning;
    const float mid = 0.5f * kVoxel;                      // the gate's mid-plane, z
    const float half = 0.5f * kVoxel;                     // its half thickness
    const float clear = half + tuning.radius + 0.02f;     // 2 cm clear of the block box

    const YardLanding open = RunYardLanding(false, false);
    ASSERT_TRUE(open.ok);
    EXPECT_TRUE(open.pushed);
    EXPECT_NEAR(open.feet.z, mid + clear, 1.0e-4f) << "to the side the feet were on (+z)";
    EXPECT_FLOAT_EQ(open.feet.x, 0.0f);
    EXPECT_FLOAT_EQ(open.feet.y, kGround);

    const YardLanding plusBlocked = RunYardLanding(true, false);
    ASSERT_TRUE(plusBlocked.ok);
    EXPECT_TRUE(plusBlocked.pushed);
    EXPECT_NEAR(plusBlocked.feet.z, mid - clear, 1.0e-4f) << "terrain on +z: to the other side";

    const YardLanding bothBlocked = RunYardLanding(true, true);
    ASSERT_TRUE(bothBlocked.ok);
    EXPECT_FALSE(bothBlocked.pushed);
    EXPECT_FLOAT_EQ(bothBlocked.feet.z, 0.1f) << "both sides terrain: no push";
}

TEST(Mechanism, StandingOnALoweredBridgeIsNotPushed) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    // A Portcullis-kind slide lying flat: 2 x 0.125 x 2 m, lowered (p 320 -> 0) long ago.
    scene::MechanismDesc desc = MakeGateDesc();
    const glm::ivec3 size{16, 1, 16};
    desc.useBox = {glm::ivec3(0), size};
    desc.blockBox = {glm::ivec3(0), size};
    desc.hingeBox = {glm::ivec3(0), glm::ivec3(16, 1, 2)};
    desc.pushAxis = 4;
    const Entity bridge = MakeYardMechanism(f, MakeSolidModel(size, f.Ids().planks, "bridge"), desc, {-8, kGroundVoxels + 8, -8}, 320);
    ASSERT_NE(bridge, kNoEntity);
    const uint16_t d = sim::SegmentDuration(320, 0, desc);
    sim::PushSegment(f.MechanismOf(bridge)->track, {10, 320, 0, d, 0});
    const uint32_t arrival = 10 + d;

    sim::PlayerTuning tuning;
    sim::PlayerState state;
    state.position = {0.0f, (kGroundVoxels + 9) * kVoxel, 0.0f};     // on the bridge's top face
    state.flags = sim::PlayerFlag_OnGround;
    const glm::vec3 before = state.position;
    const physics::CharacterId character = f.MakeCharacter(state.position, tuning);
    for (const uint32_t tick : {arrival, arrival + 1, arrival + 500}) {
        sim::PoseMechanisms(f.simulation, tick, sim::PlayerReach(state.position, tuning));
        EXPECT_FALSE(sim::PushOutOfMechanisms(f.simulation, tick, state, character, tuning)) << tick;
        EXPECT_EQ(state.position, before) << tick;
    }
}

TEST(Mechanism, LeaningOnAClosedDoorIsNotPushed) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    const Entity door = MakeYardDoor(f, {0, kGroundVoxels, 0});
    ASSERT_NE(door, kNoEntity);
    sim::PushSegment(f.MechanismOf(door)->track, {100, 0, 1024, 36, 0});
    sim::PushSegment(f.MechanismOf(door)->track, {150, 1024, 0, 36, 0});    // closed again at 186

    sim::PlayerTuning tuning;
    sim::PlayerState state;
    state.position = {0.5f, kGround, kDoorSize.z * kVoxel + tuning.radius};   // the capsule touches the leaf
    state.flags = sim::PlayerFlag_OnGround;
    const glm::vec3 before = state.position;
    const physics::CharacterId character = f.MakeCharacter(state.position, tuning);
    for (const uint32_t tick : {0u, 1u, 50u, 300u, 1000u}) {
        sim::PoseMechanisms(f.simulation, tick, sim::PlayerReach(state.position, tuning));
        EXPECT_FALSE(sim::PushOutOfMechanisms(f.simulation, tick, state, character, tuning)) << tick;
        EXPECT_EQ(state.position, before) << tick;
    }
}

TEST(Mechanism, ClosingGateCrushesACrateInsteadOfLaunchingIt) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    const Entity gate = MakeYardGate(f, {-8, kGroundVoxels, 0}, 320);
    ASSERT_NE(gate, kNoEntity);
    const Entity crate = f.Scene().CreateVoxelObject(
        {.model = sim::CrateModel(f.Ids(), sim::PlaygroundScale(kVoxel)), .position = {-0.4f, kGround, -0.3f}});
    ASSERT_NE(crate, kNoEntity);
    const std::array<glm::dvec3, 1> focus = {glm::dvec3(0.0, kGround, 0.0)};
    for (int i = 0; i < 30; ++i) f.simulation.Step(focus);     // the crate settles under the raised gate

    const scene::MechanismDesc desc = MakeGateDesc();
    const uint32_t start = f.simulation.Tick() + 5;
    const uint16_t d = sim::SegmentDuration(320, 0, desc);
    sim::PushSegment(f.MechanismOf(gate)->track, {start, 320, 0, d, 0});
    const uint32_t arrival = start + d;

    uint32_t crushed = 0;
    float fastest = 0.0f;
    for (int i = 0; i < 120; ++i) {
        f.simulation.UpdateStreaming(focus);
        const uint32_t tick = f.simulation.Tick();
        const sim::MechanismStepResult r = sim::StepMechanisms(f.simulation, tick, true);
        crushed += r.crushed;
        f.simulation.StepPhysics();
        if (const scene::RigidBody* body = f.Scene().TryGet<scene::RigidBody>(crate))
            fastest = std::max(fastest, glm::length(f.simulation.Physics().GetState(body->body).linearVelocity));
        if (tick + 1 >= arrival) EXPECT_FALSE(f.Scene().IsValid(crate)) << "the crate is gone by the arrival tick " << arrival;
    }
    EXPECT_LE(fastest, 5.0f) << "a launch is faster";
    EXPECT_EQ(crushed, 1u);
    EXPECT_FALSE(f.Scene().IsValid(crate));
    EXPECT_TRUE(f.Scene().IsValid(gate));
}

TEST(Mechanism, PlayerStepIsTheSameReplayed) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    // The door at x 0..1 m, z 0..0.375 m; the player walks through it along -z from z = 2 m. It
    // opens (away from the walker, to -z) from tick 5.
    const Entity door = MakeYardDoor(f, {0, kGroundVoxels, 0});
    ASSERT_NE(door, kNoEntity);
    sim::PushSegment(f.MechanismOf(door)->track, {5, 0, 1024, 36, 0});

    sim::PlayerTuning tuning;
    sim::PlayerState state;
    state.position = {0.5f, kGround, 2.0f};
    const physics::CharacterId character = f.MakeCharacter(state.position, tuning);
    const sim::PlayArea area;
    sim::PlayerInput walk;
    walk.moveZ = 127;
    walk.yaw = 0;                                         // looks along -z

    const auto stepOnce = [&](uint32_t tick) {
        const math::AABB reach = sim::PlayerReach(state.position, tuning);
        f.simulation.Terrain().FlushBox(reach);
        sim::PoseMechanisms(f.simulation, tick, reach);
        sim::PushOutOfMechanisms(f.simulation, tick, state, character, tuning);
        sim::StepPlayer(f.simulation.Physics(), f.simulation.Terrain(), character, state, walk, tuning, area, f.Dt());
        sim::StepMechanisms(f.simulation, tick, false);
        f.simulation.StepPhysics();
    };

    const std::array<glm::dvec3, 1> focus = {glm::dvec3(0.5, kGround, 1.0)};
    f.simulation.UpdateStreaming(focus);
    f.simulation.Terrain().Flush();
    std::vector<sim::PlayerState> recorded(60);
    for (uint32_t tick = 0; tick < 60; ++tick) {
        stepOnce(tick);
        recorded[tick] = state;
    }
    EXPECT_LT(recorded[59].position.z, 0.0f) << "walked through the open door";

    state = recorded[20];
    sim::RestorePlayer(f.simulation.Physics(), character, state);
    for (uint32_t tick = 21; tick < 60; ++tick) {
        stepOnce(tick);
        EXPECT_EQ(state, recorded[tick]) << "replayed tick " << tick;
    }
}

// =================================================================================================
//  Use targets, ApplyUse and links
// =================================================================================================

namespace {
    /// The door the Use tests look at: x 0..1 m, y 12..14 m, z 0..0.375 m, hinge at x = 0.
    constexpr glm::ivec3 kYardDoorSpawn{0, kGroundVoxels, 0};
    const glm::dvec3 kFrontEye{0.5, 13.0, 1.5};           // on +z, looking along -z
    const glm::vec3 kLookMinusZ{0.0f, 0.0f, -1.0f};
}

TEST(Use, TargetAtTheInputTick) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    const Entity door = MakeYardDoor(f, kYardDoorSpawn);
    ASSERT_NE(door, kNoEntity);
    sim::PushSegment(f.MechanismOf(door)->track, {100, 0, 1024, 36, 0});
    const scene::Mechanism& m = *f.MechanismOf(door);

    // Vertical rays from 1 m above the top of the door through a point of the leaf 0.6 m from the
    // hinge, as the leaf stands at tick 110 and at tick 120 (29 deg apart: 0.29 m off the other).
    const glm::vec3 onLeaf{4.8f, static_cast<float>(kDoorSize.y), 1.5f};
    const auto rayAt = [&](uint32_t tick) {
        const glm::dvec3 p = YardVoxelCentre(m, onLeaf - glm::vec3(0.5f), sim::PositionAt(m, tick));
        return glm::dvec3(p.x, kGround + 3.0, p.z);
    };
    const glm::vec3 down{0.0f, -1.0f, 0.0f};
    const glm::dvec3 eye110 = rayAt(110);
    const glm::dvec3 eye120 = rayAt(120);

    sim::UseTarget t;
    ASSERT_TRUE(sim::FindUseTarget(f.simulation, eye110, down, 110, t));
    EXPECT_EQ(t.entity, door);
    EXPECT_FALSE(t.info);
    EXPECT_FALSE(sim::FindUseTarget(f.simulation, eye110, down, 120, t));

    ASSERT_TRUE(sim::FindUseTarget(f.simulation, eye120, down, 120, t));
    EXPECT_EQ(t.entity, door);
    EXPECT_FALSE(sim::FindUseTarget(f.simulation, eye120, down, 110, t));
}

TEST(Use, TerrainOccludes) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    const Entity door = MakeYardDoor(f, kYardDoorSpawn);
    ASSERT_NE(door, kNoEntity);
    const glm::ivec3 wallMin{-8, kGroundVoxels, 6}, wallMax{16, kGroundVoxels + 24, 8};    // z 0.75..1 m
    f.Apply(sim::WorldOp::Box(wallMin, wallMax, f.Ids().stone));
    sim::UseTarget t;
    EXPECT_FALSE(sim::FindUseTarget(f.simulation, kFrontEye, kLookMinusZ, 10, t));
    f.Apply(sim::WorldOp::Box(wallMin, wallMax, 0));
    ASSERT_TRUE(sim::FindUseTarget(f.simulation, kFrontEye, kLookMinusZ, 10, t));
    EXPECT_EQ(t.entity, door);
}

TEST(Use, ReachLimit) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    const Entity door = MakeYardDoor(f, kYardDoorSpawn);
    ASSERT_NE(door, kNoEntity);
    const double face = kDoorSize.z * kVoxel + sim::kUseBoxInflate;    // the inflated use box's front face
    sim::UseTarget t;
    ASSERT_TRUE(sim::FindUseTarget(f.simulation, glm::dvec3(0.5, 13.0, face + 2.4), kLookMinusZ, 10, t));
    EXPECT_EQ(t.entity, door);
    EXPECT_NEAR(t.distance, 2.4f, 0.01f);
    EXPECT_FALSE(sim::FindUseTarget(f.simulation, glm::dvec3(0.5, 13.0, face + 2.7), kLookMinusZ, 10, t));
}

TEST(Use, NearestOfTwo) {
    {
        YardField f;
        ASSERT_TRUE(f.initialized);
        const Entity near = MakeYardDoor(f, kYardDoorSpawn);
        const Entity far = MakeYardDoor(f, kYardDoorSpawn - glm::ivec3(0, 0, 8));     // 1 m behind it
        ASSERT_NE(near, kNoEntity);
        ASSERT_NE(far, kNoEntity);
        sim::UseTarget t;
        ASSERT_TRUE(sim::FindUseTarget(f.simulation, kFrontEye, kLookMinusZ, 10, t));
        EXPECT_EQ(t.entity, near);
    }
    {
        // Side by side: a ray along their shared edge meets both inflated use boxes at the same distance.
        YardField f;
        ASSERT_TRUE(f.initialized);
        const Entity a = MakeYardDoor(f, kYardDoorSpawn);
        const Entity b = MakeYardDoor(f, kYardDoorSpawn + glm::ivec3(kDoorSize.x, 0, 0));
        ASSERT_NE(a, kNoEntity);
        ASSERT_NE(b, kNoEntity);
        sim::UseTarget t;
        ASSERT_TRUE(sim::FindUseTarget(f.simulation, glm::dvec3(kDoorSize.x * kVoxel, 13.0, 1.5), kLookMinusZ, 10, t));
        EXPECT_EQ(t.entity, static_cast<uint32_t>(a) < static_cast<uint32_t>(b) ? a : b) << "ties: the lower key";
    }
}

TEST(Use, SmallLeverIsUsable) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    const Entity lever = MakeYardLever(f, {0, kGroundVoxels, 0});
    ASSERT_NE(lever, kNoEntity);
    sim::UseTarget t;
    ASSERT_TRUE(sim::FindUseTarget(f.simulation, glm::dvec3(0.125, kGround + 0.4, 1.5), kLookMinusZ, 10, t));
    EXPECT_EQ(t.entity, lever);
    EXPECT_FALSE(t.info);
}

TEST(Use, ClosedGateBlocksTheLeverBehindIt) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    const Entity gate = MakeYardGate(f, {-8, kGroundVoxels, 0});            // z 0..0.125 m, closed
    const Entity lever = MakeYardLever(f, {0, kGroundVoxels, -4});          // z -0.5..-0.25 m, behind it
    ASSERT_NE(gate, kNoEntity);
    ASSERT_NE(lever, kNoEntity);
    const glm::dvec3 eye{0.125, kGround + 0.4, 1.0};
    sim::UseTarget t;
    ASSERT_TRUE(sim::FindUseTarget(f.simulation, eye, kLookMinusZ, 10, t));
    EXPECT_EQ(t.entity, gate) << "no usable target: the gate is the Info target";
    EXPECT_TRUE(t.info);

    sim::PushSegment(f.MechanismOf(gate)->track, {20, 320, 320, 0, 0});     // raised (p = maxPos)
    ASSERT_TRUE(sim::FindUseTarget(f.simulation, eye, kLookMinusZ, 30, t));
    EXPECT_EQ(t.entity, lever);
    EXPECT_FALSE(t.info);
}

TEST(Use, GateIsAnInfoTarget) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    const Entity gate = MakeYardGate(f, {-8, kGroundVoxels, 0});
    ASSERT_NE(gate, kNoEntity);
    const glm::dvec3 eye{0.0, kGround + 1.5, 1.5};
    sim::UseTarget t;
    ASSERT_TRUE(sim::FindUseTarget(f.simulation, eye, kLookMinusZ, 10, t));
    EXPECT_EQ(t.entity, gate);
    EXPECT_TRUE(t.info);
    std::vector<sim::MechanismChange> changes;
    EXPECT_EQ(sim::ApplyUse(f.simulation, t, eye, 10, sim::kOfflineMechanismLead, changes), sim::UseResult::NotUsable);
    EXPECT_TRUE(changes.empty());
    bool greyed = false;
    EXPECT_EQ(sim::UsePrompt(f.simulation, t, 10, greyed), "Gate - opened by a lever");
    EXPECT_TRUE(greyed);
}

TEST(Use, DoorOpensAwayFromTheUser) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    const Entity front = MakeYardDoor(f, {0, kGroundVoxels, 0});
    const Entity back = MakeYardDoor(f, {24, kGroundVoxels, 0});
    const Entity oneFront = MakeYardDoor(f, {48, kGroundVoxels, 0}, true);
    const Entity oneBack = MakeYardDoor(f, {72, kGroundVoxels, 0}, true);
    ASSERT_NE(front, kNoEntity);
    ASSERT_NE(back, kNoEntity);
    ASSERT_NE(oneFront, kNoEntity);
    ASSERT_NE(oneBack, kNoEntity);

    const auto use = [&](Entity e, double eyeZ) {
        const scene::Mechanism& m = *f.MechanismOf(e);
        const glm::dvec3 eye = glm::dvec3(m.spawnVoxel) * static_cast<double>(kVoxel) + glm::dvec3(0.5, 1.0, eyeZ);
        std::vector<sim::MechanismChange> changes;
        EXPECT_EQ(sim::ApplyUse(f.simulation, {e, false, 1.0f}, eye, 100, sim::kOfflineMechanismLead, changes), sim::UseResult::None);
        EXPECT_EQ(changes.size(), 1u);
        return changes.empty() ? scene::MechanismSegment{} : changes[0].segment;
    };

    EXPECT_EQ(use(front, 1.5).targetPos, 1024) << "eye on +z";
    // The free end of the leaf swings to -z, away from the eye.
    const glm::dvec3 freeEnd = YardVoxelCentre(*f.MechanismOf(front), glm::vec3(kDoorSize.x - 1, 8, 1), 1024);
    EXPECT_LT(freeEnd.z, 0.0);
    EXPECT_EQ(use(back, -1.5).targetPos, -1024) << "eye on -z";
    const glm::dvec3 backEnd = YardVoxelCentre(*f.MechanismOf(back), glm::vec3(kDoorSize.x - 1, 8, 1), -1024);
    EXPECT_GT(backEnd.z, kDoorSize.z * kVoxel);
    EXPECT_EQ(use(oneFront, 1.5).targetPos, 1024) << "OneWay ignores the side";
    EXPECT_EQ(use(oneBack, -1.5).targetPos, 1024) << "OneWay ignores the side";
}

TEST(Use, LeverDrivesItsTargets) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    const Entity gate = MakeYardGate(f, {-8, kGroundVoxels, 0});
    const Entity lever = MakeYardLever(f, {0, kGroundVoxels, 40});
    ASSERT_NE(gate, kNoEntity);
    ASSERT_NE(lever, kNoEntity);
    f.MechanismOf(lever)->linked = gate;
    const glm::dvec3 eye{0.125, kGround + 0.4, 6.0};
    const uint32_t lead = 4;

    std::vector<sim::MechanismChange> up;
    ASSERT_EQ(sim::ApplyUse(f.simulation, {lever, false, 1.0f}, eye, 100, lead, up), sim::UseResult::None);
    ASSERT_EQ(up.size(), 2u);
    EXPECT_EQ(up[0].entity, lever);
    EXPECT_EQ(up[1].entity, gate);
    EXPECT_EQ(up[0].segment.startTick, 100 + lead);
    EXPECT_EQ(up[1].segment.startTick, 100 + lead);
    EXPECT_EQ(up[0].segment.targetPos, 512);
    EXPECT_EQ(up[1].segment.targetPos, 320) << "lever up: gate to maxPos";
    EXPECT_EQ(f.MechanismOf(gate)->track.Newest(), up[1].segment);

    std::vector<sim::MechanismChange> down;
    ASSERT_EQ(sim::ApplyUse(f.simulation, {lever, false, 1.0f}, eye, 400, lead, down), sim::UseResult::None);
    ASSERT_EQ(down.size(), 2u);
    EXPECT_EQ(down[0].segment.startTick, down[1].segment.startTick);
    EXPECT_EQ(down[0].segment.targetPos, 0);
    EXPECT_EQ(down[1].segment.targetPos, 0) << "lever down: gate to minPos";

    std::vector<sim::MechanismChange> freed;
    sim::ReleaseMechanism(f.simulation, gate, 700, scene::MechanismFlag_Freed, freed);
    EXPECT_EQ(f.MechanismOf(gate), nullptr);
    std::vector<sim::MechanismChange> broken;
    EXPECT_EQ(sim::ApplyUse(f.simulation, {lever, false, 1.0f}, eye, 800, lead, broken), sim::UseResult::Broken);
    ASSERT_EQ(broken.size(), 1u) << "the lever still throws";
    EXPECT_EQ(broken[0].entity, lever);
    EXPECT_EQ(broken[0].segment.targetPos, 512);
}

TEST(Use, RefusalsInOrder) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    const glm::dvec3 eye = kFrontEye;
    std::vector<sim::MechanismChange> changes;

    scene::MechanismDesc lockedDesc = MakeDoorDesc();
    lockedDesc.flags |= scene::MechanismDesc_Locked;
    const Entity locked = MakeYardMechanism(f, MakeSolidModel(kDoorSize, f.Ids().planks, "door"), lockedDesc, {0, kGroundVoxels, 0});
    const Entity door = MakeYardDoor(f, {24, kGroundVoxels, 0});
    const Entity lever = MakeYardLever(f, {48, kGroundVoxels, 0});        // no target
    const Entity gate = MakeYardGate(f, {64, kGroundVoxels, 0});
    ASSERT_NE(locked, kNoEntity);
    ASSERT_NE(door, kNoEntity);
    ASSERT_NE(lever, kNoEntity);
    ASSERT_NE(gate, kNoEntity);

    EXPECT_EQ(sim::ApplyUse(f.simulation, {locked, false, 1.0f}, eye, 100, 2, changes), sim::UseResult::Locked);
    EXPECT_EQ(sim::ApplyUse(f.simulation, {door, false, 1.0f}, eye, 100, 2, changes), sim::UseResult::None);
    EXPECT_EQ(sim::ApplyUse(f.simulation, {door, false, 1.0f}, eye, 105, 2, changes), sim::UseResult::Busy);
    EXPECT_EQ(sim::ApplyUse(f.simulation, {lever, false, 1.0f}, eye, 100, 2, changes), sim::UseResult::Broken);
    EXPECT_EQ(sim::ApplyUse(f.simulation, {gate, true, 1.0f}, eye, 100, 2, changes), sim::UseResult::NotUsable);

    sim::UseTarget sky;
    EXPECT_FALSE(sim::FindUseTarget(f.simulation, glm::dvec3(0.0, kGround + 2.0, 20.0), glm::vec3(0.0f, 1.0f, 0.0f), 100, sky));
    EXPECT_EQ(sim::ApplyUse(f.simulation, sky, eye, 100, 2, changes), sim::UseResult::NoTarget);

    bool greyed = false;
    EXPECT_EQ(sim::UsePrompt(f.simulation, {locked, false, 1.0f}, 100, greyed), "Locked");
    EXPECT_TRUE(greyed);
    EXPECT_STREQ(sim::ToString(sim::UseResult::Busy), "busy");
    EXPECT_STREQ(sim::ToString(sim::UseResult::NoTarget), "out of reach");
}

TEST(Use, OpenCannotInterruptAClose) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    const Entity gate = MakeYardGate(f, {-8, kGroundVoxels, 0}, 320);      // raised
    const Entity lever = MakeYardLever(f, {0, kGroundVoxels, 40}, 512);    // up
    ASSERT_NE(gate, kNoEntity);
    ASSERT_NE(lever, kNoEntity);
    f.MechanismOf(lever)->linked = gate;
    const glm::dvec3 eye{0.125, kGround + 0.4, 6.0};
    const uint32_t lead = 4;
    std::vector<sim::MechanismChange> changes;

    ASSERT_EQ(sim::ApplyUse(f.simulation, {lever, false, 1.0f}, eye, 100, lead, changes), sim::UseResult::None);
    ASSERT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[1].segment.targetPos, 0) << "the gate drops";
    const uint32_t arrival = sim::ArrivalTick(f.MechanismOf(gate)->track);
    EXPECT_EQ(arrival, 100 + lead + 45);

    EXPECT_EQ(sim::ApplyUse(f.simulation, {lever, false, 1.0f}, eye, 115, lead, changes), sim::UseResult::Busy) << "15 ticks later";
    EXPECT_EQ(sim::ApplyUse(f.simulation, {lever, false, 1.0f}, eye, arrival + 10, lead, changes), sim::UseResult::Busy)
        << "10 ticks after its arrival";
    changes.clear();
    EXPECT_EQ(sim::ApplyUse(f.simulation, {lever, false, 1.0f}, eye, arrival + 21, lead, changes), sim::UseResult::None)
        << "21 ticks after its arrival";
    ASSERT_EQ(changes.size(), 2u);
    EXPECT_EQ(changes[1].segment.targetPos, 320);
}

TEST(Use, CloseInterruptsAnOpen) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    const Entity door = MakeYardDoor(f, kYardDoorSpawn);
    ASSERT_NE(door, kNoEntity);
    std::vector<sim::MechanismChange> changes;
    ASSERT_EQ(sim::ApplyUse(f.simulation, {door, false, 1.0f}, kFrontEye, 100, 2, changes), sim::UseResult::None);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].segment.targetPos, 1024);

    const uint32_t start = 113 + 2;
    const int16_t there = sim::PositionAt(*f.MechanismOf(door), start);
    changes.clear();
    ASSERT_EQ(sim::ApplyUse(f.simulation, {door, false, 1.0f}, kFrontEye, 113, 2, changes), sim::UseResult::None);
    ASSERT_EQ(changes.size(), 1u);
    EXPECT_EQ(changes[0].segment.startTick, start);
    EXPECT_EQ(changes[0].segment.startPos, there) << "starts at the exact position";
    EXPECT_EQ(changes[0].segment.targetPos, 0);
    EXPECT_EQ(sim::PositionAt(*f.MechanismOf(door), start), there);
}

namespace {
    /// A template of a gate (index 0) and its lever (index 1) on the gate's -z side, the lever's
    /// nearest use-box face `distance` metres from the gate's outer (+z) face.
    sim::RegionTemplate MakeYardGateTemplate(const voxel::WorldMaterials& ids, double distance) {
        sim::RegionTemplate t;
        t.mechanisms.push_back({.model = MakeSolidModel(kGateSize, ids.wood, "gate"),
                                .spawnVoxel = {-8, kGroundVoxels, 0},
                                .desc = MakeGateDesc()});
        const double outerFace = kGateSize.z * kVoxel;
        const int leverZ = static_cast<int>(std::lround((outerFace - distance) / kVoxel)) - kLeverSize.z;
        t.mechanisms.push_back({.model = MakeSolidModel(kLeverSize, ids.wood, "lever"),
                                .spawnVoxel = {0, kGroundVoxels, leverZ},
                                .desc = MakeLeverDesc(),
                                .link = 0});
        return t;
    }
}

TEST(Use, AuthoringRuleRejectsAReachableLever) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    const sim::RegionTemplate reachable = MakeYardGateTemplate(f.Ids(), 2.0);
    std::string why;
    EXPECT_FALSE(sim::CheckMechanismAuthoring(reachable, kVoxel, &why));
    EXPECT_NE(why.find("reachable through its gate"), std::string::npos) << why;
    EXPECT_TRUE(sim::CreateTemplateMechanisms(f.simulation, reachable).empty());

    const sim::RegionTemplate safe = MakeYardGateTemplate(f.Ids(), 3.5);
    EXPECT_TRUE(sim::CheckMechanismAuthoring(safe, kVoxel));
    const std::vector<Entity> made = sim::CreateTemplateMechanisms(f.simulation, safe);
    ASSERT_EQ(made.size(), 2u);
    ASSERT_NE(f.MechanismOf(made[1]), nullptr);
    EXPECT_EQ(f.MechanismOf(made[1])->linked, made[0]);
}

// =================================================================================================
//  The damage rule
// =================================================================================================

namespace {
    /// Centre of the door's leaf at rest (world metres).
    glm::dvec3 YardDoorCentre(const glm::ivec3& spawn) {
        return glm::dvec3(spawn) * static_cast<double>(kVoxel) + glm::dvec3(glm::vec3(kDoorSize) * (0.5f * kVoxel));
    }
}

TEST(Mechanism, DentedKeepsWorking) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    const Entity door = MakeYardDoor(f, kYardDoorSpawn);
    ASSERT_NE(door, kNoEntity);
    const uint32_t before = f.SolidOf(door);
    sim::BlastResult into;
    const sim::MechanismHit hit = sim::DamageMechanism(f.simulation, door, YardDoorCentre(kYardDoorSpawn), voxel::QuantizePower(6.0f), into);
    EXPECT_GT(hit.removed, 0u);
    EXPECT_FALSE(hit.freed);
    EXPECT_LT(f.SolidOf(door), before);
    ASSERT_NE(f.MechanismOf(door), nullptr);
    EXPECT_EQ(f.Scene().TryGet<scene::RigidBody>(door)->motion, physics::MotionType::Kinematic);

    std::vector<sim::MechanismChange> changes;
    ASSERT_EQ(sim::ApplyUse(f.simulation, {door, false, 1.0f}, kFrontEye, 100, 2, changes), sim::UseResult::None);
    EXPECT_EQ(sim::PositionAt(*f.MechanismOf(door), sim::ArrivalTick(f.MechanismOf(door)->track)), 1024);
}

TEST(Mechanism, DentThatCropsTheModelKeepsThePivot) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    // Hinged at the far edge (x = 8): the hits empty the model's minimum-corner column.
    scene::MechanismDesc desc = MakeDoorDesc();
    desc.pivotQ = {kDoorSize.x * 16, 0, kDoorSize.z * 8};
    desc.hingeBox = {glm::ivec3(kDoorSize.x - 1, 0, 0), kDoorSize};
    const Entity door = MakeYardMechanism(f, MakeSolidModel(kDoorSize, f.Ids().planks, "door"), desc, kYardDoorSpawn);
    ASSERT_NE(door, kNoEntity);
    const glm::vec3 hinge{kDoorSize.x - 1, 8, 1};          // an original-model voxel the hits never reach
    const std::array<int16_t, 5> ps{-1024, -512, 0, 512, 1024};
    std::array<glm::dvec3, 5> was{};
    for (size_t i = 0; i < ps.size(); ++i) was[i] = YardVoxelCentre(*f.MechanismOf(door), hinge, ps[i]);

    sim::BlastResult into;
    for (const int y : {2, 6, 10, 14}) {
        const glm::dvec3 centre = glm::dvec3(f.World().VoxelToWorld(kYardDoorSpawn + glm::ivec3(0, y, 0))) +
                                  glm::dvec3(0.0, 0.0, kDoorSize.z * 0.5 * kVoxel);
        sim::DamageMechanism(f.simulation, door, centre, voxel::QuantizePower(6.0f), into);
    }
    ASSERT_NE(f.MechanismOf(door), nullptr);
    EXPECT_NE(f.MechanismOf(door)->modelOffset, glm::ivec3(0)) << "the model was cropped";
    for (size_t i = 0; i < ps.size(); ++i)
        EXPECT_LT(glm::length(YardVoxelCentre(*f.MechanismOf(door), hinge, ps[i]) - was[i]), 1.0e-3) << "p = " << ps[i];
    const sim::MechanismPose pose = sim::PoseAt(*f.MechanismOf(door), sim::PositionAt(*f.MechanismOf(door), 0), kVoxel);
    const scene::Transform* transform = f.Scene().TryGet<scene::Transform>(door);
    ASSERT_NE(transform, nullptr);
    EXPECT_LT(glm::length(glm::dvec3(transform->current.position) - pose.position), 1.0e-3) << "re-posed after the dent";
}

TEST(Mechanism, SplitDoorKeepsItsHingedPart) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    // A short 12 x 4 x 3 door: a hit at x = 4 voxels cuts it across, leaving the hinge part (x 0..1,
    // 24 voxels) smaller than the free part (x 7..11).
    const glm::ivec3 size{12, 4, 3};
    scene::MechanismDesc desc = MakeDoorDesc();
    desc.useBox = {glm::ivec3(0), size};
    desc.blockBox = {glm::ivec3(0), size};
    desc.hingeBox = {glm::ivec3(0), glm::ivec3(1, size.y, size.z)};
    const Entity door = MakeYardMechanism(f, MakeSolidModel(size, f.Ids().planks, "door"), desc, kYardDoorSpawn);
    ASSERT_NE(door, kNoEntity);
    const glm::dvec3 centre = glm::dvec3(f.World().VoxelToWorld(kYardDoorSpawn)) + glm::dvec3(4.0, 2.0, 1.5) * static_cast<double>(kVoxel);
    sim::BlastResult into;
    const sim::MechanismHit hit = sim::DamageMechanism(f.simulation, door, centre, voxel::QuantizePower(6.0f), into);
    EXPECT_GT(hit.removed, 0u);
    EXPECT_FALSE(hit.freed);
    ASSERT_NE(f.MechanismOf(door), nullptr) << "the hinged part keeps the entity";
    EXPECT_EQ(f.Scene().TryGet<scene::RigidBody>(door)->motion, physics::MotionType::Kinematic);
    EXPECT_LT(f.SolidOf(door), 40u);

    uint32_t dynamicPieces = 0;
    for (const auto [entity, body] : f.Scene().Registry().view<const scene::RigidBody>().each())
        if (entity != door && body.motion == physics::MotionType::Dynamic && f.SolidOf(entity) > 0) ++dynamicPieces;
    EXPECT_EQ(dynamicPieces, 1u) << "the other part is a new Dynamic piece";
}

TEST(Mechanism, HingeGoneFrees) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    const Entity door = MakeYardDoor(f, kYardDoorSpawn);
    ASSERT_NE(door, kNoEntity);
    const scene::MechanismDesc desc = MakeDoorDesc();
    sim::BlastResult into;
    bool freed = false;
    for (int i = 0; i < 12 && !freed; ++i) {
        const int y = desc.hingeBox.min.y + (i * 3) % (desc.hingeBox.max.y - desc.hingeBox.min.y);
        const glm::dvec3 centre = glm::dvec3(f.World().VoxelToWorld(kYardDoorSpawn + glm::ivec3(0, y, 0))) +
                                  glm::dvec3(0.5, 0.5, kDoorSize.z * 0.5) * static_cast<double>(kVoxel);
        freed = sim::DamageMechanism(f.simulation, door, centre, voxel::QuantizePower(6.0f), into).freed;
    }
    ASSERT_TRUE(freed);
    EXPECT_EQ(f.MechanismOf(door), nullptr);
    if (f.Scene().IsValid(door)) {
        EXPECT_EQ(f.Scene().TryGet<scene::RigidBody>(door)->motion, physics::MotionType::Dynamic);
        EXPECT_EQ(f.LayerOf(door), physics::Layer::Dynamic);
    }
    ASSERT_FALSE(into.mechanismChanges.empty());
    EXPECT_NE(into.mechanismChanges.back().segment.flags & scene::MechanismFlag_Freed, 0);
}

TEST(Mechanism, AnchorAirFrees) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    const uint8_t stone = f.Ids().stone;
    const glm::ivec3 low{-1, kGroundVoxels + 4, 1}, high{-1, kGroundVoxels + 11, 1};
    f.Apply(sim::WorldOp::Box({-2, kGroundVoxels, 0}, {0, kGroundVoxels + 16, 3}, stone));
    scene::MechanismDesc desc = MakeDoorDesc();
    desc.anchorCount = 2;
    desc.anchors = {low, high};
    const Entity door = MakeYardMechanism(f, MakeSolidModel(kDoorSize, f.Ids().planks, "door"), desc, kYardDoorSpawn);
    ASSERT_NE(door, kNoEntity);

    const sim::MechanismStepResult held = sim::StepMechanisms(f.simulation, 10, true);
    EXPECT_TRUE(held.changes.empty());
    ASSERT_NE(f.MechanismOf(door), nullptr);

    f.Apply(sim::WorldOp::Box(low, low + glm::ivec3(1), 0));
    const sim::MechanismStepResult one = sim::StepMechanisms(f.simulation, 11, true);
    EXPECT_TRUE(one.changes.empty()) << "one anchor left holds it";
    f.Apply(sim::WorldOp::Box(high, high + glm::ivec3(1), 0));
    const sim::MechanismStepResult r = sim::StepMechanisms(f.simulation, 12, true);
    ASSERT_EQ(r.changes.size(), 1u);
    EXPECT_EQ(r.changes[0].entity, door);
    EXPECT_NE(r.changes[0].segment.flags & scene::MechanismFlag_Freed, 0);
    EXPECT_EQ(f.MechanismOf(door), nullptr);
    EXPECT_EQ(f.Scene().TryGet<scene::RigidBody>(door)->motion, physics::MotionType::Dynamic);
}

TEST(Mechanism, CastAtTheGateLeavesTheLeverBesideIt) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    // A wall z 0..1 m with a lever on its +z face; the cast hits the wall 0.8 m beside the lever.
    f.Apply(sim::WorldOp::Box({-16, kGroundVoxels, 0}, {16, kGroundVoxels + 24, 8}, f.Ids().stone));
    const Entity lever = MakeYardLever(f, {0, kGroundVoxels + 8, 8});
    ASSERT_NE(lever, kNoEntity);
    const uint32_t before = f.SolidOf(lever);
    const double leverRight = kLeverSize.x * kVoxel;
    const glm::dvec3 centre{leverRight + 0.8, (kGroundVoxels + 11) * kVoxel, 1.0};
    const sim::BlastResult blast = sim::Blast(f.simulation, sim::BlastDesc{centre, 1.2f, 6.0f});
    EXPECT_GT(blast.ops.at(0).result.changed, 0u) << "the wall was carved";
    EXPECT_EQ(f.SolidOf(lever), before) << "the lever keeps every voxel";
    EXPECT_NE(f.MechanismOf(lever), nullptr);
    EXPECT_TRUE(blast.mechanismChanges.empty());
}

TEST(Mechanism, DoorTakesNHits) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    const Entity door = MakeYardDoor(f, kYardDoorSpawn);
    ASSERT_NE(door, kNoEntity);
    const uint16_t power = voxel::QuantizePower(6.0f);
    const double radius = 1.2;
    // From the leaf centre towards the hinge in 0.25 m steps, then up and down the hinge column.
    const glm::dvec3 centre = YardDoorCentre(kYardDoorSpawn);
    int hits = 0;
    bool freed = false;
    for (int i = 0; i < 12 && !freed; ++i) {
        const double x = std::max(centre.x - 0.25 * i, 0.0625);
        const int over = std::max(0, i - 2);
        const double y = centre.y + 0.25 * ((over + 1) / 2) * (over % 2 == 1 ? 1.0 : -1.0);
        const sim::BlastResult r = sim::MechanismBlast(f.simulation, door, YardSphere({x, y, centre.z}, radius), power);
        ++hits;
        for (const sim::MechanismChange& c : r.mechanismChanges)
            if (c.entity == door && (c.segment.flags & scene::MechanismFlag_Freed) != 0) freed = true;
    }
    std::printf("Mechanism hits to free a door: %d\n", hits);
    EXPECT_TRUE(freed);
    EXPECT_GE(hits, 3);
    EXPECT_LE(hits, 8);
}

TEST(Mechanism, MechanismHitCarvesNoTerrain) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    // A stone frame around the door: jambs on both sides and a lintel above.
    const uint8_t stone = f.Ids().stone;
    const int g = kGroundVoxels;
    f.Apply(sim::WorldOp::Box({-3, g, 0}, {0, g + kDoorSize.y + 3, kDoorSize.z}, stone));
    f.Apply(sim::WorldOp::Box({kDoorSize.x, g, 0}, {kDoorSize.x + 3, g + kDoorSize.y + 3, kDoorSize.z}, stone));
    f.Apply(sim::WorldOp::Box({0, g + kDoorSize.y, 0}, {kDoorSize.x, g + kDoorSize.y + 3, kDoorSize.z}, stone));
    const Entity door = MakeYardDoor(f, kYardDoorSpawn);
    ASSERT_NE(door, kNoEntity);
    const voxel::WorldHashResult before = f.Hash();
    const sim::BlastResult r =
        sim::MechanismBlast(f.simulation, door, YardSphere(YardDoorCentre(kYardDoorSpawn), 1.2), voxel::QuantizePower(6.0f));
    EXPECT_TRUE(r.ops.empty()) << "no terrain operation";
    EXPECT_GT(r.objectVoxels, 0u);
    EXPECT_EQ(f.Hash().hash, before.hash);
}

TEST(Mechanism, WardedTakesNoDamage) {
    YardField f;
    ASSERT_TRUE(f.initialized);
    scene::MechanismDesc desc = MakeDoorDesc();
    desc.flags |= scene::MechanismDesc_Warded;
    const glm::ivec3 spawn{0, kGroundVoxels + 16, 0};                 // 2 m up: the blast carves no ground
    const Entity door = MakeYardMechanism(f, MakeSolidModel(kDoorSize, f.Ids().planks, "door"), desc, spawn);
    ASSERT_NE(door, kNoEntity);
    const uint32_t before = f.SolidOf(door);
    const glm::dvec3 centre = YardDoorCentre(spawn);
    const sim::BlastResult blast = sim::Blast(f.simulation, sim::BlastDesc{centre, 1.2f, 6.0f});
    EXPECT_EQ(blast.objectVoxels, 0u);
    const sim::BlastResult hit = sim::MechanismBlast(f.simulation, door, YardSphere(centre, 1.2), voxel::QuantizePower(6.0f));
    EXPECT_EQ(hit.objectVoxels, 0u);
    EXPECT_EQ(f.SolidOf(door), before);
    EXPECT_NE(f.MechanismOf(door), nullptr);
}

namespace {
    /// The same uses and casts into a fresh yard: a door, and a lever that drives a gate.
    struct YardScriptOut {
        std::array<scene::MechanismTrack, 3> tracks{};
        std::array<glm::ivec3, 3> offsets{};
        std::array<std::vector<uint8_t>, 3> volumes{};
        uint64_t worldHash = 0;
        bool ok = false;
    };
    YardScriptOut RunYardScript() {
        YardScriptOut out;
        YardField f;
        if (!f.initialized) return out;
        const Entity door = MakeYardDoor(f, kYardDoorSpawn);
        const Entity gate = MakeYardGate(f, {-40, kGroundVoxels, 0});
        const Entity lever = MakeYardLever(f, {-24, kGroundVoxels, 40});
        if (door == kNoEntity || gate == kNoEntity || lever == kNoEntity) return out;
        f.MechanismOf(lever)->linked = gate;
        const uint16_t power = voxel::QuantizePower(6.0f);
        std::vector<sim::MechanismChange> changes;

        sim::ApplyUse(f.simulation, {door, false, 1.0f}, kFrontEye, 100, 4, changes);
        sim::ApplyUse(f.simulation, {lever, false, 1.0f}, glm::dvec3(-2.875, kGround + 0.4, 6.0), 110, 7, changes);
        sim::MechanismBlast(f.simulation, door, YardSphere(YardDoorCentre(kYardDoorSpawn), 1.2), power);
        sim::ApplyUse(f.simulation, {door, false, 1.0f}, kFrontEye, 140, 5, changes);
        sim::ApplyUse(f.simulation, {door, false, 1.0f}, glm::dvec3(0.5, 13.0, -1.5), 200, 4, changes);
        sim::Blast(f.simulation, sim::BlastDesc{glm::dvec3(-4.0, kGround, 2.0), 1.2f, 6.0f});
        sim::ApplyUse(f.simulation, {lever, false, 1.0f}, glm::dvec3(-2.875, kGround + 0.4, 6.0), 400, 20, changes);

        const std::array<Entity, 3> all{door, gate, lever};
        for (size_t i = 0; i < all.size(); ++i) {
            if (const scene::Mechanism* m = f.MechanismOf(all[i])) {
                out.tracks[i] = m->track;
                out.offsets[i] = m->modelOffset;
            }
            if (const scene::VoxelBody* v = f.Scene().TryGet<scene::VoxelBody>(all[i])) out.volumes[i] = v->model->Volume().Data();
        }
        out.worldHash = f.Hash().hash;
        out.ok = true;
        return out;
    }
}

TEST(Mechanism, TwoSimulationsFedTheSameInputsAgree) {
    const YardScriptOut a = RunYardScript();
    const YardScriptOut b = RunYardScript();
    ASSERT_TRUE(a.ok);
    ASSERT_TRUE(b.ok);
    for (size_t i = 0; i < a.tracks.size(); ++i) {
        EXPECT_EQ(a.tracks[i].count, b.tracks[i].count) << i;
        EXPECT_EQ(a.tracks[i].ring, b.tracks[i].ring) << i;
        EXPECT_EQ(a.offsets[i], b.offsets[i]) << i;
        EXPECT_EQ(a.volumes[i], b.volumes[i]) << i;
        EXPECT_GT(static_cast<uint32_t>(a.tracks[i].count), 1u) << "every mechanism moved, " << i;
    }
    EXPECT_EQ(a.worldHash, b.worldHash);
}

// =================================================================================================
//  Holding a small object with E (mechanisms.md "One use key: E")
// =================================================================================================

namespace {
    constexpr float kHoldVoxel = 0.1f;
    const glm::vec3 kHoldFeet{0.0f, 10.0f, 0.0f};
    const glm::vec3 kHoldLook{0.0f, 0.0f, -1.0f};
    const glm::dvec3 kHoldEye{0.0, 11.62, 0.0};

    /// A simulation on flat stone ground (top at y = 10 m), its window loaded at the origin.
    struct HoldYard {
        explicit HoldYard(float voxelSize = kHoldVoxel) : simulation(Config(voxelSize)) {
            voxel::MaterialLibrary scratch;
            const voxel::WorldMaterials ids = voxel::RegisterDefaultWorldMaterials(scratch);
            const int ground = static_cast<int>(std::lround(10.0 / static_cast<double>(voxelSize)));
            initialized = simulation.Initialize(voxel::CreateFlatGenerator(ground, ids.stone, ids.stone));
            if (initialized) simulation.World().LoadBlocking(glm::dvec3(0.0, 10.0, 0.0));
        }
        HoldYard(const HoldYard&) = delete;
        HoldYard& operator=(const HoldYard&) = delete;

        static sim::SimulationConfig Config(float voxelSize) {
            sim::SimulationConfig config = YardConfig();
            config.world.voxelSize = voxelSize;
            return config;
        }
        scene::Scene& Scene() { return simulation.Scene(); }
        const voxel::WorldMaterials& Ids() const { return simulation.WorldMaterialIds(); }
        void Apply(const sim::WorldOp& op) { EXPECT_TRUE(sim::ApplyOp(simulation.World(), op).Complete()); }
        /// The voxel of a point given in metres (0.1 m voxels).
        static glm::ivec3 V(const glm::dvec3& p) {
            return {static_cast<int>(std::lround(p.x / kHoldVoxel)), static_cast<int>(std::lround(p.y / kHoldVoxel)),
                    static_cast<int>(std::lround(p.z / kHoldVoxel))};
        }
        void Stone(const glm::dvec3& min, const glm::dvec3& max) { Apply(sim::WorldOp::Box(V(min), V(max), Ids().stone)); }
        void Air(const glm::dvec3& min, const glm::dvec3& max) { Apply(sim::WorldOp::Box(V(min), V(max), 0)); }

        /// A crate (8^3 voxels at 0.1 m) with its centre at `centre`.
        Entity Crate(const glm::dvec3& centre, physics::MotionType motion = physics::MotionType::Dynamic) {
            const auto model = sim::CrateModel(Ids(), sim::PlaygroundScale(simulation.World().VoxelSize()));
            const glm::vec3 half = glm::vec3(model->Volume().Size()) * (0.5f * simulation.World().VoxelSize());
            return Scene().CreateVoxelObject({.model = model, .position = glm::vec3(centre) - half, .motion = motion});
        }
        glm::dvec3 CentreOf(Entity e) {
            const scene::Transform* t = Scene().TryGet<scene::Transform>(e);
            const scene::VoxelBody* v = Scene().TryGet<scene::VoxelBody>(e);
            const glm::vec3 half = glm::vec3(v->model->Volume().Size()) * (0.5f * simulation.World().VoxelSize());
            return glm::dvec3(t->current.position) + glm::dvec3(t->current.rotation * half);
        }
        sim::HoldEnv Env() const { return sim::HoldEnv{sim::PlayerTuning{}, sim::PlayArea{}, {}}; }
        physics::BodyId Body(Entity e) { return Scene().TryGet<scene::RigidBody>(e)->body; }

        sim::Simulation simulation;
        bool initialized = false;
    };

    /// A door (8 x 16 x 3 voxels) with its spawn voxel `spawn`, at travel position `p`.
    Entity HoldYardDoor(HoldYard& y, const glm::ivec3& spawn, int16_t p = 0) {
        const Entity e = y.Scene().CreateVoxelObject({.model = MakeSolidModel(kDoorSize, y.Ids().planks, "door"),
                                                      .position = glm::vec3(y.simulation.World().VoxelToWorld(spawn)),
                                                      .motion = physics::MotionType::Kinematic});
        if (e == kNoEntity || !sim::MakeMechanism(y.simulation, e, MakeDoorDesc(), spawn, 0, p, 0)) return kNoEntity;
        return e;
    }

    /// A lever (2 x 6 x 2 voxels) with its spawn voxel `spawn`.
    Entity HoldYardLever(HoldYard& y, const glm::ivec3& spawn) {
        const Entity e = y.Scene().CreateVoxelObject({.model = MakeSolidModel(kLeverSize, y.Ids().wood, "lever"),
                                                      .position = glm::vec3(y.simulation.World().VoxelToWorld(spawn)),
                                                      .motion = physics::MotionType::Kinematic});
        if (e == kNoEntity || !sim::MakeMechanism(y.simulation, e, MakeLeverDesc(), spawn, 0, 0, 0)) return kNoEntity;
        return e;
    }

    sim::UseTarget HoldFind(HoldYard& y, const glm::dvec3& eye, const glm::vec3& dir = kHoldLook) {
        sim::UseTarget t;
        sim::FindUseTarget(y.simulation, eye, dir, y.simulation.Tick(), t);
        return t;
    }

    glm::dvec3 HeldCentre(const sim::HeldPose& p) { return p.pose.position + glm::dvec3(p.pose.rotation * glm::vec3(0.4f)); }
}

TEST(Hold, CrateIsHoldableBigThingsAreNot) {
    for (const float vs : {0.1f, 0.125f, 0.15f}) {
        HoldYard y(vs);
        ASSERT_TRUE(y.initialized);
        const Entity crate = y.Crate({0.0, 12.0, -3.0});
        ASSERT_NE(crate, kNoEntity);
        EXPECT_TRUE(sim::IsHoldable(y.simulation, crate, false)) << vs;
    }
    EXPECT_EQ(sim::HoldMaxVoxels(0.1f), 600u);
    EXPECT_EQ(sim::HoldMaxVoxels(0.125f), 307u);
    EXPECT_EQ(sim::HoldMaxVoxels(0.15f), 178u);

    HoldYard y;
    ASSERT_TRUE(y.initialized);
    const auto object = [&](const glm::ivec3& size, uint8_t material, physics::MotionType motion = physics::MotionType::Dynamic) {
        return y.Scene().CreateVoxelObject({.model = MakeSolidModel(size, material, "box"), .position = {2.0f, 12.0f, -3.0f}, .motion = motion});
    };
    EXPECT_FALSE(sim::IsHoldable(y.simulation, object({13, 4, 4}, y.Ids().planks), false)) << "1.3 m long";
    EXPECT_FALSE(sim::IsHoldable(y.simulation, object({9, 9, 9}, y.Ids().stone), false)) << "729 > 600 voxels";
    EXPECT_FALSE(sim::IsHoldable(y.simulation, object({5, 2, 2}, y.Ids().stone), false)) << "under debrisBodyVoxels";
    EXPECT_FALSE(sim::IsHoldable(y.simulation, HoldYardDoor(y, {40, 100, 0}), false)) << "a mechanism";
    EXPECT_FALSE(sim::IsHoldable(y.simulation, object({4, 4, 4}, y.Ids().wood, physics::MotionType::Kinematic), false)) << "kinematic";

    const Entity copy = y.Crate({-2.0, 12.0, -3.0}, physics::MotionType::Kinematic);
    EXPECT_TRUE(sim::IsHoldableShape(y.simulation, copy)) << "a client copy passes the shape test";
    EXPECT_FALSE(sim::IsHoldable(y.simulation, copy, false));

    const Entity crate = y.Crate({0.0, 12.0, -3.0});
    EXPECT_FALSE(sim::IsHoldable(y.simulation, crate, true)) << "replicatedOnly: no NetId";
    sim::HoldChange c;
    ASSERT_EQ(sim::BeginHold(y.simulation, crate, 0, 0, 10, 0, false, false, false, c), sim::UseResult::None);
    EXPECT_FALSE(sim::IsHoldable(y.simulation, crate, false)) << "already held";
}

TEST(Hold, NearestTargetWins) {
    // The lever (x -0.1..0.1, y 11.3..11.9, z -2.3..-2.1): its inflated use box is entered 2.0 m ahead.
    {
        HoldYard y;
        ASSERT_TRUE(y.initialized);
        ASSERT_NE(HoldYardLever(y, {-1, 113, -23}), kNoEntity);
        const Entity crate = y.Crate({0.0, 11.62, -1.7});   // its inflated box is entered 1.2 m ahead
        const sim::UseTarget t = HoldFind(y, kHoldEye);
        EXPECT_EQ(t.kind, sim::UseTargetKind::Holdable);
        EXPECT_EQ(t.entity, crate);
        EXPECT_NEAR(t.distance, 1.2f, 1.0e-3f);
    }
    {
        HoldYard y;
        ASSERT_TRUE(y.initialized);
        const Entity lever = HoldYardLever(y, {-1, 113, -23});
        ASSERT_NE(lever, kNoEntity);
        y.Crate({0.0, 11.62, -2.9});                        // behind the lever
        const sim::UseTarget t = HoldFind(y, kHoldEye);
        EXPECT_EQ(t.kind, sim::UseTargetKind::Mechanism);
        EXPECT_EQ(t.entity, lever);
    }
}

TEST(Hold, ClosedDoorHidesACrateBehindIt) {
    for (const int16_t p : {int16_t{0}, int16_t{1024}}) {
        HoldYard y;
        ASSERT_TRUE(y.initialized);
        // The door at x -0.6..0.2 (hinge at x -0.6), z -1.3..-1.0; the eye and the crate on x = 0.
        const Entity door = HoldYardDoor(y, {-6, 105, -13}, p);
        ASSERT_NE(door, kNoEntity);
        const Entity crate = y.Crate({0.0, 11.62, -2.5});
        const sim::UseTarget t = HoldFind(y, kHoldEye);
        if (p == 0) {
            EXPECT_EQ(t.kind, sim::UseTargetKind::Mechanism);
            EXPECT_EQ(t.entity, door);
        } else {
            EXPECT_EQ(t.kind, sim::UseTargetKind::Holdable) << "the open door is out of the ray";
            EXPECT_EQ(t.entity, crate);
        }
    }
}

TEST(Hold, HeldByAnotherIsBusy) {
    HoldYard y;
    ASSERT_TRUE(y.initialized);
    const Entity crate = y.Crate({0.0, 11.62, -1.5});
    ASSERT_NE(HoldYardDoor(y, {-4, 105, -25}), kNoEntity);
    sim::HoldChange c;
    ASSERT_EQ(sim::BeginHold(y.simulation, crate, 1, 0, 10, 0, false, false, false, c), sim::UseResult::None);
    const sim::UseTarget t = HoldFind(y, kHoldEye);
    EXPECT_EQ(t.kind, sim::UseTargetKind::HeldByOther);
    EXPECT_EQ(t.entity, crate);
    bool greyed = false;
    EXPECT_EQ(sim::UsePrompt(y.simulation, t, 10, greyed), "Held");
    EXPECT_TRUE(greyed);
}

TEST(Hold, CannotPassASlit) {
    HoldYard y;
    ASSERT_TRUE(y.initialized);
    y.Stone({-2.0, 10.0, -2.0}, {2.0, 13.0, -1.6});
    y.Air({-0.1, 10.0, -2.0}, {0.1, 13.0, -1.6});
    const Entity crate = y.Crate({0.0, 11.62, -2.4});
    EXPECT_FALSE(sim::PickupPathClear(y.simulation, crate, sim::HoldAnchor(kHoldFeet, sim::PlayerTuning{})));
    sim::UseTarget t;
    EXPECT_FALSE(sim::FindUseTarget(y.simulation, kHoldEye, kHoldLook, y.simulation.Tick(), t));
    EXPECT_EQ(t.kind, sim::UseTargetKind::None);

    y.Stone({-0.1, 10.0, -2.0}, {0.1, 13.0, -1.6});
    y.Air({-0.5, 10.0, -2.0}, {0.5, 13.0, -1.6});
    EXPECT_EQ(HoldFind(y, kHoldEye).kind, sim::UseTargetKind::Holdable) << "a 1.0 m opening";

    y.Stone({-0.5, 10.0, -2.0}, {0.5, 13.0, -1.6});
    y.Air({-0.1, 10.0, -2.0}, {0.1, 13.0, -1.6});
    sim::HoldChange c;
    ASSERT_EQ(sim::BeginHold(y.simulation, crate, 0, 0, 10, 0, false, false, false, c), sim::UseResult::None);
    const sim::HeldPose p = sim::HeldPoseAt(y.simulation, crate, kHoldFeet, 0, 0, y.Env(), y.simulation.Tick());
    EXPECT_NEAR(p.distance, 1.33520f, 1.0e-3f) << "the slit's edge voxels stop the sphere";
    EXPECT_GT(HeldCentre(p).z, -1.6) << "the centre stays on the near side";
}

TEST(Hold, CannotPickUpWhatAPlayerStandsOn) {
    HoldYard y;
    ASSERT_TRUE(y.initialized);
    const Entity crate = y.Crate({0.0, 10.4, -1.5});
    sim::HoldChange c;
    EXPECT_EQ(sim::BeginHold(y.simulation, crate, 0, 0, 10, 0, false, true, false, c), sim::UseResult::Busy);
    EXPECT_EQ(y.Scene().TryGet<scene::Held>(crate), nullptr);
    EXPECT_EQ(sim::BeginHold(y.simulation, crate, 0, 0, 10, 0, false, false, false, c), sim::UseResult::None);
    EXPECT_NE(y.Scene().TryGet<scene::Held>(crate), nullptr);
}

TEST(Hold, PoseFollowsTheHolder) {
    HoldYard y;
    ASSERT_TRUE(y.initialized);
    const Entity crate = y.Crate({3.0, 10.4, 3.0});
    sim::HoldChange c;
    ASSERT_EQ(sim::BeginHold(y.simulation, crate, 0, 0, 10, 0, false, false, false, c), sim::UseResult::None);
    EXPECT_EQ(c.relYaw, 0u);

    const sim::HeldPose a = sim::HeldPoseAt(y.simulation, crate, kHoldFeet, 0, 0, y.Env(), 10);
    EXPECT_NEAR(HeldCentre(a).x, 0.0, 1.0e-4);
    EXPECT_NEAR(HeldCentre(a).y, 11.12, 1.0e-4);
    EXPECT_NEAR(HeldCentre(a).z, -1.5, 1.0e-4);
    EXPECT_NEAR(a.pose.position.x, -0.4, 1.0e-4);
    EXPECT_NEAR(a.pose.position.y, 10.72, 1.0e-4);
    EXPECT_NEAR(a.pose.position.z, -1.9, 1.0e-4);
    EXPECT_FALSE(a.pinned);

    const sim::HeldPose b = sim::HeldPoseAt(y.simulation, crate, kHoldFeet, 16384, 0, y.Env(), 10);
    EXPECT_NEAR(HeldCentre(b).x, -1.5, 1.0e-4);
    EXPECT_NEAR(HeldCentre(b).z, 0.0, 1.0e-4);
    const glm::quat quarter = glm::angleAxis(1.5707963f, glm::vec3(0.0f, 1.0f, 0.0f));
    EXPECT_NEAR(std::abs(glm::dot(b.pose.rotation, quarter)), 1.0f, 1.0e-5f);

    EXPECT_EQ(sim::HoldPitch(20000), 64);
    EXPECT_EQ(sim::HoldPitch(-20000), -64);
    const sim::HeldPose up = sim::HeldPoseAt(y.simulation, crate, kHoldFeet, 0, sim::HoldPitch(20000), y.Env(), 10);
    EXPECT_NEAR(HeldCentre(up).y, 12.18066, 1.0e-3);
    EXPECT_NEAR(HeldCentre(up).z, -1.06063, 1.0e-3);

    EXPECT_EQ(sim::RelYawAtPickup(glm::angleAxis(1.5707963f, glm::vec3(0.0f, 1.0f, 0.0f)), 0), 16384u);
}

TEST(Hold, WallClampsTheDistance) {
    HoldYard y;
    ASSERT_TRUE(y.initialized);
    const Entity crate = y.Crate({3.0, 10.4, 3.0});
    sim::HoldChange c;
    ASSERT_EQ(sim::BeginHold(y.simulation, crate, 0, 0, 10, 0, false, false, false, c), sim::UseResult::None);

    y.Stone({-2.0, 10.0, -2.6}, {2.0, 13.0, -1.6});
    sim::HeldPose p = sim::HeldPoseAt(y.simulation, crate, kHoldFeet, 0, 0, y.Env(), 10);
    EXPECT_NEAR(p.distance, 1.30489f, 1.0e-3f);
    EXPECT_FALSE(p.pinned);

    y.Air({-2.0, 10.0, -2.6}, {2.0, 13.0, -1.6});
    y.Stone({-2.0, 10.0, -2.2}, {2.0, 13.0, -1.0});
    p = sim::HeldPoseAt(y.simulation, crate, kHoldFeet, 0, 0, y.Env(), 10);
    EXPECT_NEAR(p.distance, 0.79282f, 1.0e-4f);
    EXPECT_TRUE(p.pinned);

    y.Air({-2.0, 10.0, -2.2}, {2.0, 13.0, -1.0});
    ASSERT_NE(HoldYardDoor(y, {-4, 105, -19}), kNoEntity);   // block box z -1.9 .. -1.6
    p = sim::HeldPoseAt(y.simulation, crate, kHoldFeet, 0, 0, y.Env(), 10);
    EXPECT_NEAR(p.distance, 1.25f, 1.0e-3f) << "the door's block box grown by the sweep radius";
}

TEST(Hold, PlayAreaClampsTheHold) {
    HoldYard y;
    ASSERT_TRUE(y.initialized);
    const Entity crate = y.Crate({3.0, 10.4, 3.0});
    sim::HoldChange c;
    ASSERT_EQ(sim::BeginHold(y.simulation, crate, 0, 0, 10, 0, false, false, false, c), sim::UseResult::None);
    sim::HoldEnv env = y.Env();
    env.area.min = {-1.0f, -1.0f};
    env.area.max = {1.0f, 1.0f};
    const sim::HeldPose p = sim::HeldPoseAt(y.simulation, crate, kHoldFeet, 0, 0, env, 10);
    EXPECT_NEAR(HeldCentre(p).x, 0.0, 1.0e-4);
    EXPECT_NEAR(HeldCentre(p).y, 11.12, 1.0e-4);
    EXPECT_NEAR(HeldCentre(p).z, -1.0, 1.0e-4);
}

TEST(Hold, HeldBodyMeetsNothing) {
    HoldYard y;
    ASSERT_TRUE(y.initialized);
    const std::array<glm::dvec3, 1> focus = {glm::dvec3(0.0, 10.0, 0.0)};
    y.simulation.UpdateStreaming(focus);
    y.simulation.Terrain().Flush();
    const Entity resting = y.Crate({0.0, 10.4, -1.5});
    for (int i = 0; i < 60; ++i) y.simulation.Step(focus);    // it settles
    const glm::dvec3 restAt = y.CentreOf(resting);

    const Entity crate = y.Crate({3.0, 10.4, 3.0});
    sim::HoldChange c;
    ASSERT_EQ(sim::BeginHold(y.simulation, crate, 0, 0, y.simulation.Tick(), 0, false, false, false, c), sim::UseResult::None);
    EXPECT_EQ(y.simulation.Physics().GetLayer(y.Body(crate)), physics::Layer::Held);

    // 1. The held crate posed through the resting one for 60 ticks: the resting crate does not move.
    sim::PlayerState holder;
    holder.position = kHoldFeet;
    for (int i = 0; i < 60; ++i) {
        sim::StepHeld(y.simulation, crate, holder, 0, 0, y.Env(), y.simulation.Tick(), true);
        y.simulation.Step(focus);
    }
    EXPECT_NEAR(y.CentreOf(resting).x, restAt.x, 1.0e-4);
    EXPECT_NEAR(y.CentreOf(resting).y, restAt.y, 1.0e-4);
    EXPECT_NEAR(y.CentreOf(resting).z, restAt.z, 1.0e-4);
    EXPECT_NEAR(glm::length(y.simulation.Physics().GetState(y.Body(resting)).linearVelocity), 0.0f, 1.0e-4f);

    // 2. The lane cleared, a second player walks along -z from z = 1 m through the held crate's place.
    y.Scene().Destroy(resting);
    holder.position = {0.0f, 10.0f, 0.0f};
    const sim::PlayerTuning tuning;
    sim::PlayerState walker;
    walker.position = {0.0f, 10.0f, 1.0f};
    const physics::CharacterId character = y.simulation.Physics().CreateCharacter(sim::PlayerCharacterDesc(walker.position, tuning));
    sim::PlayerInput walk;
    walk.moveZ = 127;
    for (int i = 0; i < 60; ++i) {
        const uint32_t tick = y.simulation.Tick();
        y.simulation.Terrain().FlushBox(sim::PlayerReach(walker.position, tuning));
        sim::StepPlayer(y.simulation.Physics(), y.simulation.Terrain(), character, walker, walk, tuning, sim::PlayArea{},
                        y.simulation.Config().fixedTimeStep);
        sim::StepHeld(y.simulation, crate, holder, 0, -64, y.Env(), tick, true);   // held low, across the walker's capsule
        y.simulation.Step(focus);
    }
    EXPECT_GT(1.0f - walker.position.z, 3.0f) << "the walker passed through the held crate";

    // 3. Rays: held level again, only a query that asks for Layer::Held alone sees it.
    sim::StepHeld(y.simulation, crate, holder, 0, 0, y.Env(), y.simulation.Tick(), true);
    y.simulation.StepPhysics();
    const glm::vec3 from = glm::vec3(kHoldEye) - glm::vec3(0.0f, 0.5f, 0.0f);
    EXPECT_FALSE(y.simulation.Physics().Raycast(from, kHoldLook, 1.2f).hit) << "the default mask does not see it";
    const physics::RayHit seen = y.simulation.Physics().Raycast(from, kHoldLook, 5.0f, physics::LayerBit(physics::Layer::Held));
    EXPECT_TRUE(seen.hit);
    EXPECT_EQ(seen.body, y.Body(crate));
}

TEST(Hold, DropKeepsPoseAndHolderVelocity) {
    for (const bool replicated : {false, true}) {
        HoldYard y;
        ASSERT_TRUE(y.initialized);
        const Entity crate = y.Crate({0.0, 13.0, -1.5});
        sim::HoldChange c;
        ASSERT_EQ(sim::BeginHold(y.simulation, crate, 0, 0, 10, 0, false, false, false, c), sim::UseResult::None);
        sim::MechanismPose pose;
        pose.position = {-0.4, 12.6, -1.9};
        const glm::dvec3 anchor = sim::HoldAnchor({0.0f, 11.5f, 0.0f}, sim::PlayerTuning{});
        sim::EndHold(y.simulation, crate, sim::HoldEnd::Drop, pose, {2.0f, 0.0f, 0.0f}, &anchor, y.Env(), 20, replicated, c);
        EXPECT_EQ(y.Scene().TryGet<scene::Held>(crate), nullptr);
        EXPECT_EQ(y.simulation.Physics().GetLayer(y.Body(crate)), replicated ? physics::Layer::Rubble : physics::Layer::Dynamic);
        EXPECT_EQ(y.simulation.Physics().GetState(y.Body(crate)).linearVelocity, glm::vec3(2.0f, 0.0f, 0.0f));
        EXPECT_EQ(y.Scene().TryGet<scene::Transform>(crate)->current.position, glm::vec3(pose.position));
        EXPECT_FALSE(c.hold);
        EXPECT_EQ(c.reason, 1u);
        EXPECT_EQ(c.tick, 20u);
    }
}

TEST(Hold, ThrowAddsSixAlongTheLook) {
    const glm::vec3 a = sim::ReleaseVelocity(sim::HoldEnd::Throw, {1.0f, 0.0f, 0.0f}, 0, 0);
    EXPECT_NEAR(a.x, 1.0f, 1.0e-5f);
    EXPECT_NEAR(a.y, 0.0f, 1.0e-5f);
    EXPECT_NEAR(a.z, -6.0f, 1.0e-5f);
    const glm::vec3 b = sim::ReleaseVelocity(sim::HoldEnd::Throw, {1.0f, 0.0f, 0.0f}, 0, 32767);
    EXPECT_NEAR(b.x, 1.0f, 1.0e-4f);
    EXPECT_NEAR(b.y, 6.0f, 1.0e-4f);
    EXPECT_NEAR(b.z, 0.0f, 1.0e-4f);
    EXPECT_EQ(sim::ReleaseVelocity(sim::HoldEnd::Drop, {1.0f, 2.0f, 3.0f}, 0, 0), glm::vec3(1.0f, 2.0f, 3.0f));
}

TEST(Hold, ReleaseStepsOutOfAWall) {
    for (const bool fix : {true, false}) {
        HoldYard y;
        ASSERT_TRUE(y.initialized);
        const Entity crate = y.Crate({3.0, 10.4, 3.0});
        sim::HoldChange c;
        ASSERT_EQ(sim::BeginHold(y.simulation, crate, 0, 0, 10, 0, false, false, false, c), sim::UseResult::None);
        y.Stone({-2.0, 10.0, -2.2}, {2.0, 13.0, -1.0});
        const sim::HeldPose p = sim::HeldPoseAt(y.simulation, crate, kHoldFeet, 0, 0, y.Env(), 10);
        ASSERT_TRUE(p.pinned);
        const glm::dvec3 anchor = sim::HoldAnchor(kHoldFeet, sim::PlayerTuning{});
        sim::EndHold(y.simulation, crate, sim::HoldEnd::Pinned, p.pose, glm::vec3(0.0f), fix ? &anchor : nullptr, y.Env(), 10, false, c);
        if (fix)
            EXPECT_NEAR(static_cast<double>(c.position.z) + 0.4, -0.59282, 1.0e-3) << "two steps of 0.1 m back";
        else
            EXPECT_NEAR(c.position.z, static_cast<float>(p.pose.position.z), 1.0e-6f) << "no fix-up without an anchor";
    }
}

TEST(Hold, ReleaseAvoidsBodiesAndPlayers) {
    {
        HoldYard y;
        ASSERT_TRUE(y.initialized);
        y.Crate({0.0, 11.12, -1.5});                         // a second crate, in Layer::Dynamic
        const Entity crate = y.Crate({3.0, 10.4, 3.0});
        sim::HoldChange c;
        ASSERT_EQ(sim::BeginHold(y.simulation, crate, 0, 0, 10, 0, false, false, false, c), sim::UseResult::None);
        const sim::HeldPose p = sim::HeldPoseAt(y.simulation, crate, kHoldFeet, 0, 0, y.Env(), 10);
        const glm::dvec3 anchor = sim::HoldAnchor(kHoldFeet, sim::PlayerTuning{});
        sim::EndHold(y.simulation, crate, sim::HoldEnd::Drop, p.pose, glm::vec3(0.0f), &anchor, y.Env(), 10, false, c);
        EXPECT_GE(static_cast<double>(c.position.z) + 0.4, -0.7 - 1.0e-3);
    }
    {
        HoldYard y;
        ASSERT_TRUE(y.initialized);
        const Entity crate = y.Crate({3.0, 10.4, 3.0});
        sim::HoldChange c;
        ASSERT_EQ(sim::BeginHold(y.simulation, crate, 0, 0, 10, 0, false, false, false, c), sim::UseResult::None);
        const std::array<glm::vec3, 1> feet = {glm::vec3(0.0f, 10.5f, -1.5f)};
        sim::HoldEnv env = y.Env();
        env.playerFeet = feet;
        const sim::HeldPose p = sim::HeldPoseAt(y.simulation, crate, kHoldFeet, 0, 0, env, 10);
        const glm::dvec3 anchor = sim::HoldAnchor(kHoldFeet, sim::PlayerTuning{});
        sim::EndHold(y.simulation, crate, sim::HoldEnd::Drop, p.pose, glm::vec3(0.0f), &anchor, env, 10, false, c);
        const math::AABB capsule = sim::PlayerBounds(feet[0], sim::PlayerTuning{});
        EXPECT_GE(c.position.z, capsule.max.z - 1.0e-4f) << "stepped back until the box clears the capsule";
    }
}

TEST(Hold, PinnedForThirtyTicksDrops) {
    for (const bool lookAway : {false, true}) {
        HoldYard y;
        ASSERT_TRUE(y.initialized);
        const Entity crate = y.Crate({3.0, 10.4, 3.0});
        sim::HoldChange c;
        ASSERT_EQ(sim::BeginHold(y.simulation, crate, 0, 0, 10, 0, false, false, false, c), sim::UseResult::None);
        y.Stone({-2.0, 10.0, -2.2}, {2.0, 13.0, -1.0});
        sim::PlayerState holder;
        holder.position = kHoldFeet;
        const int calls = lookAway ? 45 : 30;
        for (int call = 1; call <= calls; ++call) {
            const uint16_t yaw = (lookAway && call == 15) ? uint16_t{32768} : uint16_t{0};
            const bool drop = sim::StepHeld(y.simulation, crate, holder, yaw, 0, y.Env(), static_cast<uint32_t>(10 + call), true);
            EXPECT_EQ(drop, call == calls) << "call " << call;
        }
    }
}

TEST(Hold, BusyAfterAChange) {
    HoldYard y;
    ASSERT_TRUE(y.initialized);
    const Entity crate = y.Crate({0.0, 10.4, -1.5});
    sim::HoldChange c;
    EXPECT_EQ(sim::BeginHold(y.simulation, crate, 0, 0, 100, 100 - 11, true, false, false, c), sim::UseResult::Busy);
    EXPECT_EQ(sim::BeginHold(y.simulation, crate, 0, 0, 100, 100 - 12, true, false, false, c), sim::UseResult::None);
    const Entity other = y.Crate({2.0, 10.4, -1.5});
    EXPECT_EQ(sim::BeginHold(y.simulation, other, 1, 0, 100, 100, false, false, false, c), sim::UseResult::None);
}

TEST(Hold, DamageReleasesFirst) {
    HoldYard y;
    ASSERT_TRUE(y.initialized);
    const Entity crate = y.Crate({0.0, 11.12, -1.5});
    sim::HoldChange c;
    ASSERT_EQ(sim::BeginHold(y.simulation, crate, 0, 0, 10, 0, false, false, false, c), sim::UseResult::None);
    const sim::BlastResult blast = sim::Blast(y.simulation, sim::BlastDesc{y.CentreOf(crate), 1.2f, 6.0f});
    ASSERT_EQ(blast.holdChanges.size(), 1u);
    EXPECT_FALSE(blast.holdChanges[0].hold);
    EXPECT_EQ(blast.holdChanges[0].reason, static_cast<uint8_t>(sim::HoldEnd::Damaged));
    if (y.Scene().IsValid(crate)) {
        EXPECT_EQ(y.Scene().TryGet<scene::Held>(crate), nullptr);
        EXPECT_EQ(y.Scene().TryGet<scene::RigidBody>(crate)->motion, physics::MotionType::Dynamic);
    }
}

TEST(Hold, PushDoesNotReleaseOrMove) {
    HoldYard y;
    ASSERT_TRUE(y.initialized);
    const Entity crate = y.Crate({3.0, 10.4, 3.0});
    sim::HoldChange c;
    ASSERT_EQ(sim::BeginHold(y.simulation, crate, 0, 0, 10, 0, false, false, false, c), sim::UseResult::None);
    sim::PlayerState holder;
    holder.position = kHoldFeet;
    sim::StepHeld(y.simulation, crate, holder, 0, 0, y.Env(), 10, true);
    y.simulation.StepPhysics();
    const uint32_t solid = y.Scene().TryGet<scene::VoxelBody>(crate)->model->SolidCount();

    // 2.0 m beside the crate's centre: the sphere's box misses the crate, the push reaches it.
    const sim::BlastResult far = sim::Blast(y.simulation, sim::BlastDesc{glm::dvec3(2.0, 11.12, -1.5), 1.0f, 2.0f});
    EXPECT_TRUE(far.holdChanges.empty());
    EXPECT_NE(y.Scene().TryGet<scene::Held>(crate), nullptr);
    // Off a corner: the sphere's box meets the crate's bounds, the sphere misses its model box.
    const sim::BlastResult corner = sim::Blast(y.simulation, sim::BlastDesc{glm::dvec3(0.85, 11.97, -0.65), 0.6f, 2.0f});
    EXPECT_TRUE(corner.holdChanges.empty());
    EXPECT_NE(y.Scene().TryGet<scene::Held>(crate), nullptr);
    EXPECT_EQ(y.Scene().TryGet<scene::VoxelBody>(crate)->model->SolidCount(), solid);

    sim::StepHeld(y.simulation, crate, holder, 0, 0, y.Env(), 11, true);
    y.simulation.StepPhysics();
    const sim::HeldPose p = sim::HeldPoseAt(y.simulation, crate, kHoldFeet, 0, 0, y.Env(), 11);
    const glm::vec3 at = y.Scene().TryGet<scene::Transform>(crate)->current.position;
    EXPECT_NEAR(at.x, static_cast<float>(p.pose.position.x), 1.0e-4f);
    EXPECT_NEAR(at.y, static_cast<float>(p.pose.position.y), 1.0e-4f);
    EXPECT_NEAR(at.z, static_cast<float>(p.pose.position.z), 1.0e-4f);
}

TEST(Hold, CrushSkipsAHeldBody) {
    HoldYard y;
    ASSERT_TRUE(y.initialized);
    // The door at x -0.4..0.4, z -1.6..-1.3, open, closing from tick 20; the held crate lies in its block box.
    const Entity door = HoldYardDoor(y, {-4, 100, -16}, 1024);
    ASSERT_NE(door, kNoEntity);
    sim::PushSegment(y.Scene().TryGet<scene::Mechanism>(door)->track, {20, 1024, 0, 36, 0});
    const Entity crate = y.Crate({0.0, 10.4, -1.45});
    sim::HoldChange c;
    ASSERT_EQ(sim::BeginHold(y.simulation, crate, 0, 0, 10, 0, false, false, false, c), sim::UseResult::None);
    uint32_t crushed = 0;
    for (uint32_t tick = 20; tick <= 60; ++tick) crushed += sim::StepMechanisms(y.simulation, tick, true).crushed;
    EXPECT_EQ(crushed, 0u);
    EXPECT_TRUE(y.Scene().IsValid(crate));

    sim::MechanismPose here;
    here.position = glm::dvec3(y.Scene().TryGet<scene::Transform>(crate)->current.position);
    sim::EndHold(y.simulation, crate, sim::HoldEnd::Drop, here, glm::vec3(0.0f), nullptr, y.Env(), 60, false, c);
    sim::PushSegment(y.Scene().TryGet<scene::Mechanism>(door)->track, {80, 0, 1024, 36, 0});
    sim::PushSegment(y.Scene().TryGet<scene::Mechanism>(door)->track, {140, 1024, 0, 36, 0});
    for (uint32_t tick = 140; tick <= 176; ++tick) crushed += sim::StepMechanisms(y.simulation, tick, true).crushed;
    EXPECT_EQ(crushed, 1u) << "released, it is crushed at the arrival";
}

TEST(Hold, TwoSimulationsAgree) {
    struct HoldRun {
        std::vector<glm::vec3> positions;
        std::vector<glm::quat> rotations;
        sim::HoldChange release;
    };
    const auto run = [] {
        HoldRun out;
        HoldYard y;
        EXPECT_TRUE(y.initialized);
        const Entity crate = y.Crate({0.0, 10.4, -1.5});
        sim::HoldChange c;
        EXPECT_EQ(sim::BeginHold(y.simulation, crate, 0, 0, 10, 0, false, false, false, c), sim::UseResult::None);
        sim::PlayerState holder;
        holder.position = kHoldFeet;
        uint16_t yaw = 0;
        for (int i = 0; i < 120; ++i) {
            holder.position.z -= 0.05f;
            yaw = static_cast<uint16_t>(yaw + 200);
            const int16_t pitch = static_cast<int16_t>(-20000 + (40000 * i) / 119);
            sim::StepHeld(y.simulation, crate, holder, yaw, sim::HoldPitch(pitch), y.Env(), static_cast<uint32_t>(10 + i), true);
            y.simulation.StepPhysics();
            const scene::Transform* t = y.Scene().TryGet<scene::Transform>(crate);
            out.positions.push_back(t->current.position);
            out.rotations.push_back(t->current.rotation);
        }
        const sim::HeldPose p = sim::HeldPoseAt(y.simulation, crate, holder.position, yaw, 0, y.Env(), 130);
        const glm::dvec3 anchor = sim::HoldAnchor(holder.position, sim::PlayerTuning{});
        sim::EndHold(y.simulation, crate, sim::HoldEnd::Throw, p.pose, sim::ReleaseVelocity(sim::HoldEnd::Throw, glm::vec3(0.0f), yaw, 0),
                     &anchor, y.Env(), 130, false, out.release);
        return out;
    };
    const HoldRun a = run();
    const HoldRun b = run();
    ASSERT_EQ(a.positions.size(), b.positions.size());
    for (size_t i = 0; i < a.positions.size(); ++i) {
        EXPECT_EQ(a.positions[i], b.positions[i]) << i;
        EXPECT_EQ(a.rotations[i], b.rotations[i]) << i;
    }
    EXPECT_EQ(a.release.position, b.release.position);
    EXPECT_EQ(a.release.velocity, b.release.velocity);
    EXPECT_EQ(a.release.reason, b.release.reason);
}
