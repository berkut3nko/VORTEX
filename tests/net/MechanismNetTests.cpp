// Mechanisms and holding on the wire ([mech-1] card 8, docs/design/mechanisms.md "Replication", "One use key: E"):
// the mechanism spawn, MechanismMove, EntityHold, EntityRelease, the held spawn, the owner block's use result and the
// remote record's hold pitch. Sizes, round trips and refusals.

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <variant>
#include <vector>

import vortex.core;
import vortex.voxel;
import vortex.physics;
import vortex.scene;
import vortex.sim;
import vortex.net;

using namespace vortex;

namespace {
    std::vector<uint8_t> Bytes(const net::Event& event) {
        net::ByteWriter w;
        net::WriteEvent(w, event);
        return w.Take();
    }

    bool Decodes(std::span<const uint8_t> bytes, net::Event& out) {
        net::ByteReader in(bytes);
        return net::ReadEvent(in, out) && in.Remaining() == 0;
    }

    net::EntitySpawn MakeObjectSpawn(net::EntityKind kind) {
        net::EntitySpawn spawn;
        spawn.netId = 7;
        spawn.kind = kind;
        spawn.object.position = {1.0f, 2.0f, 3.0f};
        spawn.object.motion = 1;
        voxel::VoxelVolume volume(glm::ivec3(8, 16, 3));
        volume.FillBox(glm::ivec3(0), glm::ivec3(8, 16, 3), 9);
        spawn.object.volume = std::move(volume);
        return spawn;
    }

    net::MechanismState MakeDoorState(size_t segments) {
        net::MechanismState m;
        scene::MechanismDesc& d = m.desc;
        d.kind = scene::MechanismKind::Door;
        d.flags = scene::MechanismDesc_Usable;
        d.pivotQ = {0, 0, 24};
        d.axis = 2;
        d.minPos = -1024;
        d.maxPos = 1024;
        d.openTicks = d.closeTicks = 72;
        d.useBox = d.blockBox = {glm::ivec3(0), glm::ivec3(8, 16, 3)};
        d.hingeBox = {glm::ivec3(0), glm::ivec3(1, 16, 3)};
        d.pushAxis = 4;
        d.anchorCount = 2;
        d.anchors = {glm::ivec3(-1, 2, 1), glm::ivec3(-1, 13, 1)};
        m.spawnVoxel = {-36, 0, -90};
        m.quarterTurns = 1;
        m.modelOffset = {0, 0, 0};
        m.linkedNetId = 0;
        for (size_t i = 0; i < segments; ++i)
            m.segments.push_back({static_cast<uint32_t>(100 + i * 20), 0, static_cast<int16_t>(i % 2 ? 0 : 1024), 72, 0});
        return m;
    }
}

TEST(Mechanisms, WireSizes) {
    // MechanismMove: 16 B, round trip.
    const net::MechanismMove move{42, {300, 0, 1024, 36, scene::MechanismFlag_Locked}};
    EXPECT_EQ(Bytes(move).size(), 16u);
    net::Event back;
    ASSERT_TRUE(Decodes(Bytes(move), back));
    EXPECT_EQ(std::get<net::MechanismMove>(back), move);

    // A mechanism spawn: the object's spawn + 63 + 24 + 11 per segment.
    const size_t object = Bytes(MakeObjectSpawn(net::EntityKind::Object)).size();
    for (const size_t segments : {size_t{1}, size_t{8}}) {
        net::EntitySpawn spawn = MakeObjectSpawn(net::EntityKind::Mechanism);
        spawn.mechanism = MakeDoorState(segments);
        const std::vector<uint8_t> bytes = Bytes(spawn);
        EXPECT_EQ(bytes.size(), object + 63 + 24 + 11 * segments) << segments;
        ASSERT_TRUE(Decodes(bytes, back)) << segments;
        EXPECT_TRUE(std::get<net::EntitySpawn>(back) == spawn) << segments;
    }

    // Refused: a segment count of 0 and 9, a mechanism kind of 4, one byte missing and one trailing.
    {
        net::EntitySpawn spawn = MakeObjectSpawn(net::EntityKind::Mechanism);
        spawn.mechanism = MakeDoorState(1);
        std::vector<uint8_t> good = Bytes(spawn);
        const size_t descAt = object;                  // the desc starts right after the object's bytes
        const size_t countAt = descAt + 63;
        std::vector<uint8_t> bad = good;
        bad[countAt] = 0;
        EXPECT_FALSE(Decodes(bad, back)) << "count 0";
        bad = good;
        bad[countAt] = 9;
        EXPECT_FALSE(Decodes(bad, back)) << "count 9";
        bad = good;
        bad[descAt] = 4;
        EXPECT_FALSE(Decodes(bad, back)) << "kind 4";
        bad.assign(good.begin(), good.end() - 1);
        EXPECT_FALSE(Decodes(bad, back)) << "one byte missing";
        bad = good;
        bad.push_back(0);
        EXPECT_FALSE(Decodes(bad, back)) << "one trailing byte";
    }

    // EntityHold 12 B, EntityRelease 44 B; round trips (the release velocity as QuantizeVelocity gives it).
    const net::EntityHold hold{42, 3, 16384, 500};
    EXPECT_EQ(Bytes(hold).size(), 12u);
    ASSERT_TRUE(Decodes(Bytes(hold), back));
    EXPECT_EQ(std::get<net::EntityHold>(back), hold);
    net::EntityRelease release{42, 510, static_cast<uint8_t>(sim::HoldEnd::Throw), {1.5f, 2.0f, -3.0f},
                               glm::angleAxis(0.5f, glm::vec3(0.0f, 1.0f, 0.0f)), {1.0f, 0.3f, -6.0f}};
    EXPECT_EQ(Bytes(release).size(), 44u);
    ASSERT_TRUE(Decodes(Bytes(release), back));
    net::EntityRelease expected = release;
    expected.velocity = net::QuantizeVelocity(release.velocity);
    EXPECT_EQ(std::get<net::EntityRelease>(back), expected);
    for (const uint8_t reason : {uint8_t{0}, uint8_t{7}}) {
        net::EntityRelease r = release;
        r.reason = reason;
        EXPECT_FALSE(Decodes(Bytes(r), back)) << "reason " << int(reason);
    }

    // A held object's spawn: its unheld spawn + 7; never held and resting at once.
    net::EntitySpawn held = MakeObjectSpawn(net::EntityKind::Object);
    held.object.flags = net::kObjectHeld;
    held.object.holder = 2;
    held.object.relYaw = 100;
    held.object.holdSince = 77;
    EXPECT_EQ(Bytes(held).size(), object + 7);
    ASSERT_TRUE(Decodes(Bytes(held), back));
    EXPECT_TRUE(std::get<net::EntitySpawn>(back) == held);
    held.object.flags = net::kObjectHeld | net::kObjectResting;
    EXPECT_FALSE(Decodes(Bytes(held), back)) << "held and resting";

    // HoldEvent turns a sim::HoldChange into the right event.
    sim::HoldChange change;
    change.hold = true;
    change.holder = 1;
    change.relYaw = 5;
    change.tick = 9;
    EXPECT_EQ(std::get<net::EntityHold>(net::HoldEvent(change, 42)), (net::EntityHold{42, 1, 5, 9}));
    change.hold = false;
    change.reason = static_cast<uint8_t>(sim::HoldEnd::Drop);
    EXPECT_TRUE(std::holds_alternative<net::EntityRelease>(net::HoldEvent(change, 42)));
}

