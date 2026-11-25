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

// --- the client: mechanism copies, prediction at mechanisms, holding ([mech-1] card 10) ------------------------

namespace {
    /// A server and predicting bots (net::Client) on impaired loopbacks, one manual clock (EntityTests' Arena with the
    /// yard's settings): a pass is one tick of server time; each client runs the ticks its own clock gives it.
    struct YardArena {
        struct Bot {
            net::LoopbackTransport loop;
            std::unique_ptr<net::ImpairedTransport> link;
            std::unique_ptr<net::Client> client;
            std::unique_ptr<sim::Simulation> simulation;
            sim::PlayerInput look;             ///< what the bot sends every tick (buttons: levels)
            uint8_t pressOnce = 0;             ///< buttons held for the next tick only
            net::PeerId peer = 0;
        };

        double clock = 0.0;
        double dt = 1.0 / 60.0;
        net::Impairment impairment;
        net::LoopbackTransport serverLoop;
        net::ImpairedTransport serverLink;
        net::Server server;
        bool started = false;
        std::vector<std::unique_ptr<Bot>> bots;

        explicit YardArena(net::ServerSettings settings, net::Impairment im = {})
            : impairment(std::move(im)), serverLink(serverLoop, this->impairment, 31, [this] { return clock; }),
              server(std::move(settings), serverLink) {
            started = server.Start();
            dt = started ? static_cast<double>(server.Simulation().FixedTimeStep()) : 1.0 / 60.0;
        }

        Bot& AddBot(std::function<bool(uint32_t, const net::Event&)> drop = {}) {
            auto bot = std::make_unique<Bot>();
            bot->link = std::make_unique<net::ImpairedTransport>(bot->loop, impairment, 700 + bots.size(), [this] { return clock; });
            net::ClientSettings cs;
            cs.token = YardToken();
            cs.testDropEntityEvent = std::move(drop);
            bot->client = std::make_unique<net::Client>(cs, *bot->link);
            bot->peer = net::LoopbackTransport::Connect(serverLoop, bot->loop);
            bots.push_back(std::move(bot));
            return *bots.back();
        }

        void ClientPass(Bot& bot) {
            if (bot.client->State() == net::ClientState::Welcomed && !bot.simulation) {
                sim::SimulationConfig base;
                base.jobThreads = 2;
                base.chunkCapacity = 4096;
                base.maxParticles = 0;
                base.physics.threads = 1;
                base.physics.maxBodies = 2048;
                base.physics.maxBodyPairs = 4096;
                base.physics.maxContactConstraints = 4096;
                bot.simulation = std::make_unique<sim::Simulation>(bot.client->MakeSimulationConfig(base));
                ASSERT_TRUE(bot.simulation->Initialize(bot.client->MakeGenerator()));
                ASSERT_TRUE(bot.client->Attach(*bot.simulation));
            }
            const uint32_t n = bot.client->TicksDue(dt);
            for (uint32_t i = 0; i < n; ++i) {
                sim::PlayerInput in{};
                if (bot.client->Predicting()) {
                    in = bot.look;
                    in.buttons = static_cast<uint8_t>(in.buttons | bot.pressOnce);
                    bot.pressOnce = 0;
                }
                bot.client->RunTick(in);
                if (bot.simulation && bot.simulation->Terrain().Stats().pending != 0) bot.simulation->Jobs().WaitIdle();
            }
        }

        void Pass() {
            clock += dt;
            server.Advance(dt);
            if (server.Simulation().Terrain().Stats().pending != 0) server.Simulation().Jobs().WaitIdle();
            for (const std::unique_ptr<Bot>& bot : bots) ClientPass(*bot);
        }

        void Passes(int count) {
            for (int i = 0; i < count; ++i) Pass();
        }

        bool Join(int maxPasses = 1200) {
            for (int i = 0; i < maxPasses; ++i) {
                bool all = true;
                for (const std::unique_ptr<Bot>& bot : bots) all = all && bot->client->Predicting() && bot->client->Synced();
                if (all) return true;
                Pass();
            }
            return false;
        }

        /// Teleports the bot's player to `feet` and waits until its prediction took the server's state and settled.
        void Place(Bot& bot, const glm::vec3& feet) {
            bot.look.moveX = bot.look.moveZ = 0;
            ASSERT_TRUE(server.TeleportPlayer(bot.client->Slot(), feet));
            Passes(90);
        }

        glm::vec3 Feet(const Bot& bot) { return server.GetPlayer(bot.client->Slot())->position; }
    };

    sim::RegionDesc ArenaYardRegion(bool yard) {
        sim::RegionDesc r = YardRegion(yard ? sim::RegionTemplateId::Playground : sim::RegionTemplateId::Empty);
        if (!yard) {
            r.playMinChunk = {-4, -4};
            r.playMaxChunk = {4, 4};
        }
        return r;
    }

    net::ServerSettings ArenaYardSettings(bool yard) {
        net::ServerSettings s = YardSettings(yard);
        s.session.region = ArenaYardRegion(yard);
        return s;
    }

    net::Impairment DelayEachWay(double seconds) {
        net::Impairment im;
        im.delaySeconds = seconds;
        return im;
    }

    /// A spot on the ground where the eye's look at `target` picks `entity` on the server's world now (no player
    /// there yet: a pure ray test), or nullopt.
    std::optional<glm::vec3> UseSpot(sim::Simulation& simulation, scene::Entity entity, const glm::dvec3& target) {
        voxel::World& world = simulation.World();
        for (const double r : {1.4, 1.8, 1.1})
            for (int k = 0; k < 16; ++k) {
                const double a = 6.283185307179586 * k / 16.0;
                const glm::dvec3 at = target + glm::dvec3(std::cos(a) * r, 0.0, std::sin(a) * r);
                const voxel::WorldRayHit ground = world.Raycast(glm::dvec3(at.x, target.y + 1.0, at.z), glm::vec3(0.0f, -1.0f, 0.0f), 6.0f);
                if (!ground.hit) continue;
                const glm::vec3 feet(static_cast<float>(at.x), static_cast<float>(ground.voxel.y + 1) * world.VoxelSize() + 0.02f,
                                     static_cast<float>(at.z));
                // The capsule's column is air.
                bool clear = true;
                for (float y = 0.2f; y < 1.8f && clear; y += 0.2f)
                    clear = world.GetVoxel(world.WorldToVoxel(glm::dvec3(feet) + glm::dvec3(0.0, y, 0.0))) == 0;
                if (!clear) continue;
                const sim::PlayerInput look = LookAt(feet, target);
                const glm::dvec3 eye = glm::dvec3(feet) + glm::dvec3(0.0, static_cast<double>(EyeHeight()), 0.0);
                sim::UseTarget t;
                if (sim::FindUseTarget(simulation, eye, sim::LookDirection(look.yaw, look.pitch), simulation.Tick(), t) && t.entity == entity)
                    return feet;
            }
        return std::nullopt;
    }

    /// The bot stands at the door's use spot and looks at its use box. False when there is no spot.
    bool AtTheDoor(YardArena& a, YardArena::Bot& bot, scene::Entity door) {
        const glm::dvec3 target = UseBoxCentre(a.server.Simulation(), door);
        const std::optional<glm::vec3> spot = UseSpot(a.server.Simulation(), door, target);
        if (!spot) return false;
        a.Place(bot, *spot);
        bot.look = LookAt(a.Feet(bot), target);
        a.Passes(10);
        return true;
    }

    /// A crate at rest on the flat ground with its centre at (x, z).
    uint32_t ArenaCrate(YardArena& a, double x, double z) {
        const double half = 0.375;
        const uint32_t netId = a.server.SpawnCrate(glm::dvec3(x - half, static_cast<double>(kYardGround) * kYardVoxel + 0.05, z - half));
        for (int i = 0; i < 600 && a.server.AwakeBodies() != 0; ++i) a.Pass();
        a.Passes(30);
        return netId;
    }

    float Ground() { return static_cast<float>(kYardGround) * kYardVoxel + 0.02f; }

    /// The bot's copy of `netId` (kNoEntity without one).
    scene::Entity CopyOn(YardArena::Bot& bot, uint32_t netId) { return bot.client->CopyOf(netId); }
}