TEST(Mechanisms, SnapshotCarriesTheUseResultAndTheHoldPitch) {
    EXPECT_EQ(net::kSnapshotOwnerBytes, 44u) << "[wp-7]'s 39 + lastUseTick 4 + useResult 1";
    EXPECT_EQ(net::kSnapshotPlayerBytes, 18u) << "[wp-7]'s 17 + holdPitch 1";
    EXPECT_EQ(net::kProtocolVersion, 12u);

    net::Snapshot s;
    s.tick = 10;
    s.hasOwner = true;
    s.lastUseTick = 1234;
    s.useResult = sim::UseResult::Locked;
    net::RemotePlayer other;
    other.slot = 3;
    other.holdPitch = -64;
    s.others.push_back(other);
    std::vector<uint8_t> bytes = net::Encode(s);
    net::Snapshot back;
    ASSERT_TRUE(net::Decode(bytes, back));
    EXPECT_EQ(back.lastUseTick, 1234u);
    EXPECT_EQ(back.useResult, sim::UseResult::Locked);
    ASSERT_EQ(back.others.size(), 1u);
    EXPECT_EQ(back.others[0], other);

    // Without the owner block and the player: the owner part is kSnapshotOwnerBytes, a player kSnapshotPlayerBytes.
    net::Snapshot bare = s;
    bare.hasOwner = false;
    bare.others.clear();
    net::Snapshot onePlayer = bare;
    onePlayer.others.push_back(other);
    EXPECT_EQ(net::Encode(s).size() - net::Encode(onePlayer).size(), net::kSnapshotOwnerBytes);
    EXPECT_EQ(net::Encode(onePlayer).size() - net::Encode(bare).size(), net::kSnapshotPlayerBytes);

    // Refused: a use result above kMaxUseResult, a hold pitch of 65 and -65.
    const size_t ownerEnd = net::kSnapshotHeaderBytes - 2 + net::kSnapshotOwnerBytes;   // header without the two counts
    std::vector<uint8_t> bad = bytes;
    bad[ownerEnd - 1] = 7;
    EXPECT_FALSE(net::Decode(bad, back)) << "use result 7";
    for (const int8_t pitch : {int8_t{65}, int8_t{-65}}) {
        net::Snapshot p = s;
        p.others[0].holdPitch = pitch;
        EXPECT_FALSE(net::Decode(net::Encode(p), back)) << int(pitch);
    }
}

// --- the server: mechanisms and holding in a session ([mech-1] card 9) ------------------------------------------

namespace {
    constexpr int kYardGround = 96;               // flat ground: voxels y < 96 (12 m at 0.125 m)
    constexpr float kYardVoxel = 0.125f;

    net::Token YardToken() {
        net::Token t{};
        for (size_t i = 0; i < t.size(); ++i) t[i] = static_cast<uint8_t>(0x51 + i * 3);
        return t;
    }

    voxel::WorldMaterials YardIds() {
        voxel::MaterialLibrary scratch;
        return voxel::RegisterDefaultWorldMaterials(scratch);
    }

    sim::RegionDesc YardRegion(sim::RegionTemplateId templateId) {
        sim::RegionDesc r;
        r.templateId = templateId;
        r.seed = 515;
        r.playMinChunk = {-7, -7};
        r.playMaxChunk = {7, 7};
        r.heightRange = {0.0f, 32.0f};
        r.generator = sim::RegionGenerator::Flat;
        r.flatGroundVoxels = kYardGround;
        r.terrain.voxelSize = kYardVoxel;
        r.terrain.materials = YardIds();
        return r;
    }