TEST(Mechanisms, LateJoinerDerivesTheSamePosition) {
    YardArena a(ArenaYardSettings(true));
    ASSERT_TRUE(a.started);
    YardArena::Bot& user = a.AddBot();
    ASSERT_TRUE(a.Join());
    const scene::Entity door = FindMechanism(a.server.Simulation(), scene::MechanismKind::Door);
    ASSERT_NE(door, scene::kNoEntity);
    const uint32_t doorId = NetIdOfEntity(a.server.Simulation(), door);
    ASSERT_TRUE(AtTheDoor(a, user, door));
    // An open, then a close that reverses it mid-swing: two segments.
    user.pressOnce = sim::PlayerButton_Use;
    a.Passes(16);
    user.pressOnce = sim::PlayerButton_Use;
    a.Passes(2);
    YardArena::Bot& late = a.AddBot();
    ASSERT_TRUE(a.Join());
    const scene::Mechanism* s = a.server.Simulation().Scene().TryGet<scene::Mechanism>(door);
    ASSERT_NE(s, nullptr);
    EXPECT_GE(s->track.count, 3u) << "the rest, the open and the close";
    for (YardArena::Bot* bot : {&user, &late}) {
        const scene::Entity copy = CopyOn(*bot, doorId);
        ASSERT_NE(copy, scene::kNoEntity);
        const scene::Mechanism* c = bot->simulation->Scene().TryGet<scene::Mechanism>(copy);
        ASSERT_NE(c, nullptr);
        EXPECT_EQ(c->track.Newest(), s->track.Newest());
        const uint32_t now = a.server.TickNumber();
        for (uint32_t t = now; t < now + 60; ++t) EXPECT_EQ(sim::PositionAt(*c, t), sim::PositionAt(*s, t)) << "tick " << t;
    }
    EXPECT_GE(user.client->Stats().mechanismMoves, 2u);
}

TEST(Mechanisms, NoCorrectionWalkingThroughAnOpeningGate) {
    YardArena a(ArenaYardSettings(true), DelayEachWay(0.05));   // 100 ms round trip
    ASSERT_TRUE(a.started);
    YardArena::Bot& user = a.AddBot();
    ASSERT_TRUE(a.Join());
    const scene::Entity door = FindMechanism(a.server.Simulation(), scene::MechanismKind::Door);
    ASSERT_NE(door, scene::kNoEntity);
    ASSERT_TRUE(AtTheDoor(a, user, door));
    a.Passes(60);
    const uint64_t before = user.client->Prediction().corrections;
    // Use the door and walk into it while it opens.
    user.pressOnce = sim::PlayerButton_Use;
    a.Passes(2);
    user.look.pitch = 0;
    user.look.moveZ = 127;
    a.Passes(150);
    EXPECT_EQ(user.client->Prediction().corrections, before);
    EXPECT_EQ(user.client->Stats().mechanismLateEvents, 0u);
    EXPECT_EQ(user.client->LastUse().result, sim::UseResult::None);
    EXPECT_GE(user.client->Stats().mechanismMoves, 1u);
}

TEST(Mechanisms, CorrectionOnlyWhenTheEventIsLate) {
    YardArena a(ArenaYardSettings(true), DelayEachWay(0.2));    // 400 ms: the lead is clamped at 20 ticks
    ASSERT_TRUE(a.started);
    YardArena::Bot& user = a.AddBot();
    YardArena::Bot& away = a.AddBot();
    ASSERT_TRUE(a.Join(2400));
    const scene::Entity door = FindMechanism(a.server.Simulation(), scene::MechanismKind::Door);
    ASSERT_NE(door, scene::kNoEntity);
    a.Place(away, glm::vec3(10.0f, Ground(), 10.0f));
    ASSERT_TRUE(AtTheDoor(a, user, door));
    a.Passes(90);
    const uint64_t awayBefore = away.client->Prediction().corrections;
    user.pressOnce = sim::PlayerButton_Use;
    a.Passes(2);
    user.look.pitch = 0;
    user.look.moveZ = 127;
    a.Passes(150);
    EXPECT_GE(user.client->Stats().mechanismLateEvents, 1u) << "a 400 ms round trip is longer than the lead";
    EXPECT_EQ(away.client->Prediction().corrections, awayBefore);
}