    /// The yard (a playground with `mechanisms = 1`), or the empty template for the holding tests.
    net::ServerSettings YardSettings(bool yard) {
        net::ServerSettings s;
        s.session.token = YardToken();
        s.session.region = YardRegion(yard ? sim::RegionTemplateId::Playground : sim::RegionTemplateId::Empty);
        s.session.packHidden = false;
        s.session.mechanisms = yard;
        s.simulation.jobThreads = 2;
        s.simulation.physics.threads = 1;
        s.simulation.physics.maxBodies = 2048;
        s.simulation.physics.maxBodyPairs = 4096;
        s.simulation.physics.maxContactConstraints = 4096;
        return s;
    }

    /// A peer that speaks the protocol by hand: it sends inputs, and keeps every event (with its opSeq), snapshot and
    /// baseline entity the server sent it.
    struct YardPeer {
        net::LoopbackTransport loop;
        net::PeerId server = 0;
        bool disconnected = false;
        bool welcomed = false;
        net::Welcome welcome;
        std::vector<std::pair<uint32_t, net::Event>> events;
        std::vector<net::Snapshot> snapshots;
        std::vector<net::BaselineEntity> baseline;

        void Poll() {
            net::TransportEvent e;
            while (loop.Poll(e)) {
                if (e.type == net::TransportEvent::Type::Connected) server = e.peer;
                if (e.type == net::TransportEvent::Type::Disconnected) disconnected = true;
                if (e.type != net::TransportEvent::Type::Received) continue;
                switch (net::PeekType(e.data).value_or(net::MessageType{})) {
                    case net::MessageType::Welcome: welcomed = net::Decode(e.data, welcome); break;
                    case net::MessageType::EventBatch: {
                        net::EventBatch b;
                        EXPECT_TRUE(net::Decode(e.data, b));
                        for (size_t i = 0; i < b.events.size(); ++i) events.emplace_back(b.firstOpSeq + static_cast<uint32_t>(i), b.events[i]);
                        break;
                    }
                    case net::MessageType::Snapshot: {
                        net::Snapshot s;
                        EXPECT_TRUE(net::Decode(e.data, s));
                        snapshots.push_back(std::move(s));
                        break;
                    }
                    case net::MessageType::BaselineEntity: {
                        net::BaselineEntity b;
                        EXPECT_TRUE(net::Decode(e.data, b));
                        baseline.push_back(std::move(b));
                        break;
                    }
                    default: break;
                }
            }
        }
        void Send(net::Channel channel, const std::vector<uint8_t>& bytes) { loop.Send(server, channel, bytes); }
        void Input(const sim::PlayerInput& in) {
            net::InputBatch b;
            b.firstTick = in.tick;
            b.inputs = {in};
            Send(net::Channel::State, net::Encode(b));
        }
        template <typename T>
        std::vector<T> Events() const {
            std::vector<T> out;
            for (const auto& [seq, e] : events)
                if (const T* t = std::get_if<T>(&e)) out.push_back(*t);
            return out;
        }
        uint8_t Slot() const { return welcome.slot; }
    };

    /// A server on a loopback transport and its raw peers; Step() is one server tick, each peer's input (if it has
    /// controls) sent for the tick the server simulates next.
    struct YardServer {
        net::LoopbackTransport serverLoop;
        net::Server server;
        bool started = false;
        std::vector<std::unique_ptr<YardPeer>> peers;
        std::vector<std::function<std::optional<sim::PlayerInput>()>> controls;

        explicit YardServer(net::ServerSettings settings) : server(std::move(settings), serverLoop) { started = server.Start(); }

        YardPeer& Join() {
            peers.push_back(std::make_unique<YardPeer>());
            controls.emplace_back();
            YardPeer& p = *peers.back();
            net::LoopbackTransport::Connect(serverLoop, p.loop);
            p.Poll();
            p.Send(net::Channel::Events, net::Encode(net::Hello{net::kProtocolVersion, server.BuildHashValue(), YardToken()}));
            for (int i = 0; i < 10 && !p.welcomed; ++i) Tick();
            EXPECT_TRUE(p.welcomed);
            return p;
        }

        void Tick() {
            server.Tick();
            if (server.Simulation().Terrain().Stats().pending != 0) server.Simulation().Jobs().WaitIdle();
            for (const std::unique_ptr<YardPeer>& p : peers) p->Poll();
        }

        void Step(int ticks = 1) {
            for (int t = 0; t < ticks; ++t) {
                for (size_t i = 0; i < peers.size(); ++i) {
                    if (!controls[i] || peers[i]->disconnected) continue;
                    if (std::optional<sim::PlayerInput> in = controls[i]()) {
                        in->tick = server.TickNumber();
                        peers[i]->Input(*in);
                    }
                }
                Tick();
            }
        }

        sim::Simulation& Sim() { return server.Simulation(); }
        glm::vec3 Feet(const YardPeer& p) { return server.GetPlayer(p.Slot())->position; }
    };

    float EyeHeight() { return net::ServerSettings{}.player.eyeHeight; }

    /// The look from `feet`'s eye at `target`.
    sim::PlayerInput LookAt(const glm::vec3& feet, const glm::dvec3& target) {
        const glm::dvec3 eye = glm::dvec3(feet) + glm::dvec3(0.0, static_cast<double>(EyeHeight()), 0.0);
        const glm::dvec3 d = glm::normalize(target - eye);
        sim::PlayerInput in;
        in.yaw = sim::QuantizeYaw(static_cast<float>(std::atan2(-d.x, -d.z)));
        in.pitch = sim::QuantizePitch(static_cast<float>(std::asin(std::clamp(d.y, -1.0, 1.0))));
        return in;
    }

    glm::dvec3 BodyCentre(sim::Simulation& simulation, scene::Entity e) {
        const scene::Transform& t = *simulation.Scene().TryGet<scene::Transform>(e);
        const glm::vec3 size = glm::vec3(simulation.Scene().TryGet<scene::VoxelBody>(e)->model->Volume().Size()) * simulation.World().VoxelSize();
        return glm::dvec3(t.current.position + t.current.rotation * (0.5f * size));
    }

    scene::Entity EntityOfNetId(sim::Simulation& simulation, uint32_t netId) {
        for (const auto [e, id] : simulation.Scene().Registry().view<const scene::NetId>().each())
            if (id.value == netId) return e;
        return scene::kNoEntity;
    }

    uint32_t NetIdOfEntity(sim::Simulation& simulation, scene::Entity e) {
        const scene::NetId* id = simulation.Scene().TryGet<scene::NetId>(e);
        return id ? id->value : 0;
    }

    scene::Entity FindMechanism(sim::Simulation& simulation, scene::MechanismKind kind) {
        for (const auto [e, m] : simulation.Scene().Registry().view<const scene::Mechanism>().each())
            if (m.desc.kind == kind) return e;
        return scene::kNoEntity;
    }

    /// The world centre of a mechanism's use box now.
    glm::dvec3 UseBoxCentre(sim::Simulation& simulation, scene::Entity e) {
        const scene::Mechanism& m = *simulation.Scene().TryGet<scene::Mechanism>(e);
        const float vs = simulation.World().VoxelSize();
        const sim::MechanismPose pose = sim::PoseAt(m, sim::PositionAt(m, simulation.Tick()), vs);
        const glm::vec3 local = (glm::vec3(m.desc.useBox.min + m.desc.useBox.max) * 0.5f - glm::vec3(m.modelOffset)) * vs;
        return pose.position + glm::dvec3(pose.rotation * local);
    }

    /// Puts `p`'s player where its eye's look at `target` picks `entity` (FindUseTarget on the server's world) and
    /// returns that look; the peer's controls hold it. False when no spot around the target works.
    bool StandAt(YardServer& y, YardPeer& p, size_t index, scene::Entity entity, const glm::dvec3& target, sim::PlayerInput& look) {
        voxel::World& world = y.Sim().World();
        for (const double r : {1.4, 1.8, 1.1})
            for (int k = 0; k < 16; ++k) {
                const double a = 6.283185307179586 * k / 16.0;
                const glm::dvec3 at = target + glm::dvec3(std::cos(a) * r, 0.0, std::sin(a) * r);
                const voxel::WorldRayHit ground = world.Raycast(glm::dvec3(at.x, target.y + 1.0, at.z), glm::vec3(0.0f, -1.0f, 0.0f), 6.0f);
                if (!ground.hit) continue;
                const glm::vec3 feet(static_cast<float>(at.x), static_cast<float>(ground.voxel.y + 1) * world.VoxelSize() + 0.02f,
                                     static_cast<float>(at.z));
                if (!y.server.TeleportPlayer(p.Slot(), feet)) return false;
                y.controls[index] = [&y, &p, target] { return std::optional<sim::PlayerInput>(LookAt(y.Feet(p), target)); };
                y.Step(10);
                const glm::vec3 now = y.Feet(p);
                if (glm::distance(now, feet) > 0.3f) continue;
                look = LookAt(now, target);
                sim::UseTarget t;
                const glm::dvec3 eye = glm::dvec3(now) + glm::dvec3(0.0, static_cast<double>(EyeHeight()), 0.0);
                if (sim::FindUseTarget(y.Sim(), eye, sim::LookDirection(look.yaw, look.pitch), y.Sim().Tick(), t) && t.entity == entity)
                    return true;
            }
        return false;
    }

    /// A crate on the flat ground at `centre` (x, z), at rest after the ticks it took.
    uint32_t RestingCrate(YardServer& y, double x, double z) {
        const double half = 0.375;
        const uint32_t netId = y.server.SpawnCrate(glm::dvec3(x - half, static_cast<double>(kYardGround) * kYardVoxel + 0.05, z - half));
        for (int i = 0; i < 600 && y.server.AwakeBodies() != 0; ++i) y.Step();
        return netId;
    }

    /// One press of `buttons` on top of `look` (the edge goes with the next Step; the controls then hold the look).
    void Press(YardServer& y, size_t index, sim::PlayerInput look, uint8_t buttons) {
        y.controls[index] = [look, buttons, first = true]() mutable {
            sim::PlayerInput in = look;
            in.buttons = first ? buttons : 0;
            first = false;
            return std::optional<sim::PlayerInput>(in);
        };
        y.Step(2);
    }

    const net::Snapshot& LastSnapshot(const YardPeer& p) { return p.snapshots.back(); }