TEST(Mechanisms, EntityCheckCoversTheSegment) {
    YardArena a(ArenaYardSettings(true));
    ASSERT_TRUE(a.started);
    YardArena::Bot& user = a.AddBot();
    bool dropped = false;
    YardArena::Bot& faulty = a.AddBot([&dropped](uint32_t, const net::Event& e) {
        if (dropped || !std::holds_alternative<net::MechanismMove>(e)) return false;
        dropped = true;
        return true;
    });
    ASSERT_TRUE(a.Join());
    const scene::Entity door = FindMechanism(a.server.Simulation(), scene::MechanismKind::Door);
    ASSERT_NE(door, scene::kNoEntity);
    const uint32_t doorId = NetIdOfEntity(a.server.Simulation(), door);
    ASSERT_TRUE(AtTheDoor(a, user, door));
    user.pressOnce = sim::PlayerButton_Use;
    for (int i = 0; i < 1200 && faulty.client->Stats().mechanismsRepaired == 0; ++i) a.Pass();
    EXPECT_TRUE(dropped);
    EXPECT_GE(faulty.client->Stats().entityMismatches, 1u);
    EXPECT_EQ(faulty.client->Stats().mechanismsRepaired, 1u);
    a.Passes(120);
    const scene::Entity copy = CopyOn(faulty, doorId);
    ASSERT_NE(copy, scene::kNoEntity);
    const scene::Mechanism* c = faulty.simulation->Scene().TryGet<scene::Mechanism>(copy);
    const scene::Mechanism* s = a.server.Simulation().Scene().TryGet<scene::Mechanism>(door);
    ASSERT_NE(c, nullptr);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(c->track.Newest(), s->track.Newest()) << "the repaired copy has the server's segment";
}

TEST(Hold, PredictedPickupIsConfirmed) {
    YardArena a(ArenaYardSettings(false), DelayEachWay(0.05));
    ASSERT_TRUE(a.started);
    YardArena::Bot& bot = a.AddBot();
    ASSERT_TRUE(a.Join());
    a.Place(bot, glm::vec3(0.0f, Ground(), 0.0f));
    const uint32_t crate = ArenaCrate(a, 0.0, -2.0);
    ASSERT_NE(crate, 0u);
    const scene::Entity copy = CopyOn(bot, crate);
    ASSERT_NE(copy, scene::kNoEntity);
    bot.look = LookAt(a.Feet(bot), BodyCentre(*bot.simulation, copy));
    a.Passes(10);
    const uint64_t corrections = bot.client->Prediction().corrections;
    // The Use press: held on the client from its own tick, before the server answered.
    bot.pressOnce = sim::PlayerButton_Use;
    a.Pass();
    EXPECT_EQ(bot.client->HeldNetId(), crate);
    EXPECT_TRUE(bot.client->HoldPending());
    EXPECT_NE(bot.simulation->Scene().TryGet<scene::Held>(copy), nullptr);
    for (int i = 0; i < 60 && bot.client->HoldPending(); ++i) a.Pass();
    EXPECT_FALSE(bot.client->HoldPending());
    EXPECT_EQ(bot.client->HeldNetId(), crate);
    EXPECT_EQ(bot.client->Stats().holdsConfirmed, 1u);
    EXPECT_EQ(bot.client->Stats().holdRefused, 0u);
    // Walking with it: no correction, the copy stays kinematic.
    bot.look.moveX = 90;
    for (int i = 0; i < 120; ++i) {
        a.Pass();
        const scene::RigidBody* rigid = bot.simulation->Scene().TryGet<scene::RigidBody>(copy);
        ASSERT_NE(rigid, nullptr);
        EXPECT_EQ(rigid->motion, physics::MotionType::Kinematic);
    }
    EXPECT_EQ(bot.client->Prediction().corrections, corrections);
    // The drop: the copy hangs until the release, then follows the server's samples (none of the release tick or older).
    bot.look.moveX = 0;
    a.Passes(10);
    bot.pressOnce = sim::PlayerButton_Use;
    for (int i = 0; i < 60 && bot.client->HeldNetId() != 0; ++i) a.Pass();
    EXPECT_EQ(bot.client->HeldNetId(), 0u);
    EXPECT_EQ(bot.simulation->Scene().TryGet<scene::Held>(copy), nullptr);
    EXPECT_EQ(bot.client->Stats().holdReleases, 1u);
    a.Passes(6);
    uint32_t oldest = 0;
    net::BodyPose pose;
    // The oldest kept sample is the release pose at the release tick (the seed) or a newer server sample, never older.
    if (bot.client->Bodies().Oldest(crate, oldest, pose)) EXPECT_GE(oldest, bot.client->LastUse().tick);
    const scene::RigidBody* rigid = bot.simulation->Scene().TryGet<scene::RigidBody>(copy);
    ASSERT_NE(rigid, nullptr);
    EXPECT_EQ(rigid->motion, physics::MotionType::Kinematic);
}

TEST(Hold, RefusedPickupIsUndone) {
    YardArena a(ArenaYardSettings(false), DelayEachWay(0.05));
    ASSERT_TRUE(a.started);
    YardArena::Bot& stander = a.AddBot();
    YardArena::Bot& grabber = a.AddBot();
    ASSERT_TRUE(a.Join());
    a.Place(grabber, glm::vec3(0.0f, Ground(), 0.0f));
    a.Place(stander, glm::vec3(4.0f, Ground(), 0.0f));
    const uint32_t crate = ArenaCrate(a, 0.0, -2.0);
    ASSERT_NE(crate, 0u);
    // The stander on the crate: the server refuses the grab (Busy); the grabber's client cannot know.
    a.Place(stander, glm::vec3(0.0f, Ground() + 0.8f, -2.0f));
    const scene::Entity copy = CopyOn(grabber, crate);
    ASSERT_NE(copy, scene::kNoEntity);
    const glm::vec3 restAt = grabber.simulation->Scene().TryGet<scene::Transform>(copy)->current.position;
    grabber.look = LookAt(a.Feet(grabber), BodyCentre(*grabber.simulation, copy));
    a.Passes(10);
    const uint32_t castsBefore = a.server.Stats().castsQueued;
    grabber.pressOnce = sim::PlayerButton_Use;
    a.Pass();
    ASSERT_TRUE(grabber.client->HoldPending()) << "the client predicted the pick-up";
    // A Cast press made and released while it is pending: latched, never sent (the grab failed).
    grabber.pressOnce = sim::PlayerButton_Cast;
    a.Pass();
    for (int i = 0; i < 60 && grabber.client->HoldPending(); ++i) a.Pass();
    a.Passes(20);
    EXPECT_FALSE(grabber.client->HoldPending());
    EXPECT_EQ(grabber.client->HeldNetId(), 0u);
    EXPECT_EQ(grabber.client->Stats().holdRefused, 1u);
    EXPECT_EQ(grabber.client->LastUse().result, sim::UseResult::Busy);
    EXPECT_EQ(grabber.simulation->Scene().TryGet<scene::Held>(copy), nullptr);
    EXPECT_LT(glm::distance(grabber.simulation->Scene().TryGet<scene::Transform>(copy)->current.position, restAt), 1.0e-4f);
    EXPECT_EQ(a.server.Stats().castsQueued, castsBefore) << "the latched press never reached the server";
}

TEST(Hold, RemoteHolderPosesTheObject) {
    YardArena a(ArenaYardSettings(false), DelayEachWay(0.03));
    ASSERT_TRUE(a.started);
    YardArena::Bot& holder = a.AddBot();
    YardArena::Bot& watcher = a.AddBot();
    ASSERT_TRUE(a.Join());
    a.Place(holder, glm::vec3(0.0f, Ground(), 0.0f));
    a.Place(watcher, glm::vec3(6.0f, Ground(), 3.0f));
    const uint32_t crate = ArenaCrate(a, 0.0, -2.0);
    const scene::Entity mine = CopyOn(holder, crate);
    ASSERT_NE(mine, scene::kNoEntity);
    holder.look = LookAt(a.Feet(holder), BodyCentre(*holder.simulation, mine));
    a.Passes(10);
    holder.pressOnce = sim::PlayerButton_Use;
    a.Pass();
    for (int i = 0; i < 60 && holder.client->HoldPending(); ++i) a.Pass();
    ASSERT_EQ(holder.client->HeldNetId(), crate);
    a.Passes(20);
    const scene::Entity seen = CopyOn(watcher, crate);
    ASSERT_NE(seen, scene::kNoEntity);
    ASSERT_NE(watcher.simulation->Scene().TryGet<scene::Held>(seen), nullptr);
    // Walking at full speed: the watcher's copy follows the holder as the watcher draws it.
    holder.look.moveX = 127;
    const sim::HoldEnv env{watcher.client->Tuning(), sim::PlayAreaOf(a.server.Region(), watcher.client->Tuning()), {}};
    // (Compared after each pass with the remote player as the watcher draws it then; at an exact sample boundary the
    // view read here can be one tick of walking past the one the copy was posed from: one such pass is allowed.)
    float worst = 0.0f;
    int over = 0;
    for (int i = 0; i < 60; ++i) {
        a.Pass();
        for (const net::RemotePlayerView& v : watcher.client->RemotePlayers()) {
            if (v.slot != holder.client->Slot()) continue;
            const sim::HeldPose p = sim::HeldPoseAt(*watcher.simulation, seen, v.position, v.yaw, v.holdPitch, env,
                                                    watcher.client->PredictTick() - 1);
            const float e = glm::distance(glm::vec3(p.pose.position), watcher.simulation->Scene().TryGet<scene::Transform>(seen)->current.position);
            over += e >= 0.05f ? 1 : 0;
            worst = std::max(worst, e);
        }
    }
    EXPECT_LE(over, 1);
    EXPECT_LT(worst, 0.07f);
    // The drop while walking: the copy's motion changes by under 0.1 m in any tick when the release is applied (it
    // moves on from the release pose into the server's samples).
    holder.pressOnce = sim::PlayerButton_Use;
    glm::vec3 last = watcher.simulation->Scene().TryGet<scene::Transform>(seen)->current.position;
    glm::vec3 lastStep(0.0f);
    bool anyStep = false;
    float jump = 0.0f;
    for (int i = 0; i < 60; ++i) {
        a.Pass();
        const glm::vec3 now = watcher.simulation->Scene().TryGet<scene::Transform>(seen)->current.position;
        const glm::vec3 step = now - last;
        if (anyStep) jump = std::max(jump, glm::distance(step, lastStep));
        anyStep = true;
        lastStep = step;
        last = now;
    }
    std::printf("[ remote hold ] worst pose error %.4f m (%d passes at 5 cm or more), largest change of the motion in a tick %.3f m\n",
                worst, over, jump);
    EXPECT_EQ(watcher.simulation->Scene().TryGet<scene::Held>(seen), nullptr);
    EXPECT_LT(jump, 0.1f);
}