    /// The player at (0, ground, 0) facing -z, the crate at (0, -2): picked up. Returns the crate's NetId.
    uint32_t PickUpACrate(YardServer& y, YardPeer& p, size_t index, sim::PlayerInput& look) {
        const glm::vec3 feet(0.0f, static_cast<float>(kYardGround) * kYardVoxel + 0.02f, 0.0f);
        EXPECT_TRUE(y.server.TeleportPlayer(p.Slot(), feet));
        y.controls[index] = [] { return std::optional<sim::PlayerInput>(sim::PlayerInput{}); };
        y.Step(5);
        const uint32_t crate = RestingCrate(y, 0.0, -2.0);
        EXPECT_NE(crate, 0u);
        const scene::Entity e = EntityOfNetId(y.Sim(), crate);
        EXPECT_TRUE(sim::IsHoldable(y.Sim(), e, true));
        look = LookAt(y.Feet(p), BodyCentre(y.Sim(), e));
        Press(y, index, look, sim::PlayerButton_Use);
        return crate;
    }
}

TEST(Use, RefusalReachesTheUser) {
    YardServer y(YardSettings(false));
    ASSERT_TRUE(y.started);
    YardPeer& p = y.Join();
    y.controls[0] = [] { return std::optional<sim::PlayerInput>(sim::PlayerInput{}); };
    y.Step(10);
    // Looking at the sky: NoTarget for the edge's input tick.
    sim::PlayerInput sky;
    sky.pitch = 30000;
    const uint32_t pressTick = y.server.TickNumber();
    Press(y, 0, sky, sim::PlayerButton_Use);
    y.Step(4);
    ASSERT_FALSE(p.snapshots.empty());
    EXPECT_EQ(LastSnapshot(p).lastUseTick, pressTick);
    EXPECT_EQ(LastSnapshot(p).useResult, sim::UseResult::NoTarget);
    // Late inputs more than kUseStaleTicks old: Stale (31 ticks, still in the state history; 40, no longer kept).
    for (const uint32_t age : {31u, 40u}) {
        y.controls[0] = nullptr;
        y.Step(static_cast<int>(age) + 2);
        sim::PlayerInput late = sky;
        late.tick = y.server.TickNumber() - age;
        late.buttons = sim::PlayerButton_Use;
        p.Input(late);
        y.Tick();
        const uint32_t useTick = late.tick;
        late.tick += 1;
        late.buttons = 0;
        p.Input(late);
        for (int i = 0; i < 4; ++i) y.Tick();
        EXPECT_EQ(LastSnapshot(p).lastUseTick, useTick) << age;
        EXPECT_EQ(LastSnapshot(p).useResult, sim::UseResult::Stale) << age;
    }
    EXPECT_EQ(y.server.Stats().usesRefused, 3u);
}

TEST(Mechanisms, GateNeverGoesToRubbleOverBudget) {
    net::ServerSettings settings = YardSettings(true);
    settings.entityEventBurstBytes = 256;          // the yard's spawns alone spend it: the budget stays spent
    settings.entityEventBytesPerTick = 0;
    YardServer y(std::move(settings));
    ASSERT_TRUE(y.started);
    YardPeer& p = y.Join();
    y.Step(5);
    ASSERT_EQ(y.server.Stats().mechanisms, 3u);
    const scene::Entity gate = FindMechanism(y.Sim(), scene::MechanismKind::Portcullis);
    ASSERT_NE(gate, scene::kNoEntity);
    const uint32_t gateId = NetIdOfEntity(y.Sim(), gate);
    // A server cast at the gate's centre (an aimed hit: within kMechanismHitRadius of its model).
    const float vs = y.Sim().World().VoxelSize();
    const glm::dvec3 centre = BodyCentre(y.Sim(), gate);
    const uint32_t modelsBefore = y.server.Stats().objectModels;
    ASSERT_TRUE(y.server.QueueCast(voxel::QuantizeSphere(centre / static_cast<double>(vs), 0.5 / vs), voxel::QuantizePower(100.0f)));
    y.Step(3);
    const net::ServerStats& st = y.server.Stats();
    EXPECT_EQ(st.modelsToRubble, 0u) << "a mechanism is never let go as rubble";
    EXPECT_GT(st.objectModels, modelsBefore);
    EXPECT_GE(st.mechanismModelsOverBudget, 1u);
    bool sent = false;
    for (const net::ObjectModel& m : p.Events<net::ObjectModel>()) {
        sent = sent || m.netId == gateId;
        if (m.netId == gateId) EXPECT_EQ(m.motion, static_cast<uint8_t>(physics::MotionType::Kinematic));
    }
    EXPECT_TRUE(sent) << "the gate's model reaches the client";
    EXPECT_NE(EntityOfNetId(y.Sim(), gateId), scene::kNoEntity);
}