TEST(Hold, LateJoinerSeesTheHold) {
    YardArena a(ArenaYardSettings(false));
    ASSERT_TRUE(a.started);
    YardArena::Bot& holder = a.AddBot();
    ASSERT_TRUE(a.Join());
    a.Place(holder, glm::vec3(0.0f, Ground(), 0.0f));
    const uint32_t crate = ArenaCrate(a, 0.0, -2.0);
    const scene::Entity mine = CopyOn(holder, crate);
    ASSERT_NE(mine, scene::kNoEntity);
    holder.look = LookAt(a.Feet(holder), BodyCentre(*holder.simulation, mine));
    a.Passes(10);
    holder.pressOnce = sim::PlayerButton_Use;
    a.Pass();
    for (int i = 0; i < 60 && holder.client->HoldPending(); ++i) a.Pass();
    ASSERT_EQ(holder.client->HeldNetId(), crate);
    YardArena::Bot& late = a.AddBot();
    ASSERT_TRUE(a.Join());
    const scene::Entity seen = CopyOn(late, crate);
    ASSERT_NE(seen, scene::kNoEntity);
    const scene::Held* h = late.simulation->Scene().TryGet<scene::Held>(seen);
    const scene::Held* s = a.server.Simulation().Scene().TryGet<scene::Held>(EntityOfNetId(a.server.Simulation(), crate));
    ASSERT_NE(h, nullptr);
    ASSERT_NE(s, nullptr);
    EXPECT_EQ(h->holder, s->holder);
    EXPECT_EQ(h->relYaw, s->relYaw);
    for (const net::EntityInfo& e : late.client->Entities())
        if (e.netId == crate) EXPECT_FALSE(e.resting);
    holder.pressOnce = sim::PlayerButton_Use;
    for (int i = 0; i < 600 && late.simulation->Scene().TryGet<scene::Held>(seen); ++i) a.Pass();
    EXPECT_EQ(late.simulation->Scene().TryGet<scene::Held>(seen), nullptr);
    bool rested = false;
    for (int i = 0; i < 600 && !rested; ++i) {
        a.Pass();
        for (const net::EntityInfo& e : late.client->Entities()) rested = rested || (e.netId == crate && e.resting);
    }
    EXPECT_TRUE(rested) << "after the drop the copy follows the server's samples to its rest";
}