TEST(Mechanisms, LateUseUsesTheStateAtItsTick) {
    YardServer y(YardSettings(true));
    ASSERT_TRUE(y.started);
    YardPeer& p = y.Join();
    y.Step(5);
    const scene::Entity door = FindMechanism(y.Sim(), scene::MechanismKind::Door);
    ASSERT_NE(door, scene::kNoEntity);
    const uint32_t doorId = NetIdOfEntity(y.Sim(), door);
    const glm::dvec3 target = UseBoxCentre(y.Sim(), door);
    sim::PlayerInput look;
    ASSERT_TRUE(StandAt(y, p, 0, door, target, look));
    const glm::vec3 at = y.Feet(p);
    // No more inputs: the server repeats the last one (no edge) and keeps the player where it stands.
    y.controls[0] = nullptr;
    const uint32_t useTick = y.server.TickNumber();
    y.Step(3);
    // Then it is somewhere else, out of reach of the door.
    glm::vec3 out = at - glm::vec3(target);
    out.y = 0.0f;
    const glm::vec3 away = at + glm::normalize(out) * 6.0f;
    ASSERT_TRUE(y.server.TeleportPlayer(p.Slot(), glm::vec3(away.x, at.y, away.z)));
    y.Step(7);
    {
        const glm::vec3 now = y.Feet(p);
        const glm::dvec3 eye = glm::dvec3(now) + glm::dvec3(0.0, static_cast<double>(EyeHeight()), 0.0);
        sim::UseTarget t;
        EXPECT_FALSE(sim::FindUseTarget(y.Sim(), eye, sim::LookDirection(look.yaw, look.pitch), y.Sim().Tick(), t) && t.entity == door)
            << "with the state now the use would miss";
    }
    const uint32_t movesBefore = y.server.Stats().mechanismMoves;
    sim::PlayerInput late = look;
    late.tick = useTick;
    late.buttons = sim::PlayerButton_Use;
    p.Input(late);
    y.Step(4);
    EXPECT_GT(y.server.Stats().mechanismMoves, movesBefore);
    bool moved = false;
    for (const net::MechanismMove& m : p.Events<net::MechanismMove>()) moved = moved || m.netId == doorId;
    EXPECT_TRUE(moved);
    EXPECT_EQ(LastSnapshot(p).lastUseTick, useTick);
    EXPECT_EQ(LastSnapshot(p).useResult, sim::UseResult::None);
}

TEST(Hold, ServerPicksUpAndDrops) {
    YardServer y(YardSettings(false));
    ASSERT_TRUE(y.started);
    YardPeer& p = y.Join();
    sim::PlayerInput look;
    const uint32_t crate = PickUpACrate(y, p, 0, look);
    ASSERT_NE(crate, 0u);
    const std::vector<net::EntityHold> holds = p.Events<net::EntityHold>();
    ASSERT_EQ(holds.size(), 1u);
    EXPECT_EQ(holds[0].netId, crate);
    EXPECT_EQ(holds[0].holder, p.Slot());
    y.Step(2);
    EXPECT_EQ(LastSnapshot(p).useResult, sim::UseResult::None);
    for (const net::EntityWake& w : p.Events<net::EntityWake>()) EXPECT_NE(w.netId, crate) << "the hold is the wake";
    // Held: not resting, no samples for 60 ticks.
    const size_t snapshotsBefore = p.snapshots.size();
    y.Step(60);
    for (const net::EntityInfo& e : y.server.Entities())
        if (e.netId == crate) EXPECT_FALSE(e.resting);
    for (size_t i = snapshotsBefore; i < p.snapshots.size(); ++i)
        for (const net::BodySample& b : p.snapshots[i].bodies) EXPECT_NE(b.netId, crate);
    EXPECT_EQ(y.server.AwakeBodies(), 0u);
    // A model answer carries the hold, never the rest.
    p.Send(net::Channel::Events, net::Encode(net::ModelRequest{{crate}}));
    y.Step(4);
    bool answered = false;
    for (const net::BaselineEntity& b : p.baseline)
        if (b.spawn.netId == crate) {
            answered = true;
            EXPECT_TRUE(b.spawn.object.flags & net::kObjectHeld);
            EXPECT_FALSE(b.spawn.object.flags & net::kObjectResting);
            EXPECT_EQ(b.spawn.object.holder, p.Slot());
        }
    EXPECT_TRUE(answered);
    // The second Use: a drop, then samples, then a rest.
    Press(y, 0, look, sim::PlayerButton_Use);
    const std::vector<net::EntityRelease> releases = p.Events<net::EntityRelease>();
    ASSERT_EQ(releases.size(), 1u);
    EXPECT_EQ(releases[0].reason, static_cast<uint8_t>(sim::HoldEnd::Drop));
    bool sampled = false, rested = false;
    for (int i = 0; i < 600 && !rested; ++i) {
        y.Step();
        for (const net::BodySample& b : LastSnapshot(p).bodies) sampled = sampled || b.netId == crate;
        for (const net::EntityRest& r : p.Events<net::EntityRest>()) rested = rested || r.netId == crate;
    }
    EXPECT_TRUE(sampled);
    EXPECT_TRUE(rested);
    EXPECT_EQ(y.server.Stats().pickups, 1u);
    EXPECT_EQ(y.server.Stats().drops, 1u);
}