TEST(Hold, EntityCheckCoversTheHold) {
    YardArena a(ArenaYardSettings(false));
    ASSERT_TRUE(a.started);
    YardArena::Bot& holder = a.AddBot();
    bool dropped = false;
    YardArena::Bot& faulty = a.AddBot([&dropped](uint32_t, const net::Event& e) {
        if (dropped || !std::holds_alternative<net::EntityHold>(e)) return false;
        dropped = true;
        return true;
    });
    ASSERT_TRUE(a.Join());
    a.Place(holder, glm::vec3(0.0f, Ground(), 0.0f));
    a.Place(faulty, glm::vec3(6.0f, Ground(), 3.0f));
    const uint32_t crate = ArenaCrate(a, 0.0, -2.0);
    const scene::Entity mine = CopyOn(holder, crate);
    ASSERT_NE(mine, scene::kNoEntity);
    holder.look = LookAt(a.Feet(holder), BodyCentre(*holder.simulation, mine));
    a.Passes(10);
    holder.pressOnce = sim::PlayerButton_Use;
    const auto heldThere = [&] {
        const scene::Entity e = CopyOn(faulty, crate);
        return e != scene::kNoEntity && faulty.simulation->Scene().TryGet<scene::Held>(e) != nullptr;
    };
    for (int i = 0; i < 1500 && !heldThere(); ++i) a.Pass();
    EXPECT_TRUE(dropped);
    const scene::Entity seen = CopyOn(faulty, crate);
    ASSERT_NE(seen, scene::kNoEntity);
    EXPECT_NE(faulty.simulation->Scene().TryGet<scene::Held>(seen), nullptr) << "the model answer re-made it held";
    EXPECT_GE(faulty.client->Stats().entityMismatches, 1u);
}

TEST(Mechanisms, DentedDoorKeepsItsLayerOnEverySide) {
    YardArena a(ArenaYardSettings(true));
    ASSERT_TRUE(a.started);
    YardArena::Bot& user = a.AddBot();
    ASSERT_TRUE(a.Join());
    sim::Simulation& server = a.server.Simulation();
    const scene::Entity door = FindMechanism(server, scene::MechanismKind::Door);
    ASSERT_NE(door, scene::kNoEntity);
    const uint32_t doorId = NetIdOfEntity(server, door);
    ASSERT_TRUE(AtTheDoor(a, user, door));
    const float vs = server.World().VoxelSize();
    const auto dent = [&](double radius, double up) {
        const glm::dvec3 centre = BodyCentre(server, door) + glm::dvec3(0.0, up, 0.0);
        ASSERT_TRUE(a.server.QueueCast(voxel::QuantizeSphere(centre / static_cast<double>(vs), radius / vs), voxel::QuantizePower(6.0f)));
    };
    const auto compare = [&](const char* when) {
        const scene::Mechanism* s = server.Scene().TryGet<scene::Mechanism>(door);
        const scene::Entity copy = CopyOn(user, doorId);
        ASSERT_NE(s, nullptr) << when << ": the door was freed (the dent was too strong)";
        ASSERT_NE(copy, scene::kNoEntity) << when;
        const scene::Mechanism* c = user.simulation->Scene().TryGet<scene::Mechanism>(copy);
        ASSERT_NE(c, nullptr) << when;
        EXPECT_EQ(c->modelOffset, s->modelOffset) << when;
        EXPECT_EQ(c->track.Newest(), s->track.Newest()) << when;
        const uint32_t now = a.server.TickNumber();
        for (uint32_t t = now; t < now + 120; ++t) {
            EXPECT_EQ(sim::MechanismLayer(*c, t), sim::MechanismLayer(*s, t)) << when << " tick " << t;
            EXPECT_EQ(sim::PositionAt(*c, t), sim::PositionAt(*s, t)) << when << " tick " << t;
        }
    };
    const uint32_t modelsBefore = user.client->Stats().models;
    // Mid-swing: the open is under way when the dent lands.
    user.pressOnce = sim::PlayerButton_Use;
    a.Passes(30);
    dent(0.3, 0.0);
    a.Passes(20);
    compare("mid-swing");
    std::printf("[ dent ] after 1: server models %u casts run %u, client models %u\n", a.server.Stats().objectModels,
                a.server.Stats().castsRun, user.client->Stats().models);
    // At rest.
    a.Passes(120);
    dent(0.3, 0.5);                                  // above the first dent
    a.Passes(20);
    compare("at rest");
    std::printf("[ dent ] after 2: server models %u casts run %u, client models %u\n", a.server.Stats().objectModels,
                a.server.Stats().castsRun, user.client->Stats().models);
    EXPECT_GE(user.client->Stats().models, modelsBefore + 2) << "both dents reached the client as models";
}