TEST(Hold, ThrowRunsNoCast) {
    YardServer y(YardSettings(false));
    ASSERT_TRUE(y.started);
    YardPeer& p = y.Join();
    sim::PlayerInput look;
    const uint32_t crate = PickUpACrate(y, p, 0, look);
    ASSERT_EQ(p.Events<net::EntityHold>().size(), 1u);
    y.Step(20);
    const uint64_t hash = y.server.Hash().hash;
    const net::ServerStats before = y.server.Stats();
    const uint32_t castTick = LastSnapshot(p).lastCastTick;
    const glm::vec3 velocity = y.server.GetPlayer(p.Slot())->velocity;
    Press(y, 0, look, sim::PlayerButton_Cast);
    y.Step(4);
    std::vector<net::EntityRelease> releases = p.Events<net::EntityRelease>();
    ASSERT_EQ(releases.size(), 1u);
    EXPECT_EQ(releases[0].netId, crate);
    EXPECT_EQ(releases[0].reason, static_cast<uint8_t>(sim::HoldEnd::Throw));
    const glm::vec3 expected = net::QuantizeVelocity(velocity + sim::LookDirection(look.yaw, look.pitch) * sim::kThrowSpeed);
    EXPECT_LT(glm::length(releases[0].velocity - expected), 0.05f);
    EXPECT_EQ(y.server.Stats().castsQueued, before.castsQueued);
    EXPECT_EQ(y.server.Stats().castsRun, before.castsRun);
    EXPECT_EQ(y.server.Hash().hash, hash);
    EXPECT_EQ(LastSnapshot(p).lastCastTick, castTick);
    // Use and Cast in one input while holding: one release (a throw), no use, no cast.
    for (int i = 0; i < 600 && y.server.AwakeBodies() != 0; ++i) y.Step();
    const scene::Entity e = EntityOfNetId(y.Sim(), crate);
    ASSERT_NE(e, scene::kNoEntity);
    const glm::dvec3 c = BodyCentre(y.Sim(), e);
    const float ground = static_cast<float>(kYardGround) * kYardVoxel + 0.02f;
    ASSERT_TRUE(y.server.TeleportPlayer(p.Slot(), glm::vec3(static_cast<float>(c.x), ground, static_cast<float>(c.z) + 2.0f)));
    y.controls[0] = [] { return std::optional<sim::PlayerInput>(sim::PlayerInput{}); };
    y.Step(5);
    look = LookAt(y.Feet(p), BodyCentre(y.Sim(), e));
    Press(y, 0, look, sim::PlayerButton_Use);
    ASSERT_EQ(p.Events<net::EntityHold>().size(), 2u);
    y.Step(20);
    const net::ServerStats mid = y.server.Stats();
    Press(y, 0, look, static_cast<uint8_t>(sim::PlayerButton_Use | sim::PlayerButton_Cast));
    y.Step(4);
    releases = p.Events<net::EntityRelease>();
    ASSERT_EQ(releases.size(), 2u);
    EXPECT_EQ(releases[1].reason, static_cast<uint8_t>(sim::HoldEnd::Throw));
    EXPECT_EQ(y.server.Stats().castsQueued, mid.castsQueued);
    EXPECT_EQ(y.server.Stats().pickups, mid.pickups);
    EXPECT_EQ(y.server.Stats().drops, mid.drops);
    EXPECT_EQ(y.server.Stats().throws, 2u);
}

TEST(Hold, HolderDeathDrops) {
    net::ServerSettings settings = YardSettings(false);
    sim::FightRules f;
    f.mode = sim::GameMode::Deathmatch;
    f.teamSpawns = {glm::vec2(-6.0f, 4.0f), glm::vec2(6.0f, 4.0f)};
    f.minPlayers = 2;
    f.countdownTicks = 1;
    f.endTicks = 60;
    f.endSession = false;
    f.protectTicks = 0;                            // the void kills at once
    settings.session.fight = f;
    YardServer y(std::move(settings));
    ASSERT_TRUE(y.started);
    YardPeer& holder = y.Join();
    YardPeer& other = y.Join();
    y.controls[0] = [] { return std::optional<sim::PlayerInput>(sim::PlayerInput{}); };
    y.controls[1] = y.controls[0];
    y.Step(60);                                    // the round goes Live
    sim::PlayerInput look;
    const uint32_t crate = PickUpACrate(y, holder, 0, look);
    ASSERT_EQ(holder.Events<net::EntityHold>().size(), 1u);
    // Into the void: the holder dies at its next step.
    const glm::vec3 feet = y.Feet(holder);
    ASSERT_TRUE(y.server.TeleportPlayer(holder.Slot(), glm::vec3(feet.x, 1.0f, feet.z)));
    y.Step(3);
    EXPECT_GE(y.server.Stats().deaths, 1u);
    const std::vector<net::EntityRelease> releases = other.Events<net::EntityRelease>();
    ASSERT_EQ(releases.size(), 1u);
    EXPECT_EQ(releases[0].netId, crate);
    EXPECT_EQ(releases[0].reason, static_cast<uint8_t>(sim::HoldEnd::HolderDead));
}

TEST(Hold, HolderLeavingDrops) {
    YardServer y(YardSettings(false));
    ASSERT_TRUE(y.started);
    YardPeer& holder = y.Join();
    YardPeer& other = y.Join();
    sim::PlayerInput look;
    const uint32_t crate = PickUpACrate(y, holder, 0, look);
    ASSERT_EQ(other.Events<net::EntityHold>().size(), 1u);
    holder.loop.Disconnect(holder.server);
    y.controls[0] = nullptr;
    y.Step(3);
    const std::vector<net::EntityRelease> releases = other.Events<net::EntityRelease>();
    ASSERT_EQ(releases.size(), 1u);
    EXPECT_EQ(releases[0].netId, crate);
    EXPECT_EQ(releases[0].reason, static_cast<uint8_t>(sim::HoldEnd::HolderGone));
    EXPECT_EQ(y.server.Stats().releases[4], 1u);
}

TEST(Hold, NotCountedAsAwake) {
    YardServer y(YardSettings(false));
    ASSERT_TRUE(y.started);
    YardPeer& p = y.Join();
    sim::PlayerInput look;
    const uint32_t crate = PickUpACrate(y, p, 0, look);
    ASSERT_NE(crate, 0u);
    // Walking about with it: never awake for the server's snapshots nor for the piece caps.
    y.controls[0] = [look] {
        sim::PlayerInput in = look;
        in.moveX = 60;
        return std::optional<sim::PlayerInput>(in);
    };
    for (int i = 0; i < 60; ++i) {
        y.Step();
        EXPECT_EQ(y.server.AwakeBodies(), 0u);
        EXPECT_EQ(sim::CountPieces(y.Sim()).awakeBodies, 0u);
    }
    EXPECT_NE(y.Sim().Scene().TryGet<scene::Held>(EntityOfNetId(y.Sim(), crate)), nullptr);
}

TEST(Hold, NoOneLosesTheirFloor) {
    YardServer y(YardSettings(false));
    ASSERT_TRUE(y.started);
    YardPeer& a = y.Join();
    YardPeer& b = y.Join();
    y.controls[0] = [] { return std::optional<sim::PlayerInput>(sim::PlayerInput{}); };
    y.controls[1] = y.controls[0];
    const float ground = static_cast<float>(kYardGround) * kYardVoxel + 0.02f;
    ASSERT_TRUE(y.server.TeleportPlayer(a.Slot(), glm::vec3(0.0f, ground, 0.0f)));
    ASSERT_TRUE(y.server.TeleportPlayer(b.Slot(), glm::vec3(4.0f, ground, 0.0f)));
    y.Step(5);
    const uint32_t crate = RestingCrate(y, 0.0, -2.0);
    const scene::Entity e = EntityOfNetId(y.Sim(), crate);
    ASSERT_NE(e, scene::kNoEntity);
    const glm::dvec3 centre = BodyCentre(y.Sim(), e);
    // B stands on the crate; A's Use at it: Busy.
    ASSERT_TRUE(y.server.TeleportPlayer(b.Slot(), glm::vec3(0.0f, ground + 0.8f, -2.0f)));
    y.Step(20);
    const sim::PlayerInput look = LookAt(y.Feet(a), centre);
    Press(y, 0, look, sim::PlayerButton_Use);
    y.Step(3);
    EXPECT_EQ(LastSnapshot(a).useResult, sim::UseResult::Busy);
    EXPECT_TRUE(a.Events<net::EntityHold>().empty());
    // B steps off; A's Use 20 ticks later is still Busy, 35 ticks later it holds it.
    ASSERT_TRUE(y.server.TeleportPlayer(b.Slot(), glm::vec3(4.0f, ground, 0.0f)));
    y.Step(18);
    Press(y, 0, look, sim::PlayerButton_Use);
    y.Step(3);
    EXPECT_EQ(LastSnapshot(a).useResult, sim::UseResult::Busy);
    y.Step(12);
    Press(y, 0, look, sim::PlayerButton_Use);
    y.Step(3);
    EXPECT_EQ(LastSnapshot(a).useResult, sim::UseResult::None);
    ASSERT_EQ(a.Events<net::EntityHold>().size(), 1u);
    EXPECT_EQ(a.Events<net::EntityHold>()[0].holder, a.Slot());
}

TEST(Hold, AutoDropEchoIsConsumed) {
    YardServer y(YardSettings(false));
    ASSERT_TRUE(y.started);
    YardPeer& p = y.Join();
    sim::PlayerInput look;
    const uint32_t crate = PickUpACrate(y, p, 0, look);
    ASSERT_EQ(p.Events<net::EntityHold>().size(), 1u);
    y.controls[0] = [look] { return std::optional<sim::PlayerInput>(look); };
    // A stone wall 0.7 m in front of the holder pins what it holds: an auto-drop (Pinned) after 30 ticks.
    const voxel::WorldMaterials ids = YardIds();
    const glm::vec3 feet = y.Feet(p);
    const auto V = [](double m) { return static_cast<int>(std::floor(m / static_cast<double>(kYardVoxel))); };
    ASSERT_NE(y.server.ApplyOp(sim::WorldOp::Box(glm::ivec3(V(feet.x - 2.0), kYardGround, V(feet.z - 1.3)),
                                                 glm::ivec3(V(feet.x + 2.0), kYardGround + 24, V(feet.z - 0.7)), ids.stone)),
              0u);
    for (int i = 0; i < 60 && p.Events<net::EntityRelease>().empty(); ++i) y.Step();
    const std::vector<net::EntityRelease> releases = p.Events<net::EntityRelease>();
    ASSERT_EQ(releases.size(), 1u);
    EXPECT_EQ(releases[0].netId, crate);
    EXPECT_EQ(releases[0].reason, static_cast<uint8_t>(sim::HoldEnd::Pinned));
    // A Cast edge and a Use edge inside the echo window (input ticks right after the release): consumed.
    const net::ServerStats before = y.server.Stats();
    const uint64_t hash = y.server.Hash().hash;
    sim::PlayerInput down = look;
    down.pitch = -12000;
    Press(y, 0, down, sim::PlayerButton_Cast);
    EXPECT_EQ(y.server.Stats().castsQueued, before.castsQueued);
    EXPECT_EQ(y.server.Stats().echoEdges, before.echoEdges + 1);
    const scene::Entity e = EntityOfNetId(y.Sim(), crate);
    ASSERT_NE(e, scene::kNoEntity);
    Press(y, 0, LookAt(y.Feet(p), BodyCentre(y.Sim(), e)), sim::PlayerButton_Use);
    EXPECT_EQ(y.server.Stats().echoEdges, before.echoEdges + 2);
    EXPECT_EQ(y.server.Stats().pickups, before.pickups);
    EXPECT_EQ(y.server.Stats().uses, before.uses);
    y.Step(2);
    EXPECT_EQ(y.server.Hash().hash, hash);
    // After the window the same edge acts: one cast.
    y.Step(30);
    Press(y, 0, down, sim::PlayerButton_Cast);
    y.Step(3);
    EXPECT_EQ(y.server.Stats().castsQueued, before.castsQueued + 1);
    EXPECT_EQ(y.server.Stats().echoEdges, before.echoEdges + 2);
}
