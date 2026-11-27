// docs/design/networking.md stage 3: the bytes of the protocol. Events (world operations and
// entity spawns), masks as run lengths, RegionDesc with raw float bits, messages; every decoder
// refuses truncated, oversized and trailing input, and random bytes never crash it.

#include <gtest/gtest.h>

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <limits>
#include <span>
#include <variant>
#include <vector>

import vortex.core;
import vortex.voxel;
import vortex.scene;
import vortex.sim;
import vortex.net;

using namespace vortex;

namespace {
    voxel::VoxelMask RandomMask(Rng& rng, glm::ivec3 size, float density) {
        voxel::VoxelMask mask(size);
        for (int z = 0; z < size.z; ++z)
            for (int y = 0; y < size.y; ++y)
                for (int x = 0; x < size.x; ++x)
                    if (rng.NextFloat() < density) mask.Set(x, y, z, true);
        return mask;
    }

    net::Event RoundTrip(const net::Event& event, size_t* bytes = nullptr) {
        net::ByteWriter w;
        net::WriteEvent(w, event);
        if (bytes) *bytes = w.Size();
        net::ByteReader r(w.Data());
        net::Event out;
        EXPECT_TRUE(net::ReadEvent(r, out));
        EXPECT_TRUE(r.AtEnd());
        return out;
    }

    sim::RegionDesc SomeRegion() {
        sim::RegionDesc r;
        r.templateId = sim::RegionTemplateId::Playground;
        r.seed = 0xdeadbeefu;
        r.playMinChunk = {-7, -3};
        r.playMaxChunk = {9, 11};
        r.heightRange = {-48.0f, 128.0f};
        r.generator = sim::RegionGenerator::Terrain;
        r.flatGroundVoxels = -17;
        r.terrain.seed = 77;
        r.terrain.voxelSize = 0.1f;
        r.terrain.baseHeight = 3.3f;
        r.terrain.horizontalScale = 1.0f / 3.0f;     // a value no decimal text holds exactly
        r.terrain.caves = false;
        voxel::MaterialLibrary scratch;
        r.terrain.materials = voxel::RegisterDefaultWorldMaterials(scratch);
        return r;
    }
}

TEST(Wire, IntegersFloatsAndLeb128RoundTrip) {
    net::ByteWriter w;
    w.U8(0xab);
    w.U16(0xbeef);
    w.U32(0xdeadbeefu);
    w.U64(0x0123456789abcdefull);
    w.I32(-5);
    w.F32(-0.0f);
    w.F32(std::numeric_limits<float>::quiet_NaN());
    w.F64(1.0 / 3.0);
    for (const uint64_t v : {0ull, 127ull, 128ull, 300ull, 0xffffffffffffffffull}) w.VarU(v);
    net::ByteReader r(w.Data());
    EXPECT_EQ(r.U8(), 0xab);
    EXPECT_EQ(r.U16(), 0xbeef);
    EXPECT_EQ(r.U32(), 0xdeadbeefu);
    EXPECT_EQ(r.U64(), 0x0123456789abcdefull);
    EXPECT_EQ(r.I32(), -5);
    EXPECT_EQ(std::bit_cast<uint32_t>(r.F32()), std::bit_cast<uint32_t>(-0.0f));
    EXPECT_TRUE(std::isnan(r.F32()));
    EXPECT_EQ(r.F64(), 1.0 / 3.0);
    for (const uint64_t v : {0ull, 127ull, 128ull, 300ull, 0xffffffffffffffffull}) EXPECT_EQ(r.VarU(), v);
    EXPECT_TRUE(r.AtEnd());
    EXPECT_EQ(r.U8(), 0);          // past the end: 0 and failed from then on
    EXPECT_TRUE(r.Failed());

    const uint8_t endless[12] = {0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x80, 0x01};
    net::ByteReader bad(endless);
    bad.VarU();
    EXPECT_TRUE(bad.Failed());     // more than 10 bytes
}

TEST(Wire, EveryWorldOpRoundTripsAtItsSize) {
    Rng rng(7);
    const voxel::SphereQ s{glm::ivec3(-123456, 789, 2147483), 4095};
    const glm::vec3 v(1.5f, -6.0f, 0.12345f);
    struct Case {
        sim::WorldOp op;
        size_t bytes;       // fixed part (networking.md "World operations")
    };
    voxel::VoxelMask mask = RandomMask(rng, {37, 11, 5}, 0.3f);
    const std::vector<Case> cases = {
        {sim::WorldOp::Carve(s, 1600), 17},
        {sim::WorldOp::Fill(s, 7), 16},
        {sim::WorldOp::Swap(s, 3, 9), 17},
        {sim::WorldOp::Box({-5, 6, -7}, {8, -9, 10}, 200), 26},
        {sim::WorldOp::Detach({-1000, 50, 2000}, mask, v, 0), 29},
        {sim::WorldOp::Detach({1, 2, 3}, voxel::VoxelMask({4, 4, 4}), glm::vec3(0.0f), 77), 29},
        {sim::WorldOp::Clear({-3, -2, -1}, mask), 19},
    };
    for (const Case& c : cases) {
        size_t bytes = 0;
        const net::Event back = RoundTrip(c.op, &bytes);
        ASSERT_TRUE(std::holds_alternative<sim::WorldOp>(back));
        sim::WorldOp expect = c.op;
        expect.velocity = net::QuantizeVelocity(expect.velocity);
        EXPECT_EQ(std::get<sim::WorldOp>(back), expect) << static_cast<int>(c.op.kind);
        const bool masked = c.op.kind == sim::WorldOpKind::Detach || c.op.kind == sim::WorldOpKind::ClearMask;
        if (masked) EXPECT_GT(bytes, c.bytes);
        else EXPECT_EQ(bytes, c.bytes);
        EXPECT_EQ(net::EventBytes(c.op), bytes);
        EXPECT_EQ(sim::OpWireBytes(c.op), bytes) << static_cast<int>(c.op.kind);
    }
    // The velocity is presentation only, on the wire in 1/256 m/s.
    EXPECT_EQ(net::QuantizeVelocity(v), glm::vec3(1.5f, -6.0f, 32.0f / 256.0f));
}

TEST(Wire, LightSpawnRoundTrips) {
    net::EntitySpawn spawn;
    spawn.netId = 4000000000u;
    spawn.light = {.position = glm::dvec3(1.0 / 3.0, -2.5, 1e6), .color = glm::vec3(1.0f, 0.62f, 0.3f), .intensity = 6.0f, .radius = 7.0f};
    size_t bytes = 0;
    const net::Event back = RoundTrip(spawn, &bytes);
    ASSERT_TRUE(std::holds_alternative<net::EntitySpawn>(back));
    EXPECT_EQ(std::get<net::EntitySpawn>(back), spawn);
    EXPECT_EQ(bytes, 1u + 4 + 1 + 24 + 12 + 4 + 4);
}

// Stage 6: objects, despawns, rests and models, bit for bit at their sizes; models as (run, id) pairs.
TEST(Wire, EntityEventsRoundTripAtTheirSizes) {
    voxel::VoxelVolume crate(glm::ivec3(6));
    crate.FillBox(glm::ivec3(0), glm::ivec3(6), 11);
    crate.FillBox(glm::ivec3(1), glm::ivec3(5), 0);
    crate.Set(0, 0, 0, 13);

    net::EntitySpawn object;
    object.netId = 42;
    object.kind = net::EntityKind::Object;
    object.object.position = glm::vec3(std::nextafter(1.0f, 2.0f), -3.25f, 1e-30f);
    object.object.rotation = glm::normalize(glm::quat(0.8f, 0.2f, 0.1f, -0.4f));
    object.object.linearVelocity = glm::vec3(0.5f, -9.0f, 0.0f);
    object.object.angularVelocity = glm::vec3(0.0f, 1.0f, 2.0f);
    object.object.flags = net::kObjectResting;
    object.object.volume = crate;
    size_t bytes = 0;
    net::Event back = RoundTrip(object, &bytes);
    ASSERT_TRUE(std::holds_alternative<net::EntitySpawn>(back));
    EXPECT_EQ(std::get<net::EntitySpawn>(back), object);
    net::ByteWriter runs;
    net::WriteVolumeRuns(runs, crate);
    EXPECT_EQ(bytes, 1u + 4 + 1 + 12 + 16 + 12 + 12 + 1 + 1 + runs.Size());
    EXPECT_LT(runs.Size(), 120u);                                   // a hollow crate is small on the wire

    const net::Event despawn = net::EntityDespawn{42, scene::DespawnReason::ToRubble};
    back = RoundTrip(despawn, &bytes);
    EXPECT_EQ(bytes, 6u);
    EXPECT_EQ(std::get<net::EntityDespawn>(back), std::get<net::EntityDespawn>(despawn));

    const net::Event rest = net::EntityRest{42, 1234, glm::vec3(1.5f, 2.5f, -3.5f), glm::quat(0.0f, 1.0f, 0.0f, 0.0f)};
    back = RoundTrip(rest, &bytes);
    EXPECT_EQ(bytes, 37u);
    EXPECT_EQ(std::get<net::EntityRest>(back), std::get<net::EntityRest>(rest));

    const net::Event wake = net::EntityWake{42, 1250};
    back = RoundTrip(wake, &bytes);
    EXPECT_EQ(bytes, 9u);
    EXPECT_EQ(std::get<net::EntityWake>(back), std::get<net::EntityWake>(wake));

    net::ObjectModel model;
    model.netId = 42;
    model.offset = glm::ivec3(-1, 0, 3);
    model.motion = 2;
    model.volume = crate;
    back = RoundTrip(model, &bytes);
    EXPECT_EQ(bytes, 1u + 4 + 6 + 1 + runs.Size());
    EXPECT_EQ(std::get<net::ObjectModel>(back), model);
    EXPECT_EQ(net::VolumeHash(std::get<net::ObjectModel>(back).volume), net::VolumeHash(crate));
    crate.Set(2, 0, 0, 12);
    EXPECT_NE(net::VolumeHash(crate), net::VolumeHash(model.volume));

    // Refused: an unknown reason, a non-finite pose, runs that do not add up, an oversized model.
    auto refused = [](std::vector<uint8_t> bytes) {
        net::ByteReader in(bytes);
        net::Event e;
        return !net::ReadEvent(in, e);
    };
    {
        net::ByteWriter w;
        net::WriteEvent(w, despawn);
        std::vector<uint8_t> b = w.Take();
        b.back() = 4;
        EXPECT_TRUE(refused(b));
    }
    {
        net::EntityRest bad = std::get<net::EntityRest>(rest);
        bad.position.y = std::nanf("");
        net::ByteWriter w;
        net::WriteEvent(w, bad);
        EXPECT_TRUE(refused(w.Take()));
    }
    {
        net::ByteWriter w;
        w.U8(35);
        w.U32(1);
        for (int i = 0; i < 3; ++i) w.I16(0);
        w.U8(2);
        w.U16(2);
        w.U16(2);
        w.U16(2);
        w.VarU(7);                                                  // 7 of 8 voxels
        w.U8(1);
        EXPECT_TRUE(refused(w.Take()));
        net::ByteWriter big;
        big.U8(35);
        big.U32(1);
        for (int i = 0; i < 3; ++i) big.I16(0);
        big.U8(2);
        big.U16(2000);                                              // over kMaxModelAxis
        big.U16(1);
        big.U16(1);
        big.VarU(2000);
        big.U8(1);
        EXPECT_TRUE(refused(big.Take()));
    }
}

TEST(Wire, MaskRunsRoundTripAndRefuseWhatDoesNotAddUp) {
    Rng rng(11);
    for (const float density : {0.0f, 0.01f, 0.5f, 0.99f, 1.0f}) {
        for (const glm::ivec3 size : {glm::ivec3(1), glm::ivec3(8, 1, 1), glm::ivec3(33, 17, 9), glm::ivec3(0, 4, 4)}) {
            const voxel::VoxelMask mask = RandomMask(rng, size, density);
            net::ByteWriter w;
            net::WriteMaskRuns(w, mask);
            net::ByteReader r(w.Data());
            voxel::VoxelMask back;
            ASSERT_TRUE(net::ReadMaskRuns(r, size, back)) << density;
            EXPECT_TRUE(r.AtEnd());
            EXPECT_EQ(back, mask);
            // The same runs read for a smaller mask: they add up to too much.
            if (mask.VoxelCount() > 1) {
                net::ByteReader small(w.Data());
                voxel::VoxelMask none;
                EXPECT_FALSE(net::ReadMaskRuns(small, size - glm::ivec3(1, 0, 0), none) && small.AtEnd());
            }
        }
    }
    // A run of zero after the first one, a stream that stops short, oversized masks.
    {
        net::ByteWriter w;
        w.VarU(3);
        w.VarU(0);
        w.VarU(5);
        net::ByteReader r(w.Data());
        voxel::VoxelMask m;
        EXPECT_FALSE(net::ReadMaskRuns(r, {8, 1, 1}, m));
    }
    {
        net::ByteWriter w;
        w.VarU(3);
        net::ByteReader r(w.Data());
        voxel::VoxelMask m;
        EXPECT_FALSE(net::ReadMaskRuns(r, {8, 1, 1}, m));
    }
    {
        net::ByteWriter w;
        w.VarU(1ull << 30);
        net::ByteReader r(w.Data());
        voxel::VoxelMask m;
        EXPECT_FALSE(net::ReadMaskRuns(r, {1024, 1024, 1024}, m));    // over kMaxMaskVoxels: refused before allocating
        EXPECT_FALSE(net::ReadMaskRuns(r, {2000, 1, 1}, m));          // over kMaxMaskAxis
    }
}

TEST(Wire, RegionRoundTripsBitForBit) {
    const sim::RegionDesc region = SomeRegion();
    net::ByteWriter w;
    net::WriteRegion(w, region);
    net::ByteReader r(w.Data());
    sim::RegionDesc back;
    ASSERT_TRUE(net::ReadRegion(r, back));
    EXPECT_TRUE(r.AtEnd());
    EXPECT_TRUE(back == region);
    EXPECT_EQ(std::bit_cast<uint32_t>(back.terrain.horizontalScale), std::bit_cast<uint32_t>(1.0f / 3.0f));
    EXPECT_EQ(std::bit_cast<uint32_t>(back.terrain.voxelSize), std::bit_cast<uint32_t>(0.1f));
    sim::RegionDesc other = region;
    other.terrain.voxelSize = std::nextafter(0.1f, 1.0f);       // one bit: another session
    EXPECT_FALSE(other == region);
}

TEST(Wire, MessagesRoundTripAndRefuseTrailingOrMissingBytes) {
    net::Hello hello;
    hello.buildHash = 0x1234567890abcdefull;
    for (size_t i = 0; i < hello.token.size(); ++i) hello.token[i] = static_cast<uint8_t>(i * 17);
    net::Welcome welcome;
    welcome.slot = 3;
    welcome.tick = 999;
    welcome.region = SomeRegion();
    welcome.spawn = glm::dvec3(1.25, 12.0, -3.5);
    welcome.joinOpSeq = 77;
    welcome.player.runSpeed = std::nextafter(7.0f, 8.0f);          // bit for bit
    welcome.player.castCooldownTicks = 9;
    welcome.tickSeconds = std::nextafter(1.0f / 30.0f, 1.0f);   // bit for bit
    welcome.gravity = glm::vec3(0.5f, -12.25f, -0.0f);
    welcome.probes = {{glm::ivec3(1, -2, 3), 0xfeedull}, {glm::ivec3(-9, 8, 7), 0xbeefull}};
    net::EventBatch batch;
    batch.firstOpSeq = 5;
    batch.tick = 60;
    batch.events = {sim::WorldOp::Box({0, 0, 0}, {4, 4, 4}, 1), sim::WorldOp::Carve(voxel::SphereQ{glm::ivec3(8), 32}, 16)};

    {
        net::Hello h;
        const std::vector<uint8_t> bytes = net::Encode(hello);
        ASSERT_TRUE(net::Decode(bytes, h));
        EXPECT_EQ(h.buildHash, hello.buildHash);
        EXPECT_EQ(h.token, hello.token);
        EXPECT_EQ(h.protocolVersion, net::kProtocolVersion);
        std::vector<uint8_t> longer = bytes;
        longer.push_back(0);
        EXPECT_FALSE(net::Decode(longer, h));
        EXPECT_FALSE(net::Decode(std::span(bytes).first(bytes.size() - 1), h));
        net::Welcome wrongType;
        EXPECT_FALSE(net::Decode(bytes, wrongType));
    }
    {
        net::Welcome w;
        const std::vector<uint8_t> bytes = net::Encode(welcome);
        ASSERT_TRUE(net::Decode(bytes, w));
        EXPECT_EQ(w.slot, 3);
        EXPECT_EQ(w.tick, 999u);
        EXPECT_TRUE(w.region == welcome.region);
        EXPECT_TRUE(w.player == welcome.player);
        EXPECT_EQ(w.spawn, welcome.spawn);
        EXPECT_EQ(std::bit_cast<uint32_t>(w.tickSeconds), std::bit_cast<uint32_t>(welcome.tickSeconds));
        EXPECT_EQ(std::bit_cast<uint32_t>(w.gravity.z), std::bit_cast<uint32_t>(welcome.gravity.z));
        EXPECT_EQ(w.gravity, welcome.gravity);
        EXPECT_EQ(w.joinOpSeq, 77u);
        EXPECT_EQ(w.probes, welcome.probes);
        EXPECT_FALSE(net::Decode(std::span(bytes).first(bytes.size() - 1), w));
    }
    {
        net::EventBatch b;
        const std::vector<uint8_t> bytes = net::Encode(batch);
        ASSERT_TRUE(net::Decode(bytes, b));
        EXPECT_EQ(b.firstOpSeq, 5u);
        EXPECT_EQ(b.tick, 60u);
        ASSERT_EQ(b.events.size(), 2u);
        EXPECT_EQ(std::get<sim::WorldOp>(b.events[1]), std::get<sim::WorldOp>(batch.events[1]));
        // Header: type, opSeq, tick, count.
        EXPECT_EQ(bytes.size(), net::kEventBatchHeaderBytes + 26 + 17);
    }
    {
        // Inputs: consecutive ticks from the header, 7 bytes each.
        net::InputBatch inputs;
        inputs.firstTick = 4000000000u;
        for (int i = 0; i < 8; ++i) {
            sim::PlayerInput in;
            in.moveX = static_cast<int8_t>(-128 + i);
            in.moveZ = 127;
            in.yaw = static_cast<uint16_t>(65535 - i);
            in.pitch = static_cast<int16_t>(-32768 + i);
            in.buttons = static_cast<uint8_t>(i);
            inputs.inputs.push_back(in);
        }
        const std::vector<uint8_t> bytes = net::Encode(inputs);
        EXPECT_EQ(bytes.size(), net::kInputBatchHeaderBytes + 8 * net::kInputBytes);
        net::InputBatch back;
        ASSERT_TRUE(net::Decode(bytes, back));
        ASSERT_EQ(back.inputs.size(), 8u);
        for (uint32_t i = 0; i < 8; ++i) {
            sim::PlayerInput expect = inputs.inputs[i];
            expect.tick = inputs.firstTick + i;
            EXPECT_EQ(back.inputs[i], expect);
        }
        EXPECT_FALSE(net::Decode(std::span(bytes).first(bytes.size() - 1), back));
        net::InputBatch none;
        none.firstTick = 1;
        EXPECT_FALSE(net::Decode(net::Encode(none), back));      // at least one input

        // Snapshots: header 21, owner 32 (stage 5: lastCastTick, castRejected), 16 per other player;
        // positions bit for bit.
        net::Snapshot snap;
        snap.tick = 120;
        snap.lastOpSeq = 4567;
        snap.ackInputTick = 125;
        snap.marginTick = 131;
        snap.inputMargin = -3;
        snap.hasOwner = true;
        snap.owner.position = glm::vec3(std::nextafter(12.5f, 13.0f), -0.0f, 1.0e-30f);
        snap.owner.velocity = glm::vec3(1.0f, -9.81f, 0.25f);
        snap.owner.flags = sim::PlayerFlag_OnGround;
        snap.owner.cooldownTicks = 7;
        snap.owner.lastButtons = sim::PlayerButton_Run;
        snap.lastCastTick = 117;
        snap.castRejected = net::CastRejection::Missed;
        snap.others = {{2, glm::vec3(1.0f, 2.0f, 3.0f), 1234, 1}, {9, glm::vec3(-4.0f, 5.0f, -6.0f), 65535, 2}};
        // Stage 6: awake bodies, 31 bytes each (position raw, rotation 4 x int16, velocity 1/256 m/s).
        net::BodySample body;
        body.netId = 77;
        body.position = glm::vec3(std::nextafter(3.0f, 4.0f), 20.5f, -1.0e-20f);
        body.rotation = glm::normalize(glm::quat(0.9f, 0.1f, -0.3f, 0.2f));
        body.velocity = glm::vec3(1.0f / 256.0f, -12.5f, 200.0f);       // 200 m/s is clamped to 127.99
        snap.bodies = {body};
        const std::vector<uint8_t> sb = net::Encode(snap);
        EXPECT_EQ(sb.size(), net::kSnapshotHeaderBytes + net::kSnapshotOwnerBytes + 2 * net::kSnapshotPlayerBytes + net::kSnapshotBodyBytes);
        net::Snapshot sback;
        ASSERT_TRUE(net::Decode(sb, sback));
        EXPECT_EQ(sback.tick, 120u);
        EXPECT_EQ(sback.lastOpSeq, 4567u);
        EXPECT_EQ(sback.ackInputTick, 125u);
        EXPECT_EQ(sback.marginTick, 131u);
        EXPECT_EQ(sback.inputMargin, -3);
        ASSERT_TRUE(sback.hasOwner);
        EXPECT_EQ(sback.owner, snap.owner);
        EXPECT_EQ(sback.lastCastTick, 117u);
        EXPECT_EQ(sback.castRejected, net::CastRejection::Missed);
        EXPECT_EQ(sback.others, snap.others);
        ASSERT_EQ(sback.bodies.size(), 1u);
        EXPECT_EQ(sback.bodies[0].netId, 77u);
        EXPECT_EQ(sback.bodies[0].position, body.position);                                   // bit for bit
        EXPECT_LT(glm::length(glm::vec4(sback.bodies[0].rotation.x - body.rotation.x, sback.bodies[0].rotation.y - body.rotation.y,
                                        sback.bodies[0].rotation.z - body.rotation.z, sback.bodies[0].rotation.w - body.rotation.w)),
                  1e-4f);
        EXPECT_EQ(sback.bodies[0].velocity, net::QuantizeVelocity(body.velocity));
        EXPECT_FALSE(net::Decode(std::span(sb).first(sb.size() - 1), sback));
        {
            std::vector<uint8_t> bad = sb;                       // an unknown rejection is refused (14 BadGesture is the last, [mag-8])
            bad[net::kSnapshotHeaderBytes - 2 + 31] = 15;
            EXPECT_FALSE(net::Decode(bad, sback));
        }
        {
            std::vector<uint8_t> bad = sb;                       // a zero rotation is refused
            for (size_t i = 0; i < 8; ++i) bad[sb.size() - net::kSnapshotBodyBytes + 16 + i] = 0;
            EXPECT_FALSE(net::Decode(bad, sback));
        }
        snap.owner.velocity.y = std::nanf("");                   // a non-finite state is refused
        EXPECT_FALSE(net::Decode(net::Encode(snap), sback));
        net::Snapshot empty;
        EXPECT_EQ(net::Encode(empty).size(), net::kSnapshotHeaderBytes);
        ASSERT_TRUE(net::Decode(net::Encode(empty), sback));
        EXPECT_FALSE(sback.hasOwner);
        EXPECT_EQ(sback.inputMargin, net::kNoInputMargin);

        net::Refuse refuse;
        ASSERT_TRUE(net::Decode(net::Encode(net::Refuse{net::RefuseReason::Token}), refuse));
        EXPECT_EQ(refuse.reason, net::RefuseReason::Token);
    }
}

// Forged packets (stage 4 tests them on a live server; here the decoders): random bytes, and
// valid messages with random bytes flipped, never crash and never allocate unbounded.
TEST(Wire, ForgedBytesNeverCrashADecoder) {
    Rng rng(99);
    net::EventBatch batch;
    batch.firstOpSeq = 1;
    voxel::VoxelMask mask = RandomMask(rng, {20, 20, 20}, 0.2f);
    batch.events = {sim::WorldOp::Detach({0, 0, 0}, mask, glm::vec3(1.0f)), sim::WorldOp::Clear({1, 2, 3}, mask)};
    net::InputBatch inputs;
    inputs.firstTick = 77;
    inputs.inputs.resize(8);
    net::Snapshot snap;
    snap.hasOwner = true;
    snap.others.resize(3);
    snap.bodies.resize(4);
    net::EventBatch entities;
    entities.firstOpSeq = 9;
    net::EntitySpawn object;
    object.netId = 5;
    object.kind = net::EntityKind::Object;
    object.object.volume = voxel::VoxelVolume(glm::ivec3(6));
    object.object.volume.FillBox(glm::ivec3(1), glm::ivec3(4), 3);
    net::ObjectModel model;
    model.netId = 5;
    model.volume = object.object.volume;
    entities.events = {object, net::EntityDespawn{5, scene::DespawnReason::Broken}, net::EntityRest{5, 7, glm::vec3(1.0f), glm::quat(1, 0, 0, 0)},
                       model, net::EntityWake{5, 9}};
    // Stage 7a's messages too.
    net::WindowAck ack;
    ack.modified = RandomMask(rng, {12, 8, 12}, 0.1f);
    net::ChunkData chunk;
    chunk.rle = {2, 3, 0x00, 0x40, 4, 0x00, 0x40};
    net::BaselineEntity baseline;
    baseline.spawn = object;
    const std::vector<std::vector<uint8_t>> valid = {net::Encode(batch), net::Encode(net::Welcome{}), net::Encode(net::Hello{}),
                                                     net::Encode(inputs), net::Encode(snap), net::Encode(entities),
                                                     net::Encode(net::WindowUpdate{1, 0, {1, 2, 3}, {4, 5, 6}}), net::Encode(ack),
                                                     net::Encode(chunk), net::Encode(net::ChunkRequest{{{1, 2, 3}, {4, 5, 6}}}),
                                                     net::Encode(net::ModelRequest{{7, 8}}), net::Encode(baseline),
                                                     net::Encode(net::BaselineEnd{3, 4}), net::Encode(net::BaselineNear{5, 6}),
                                                     // Stage 7b's messages.
                                                     net::Encode(net::TerrainAck{2, 9, {4, 2, 3}, {{5, net::TerrainCode::Uniform, 3}, {7, net::TerrainCode::Data, 0},
                                                                                                   {12, net::TerrainCode::NotInAck, 0}}}),
                                                     net::Encode(net::HashCheck{4, {{{1, 2, 3}, 77, true}, {{-1, 0, 5}, 78, false}}}),
                                                     net::Encode(net::EntityCheck{5, 3, 7, 9}), net::Encode(net::EntityListRequest{6}),
                                                     net::Encode(net::EntityList{7, {{3, 11}, {9, 12}}}),
                                                     net::Encode(net::ChunkRequest{{{1, 2, 3}}, net::ChunkRequestReason::Repair})};
    uint32_t decoded = 0;
    for (int i = 0; i < 4000; ++i) {
        std::vector<uint8_t> bytes;
        if (i % 2 == 0) {
            bytes.resize(rng.NextBounded(200));
            for (uint8_t& b : bytes) b = static_cast<uint8_t>(rng.NextU32());
            if (!bytes.empty()) bytes[0] = static_cast<uint8_t>(1 + rng.NextBounded(20));
        } else {
            bytes = valid[rng.NextBounded(static_cast<uint32_t>(valid.size()))];
            for (int k = 0; k < 3; ++k) bytes[rng.NextBounded(static_cast<uint32_t>(bytes.size()))] = static_cast<uint8_t>(rng.NextU32());
        }
        net::Hello h;
        net::Welcome w;
        net::EventBatch b;
        net::InputBatch in;
        net::Snapshot s;
        net::Refuse r;
        decoded += net::Decode(bytes, h) + net::Decode(bytes, w) + net::Decode(bytes, b) + net::Decode(bytes, in) + net::Decode(bytes, s) +
                   net::Decode(bytes, r);
        net::WindowUpdate wu;
        net::WindowAck wa;
        net::ChunkData cd;
        net::ChunkRequest cr;
        net::ModelRequest mr;
        net::BaselineEntity be;
        net::BaselineEnd bn;
        net::BaselineNear bnear;
        decoded += net::Decode(bytes, wu) + net::Decode(bytes, wa) + net::Decode(bytes, cd) + net::Decode(bytes, cr) + net::Decode(bytes, mr) +
                   net::Decode(bytes, be) + net::Decode(bytes, bn) + net::Decode(bytes, bnear);
        net::TerrainAck ta;
        net::HashCheck hc;
        net::EntityCheck ec;
        net::EntityListRequest elr;
        net::EntityList el;
        decoded += net::Decode(bytes, ta) + net::Decode(bytes, hc) + net::Decode(bytes, ec) + net::Decode(bytes, elr) + net::Decode(bytes, el);
        // A TerrainAck that decodes never codes more cells than its box (protocol 10: it codes the
        // entering cells, at most the whole box; the client checks they are exactly its entering ones).
        if (net::Decode(bytes, ta)) {
            uint64_t cells = 0;
            for (const net::TerrainRun& r : ta.runs) cells += r.count;
            EXPECT_LE(cells, static_cast<uint64_t>(ta.size.x) * static_cast<uint64_t>(ta.size.y) * static_cast<uint64_t>(ta.size.z));
        }
    }
    SUCCEED() << decoded << " forged messages happened to decode";
}

// One batch decodes at most kMaxMaskVoxels of masks in all (16 MiB of bits), checked before each
// mask is allocated: a 25-byte ClearMask of the largest size cannot be repeated into gigabytes.
TEST(Wire, ABatchDecodesABoundedMaskVolume) {
    const glm::ivec3 largest(net::kMaxMaskAxis, net::kMaxMaskAxis, static_cast<int>(net::kMaxMaskVoxels / (1024u * 1024u)));
    voxel::VoxelMask full(largest);
    full.SetRange(0, full.VoxelCount());
    net::ByteWriter one;
    net::WriteEvent(one, sim::WorldOp::Clear({0, 0, 0}, std::move(full)));
    EXPECT_LT(one.Size(), 32u);
    auto batchOf = [&one](uint32_t count) {
        std::vector<uint8_t> events;
        for (uint32_t i = 0; i < count; ++i) events.insert(events.end(), one.Data().begin(), one.Data().end());
        return net::EncodeBatch(1, 0, count, events);
    };
    net::EventBatch out;
    EXPECT_TRUE(net::Decode(batchOf(1), out));             // the largest mask alone fits
    EXPECT_FALSE(net::Decode(batchOf(4), out));
    EXPECT_FALSE(net::Decode(batchOf(600), out));          // 10 GiB of masks before the bound
    // Two halves fit exactly.
    voxel::VoxelMask half(glm::ivec3(largest.x, largest.y, largest.z / 2));
    half.SetRange(0, half.VoxelCount() / 3);
    net::EventBatch two;
    two.firstOpSeq = 1;
    two.events = {sim::WorldOp::Clear({0, 0, 0}, half), sim::WorldOp::Clear({0, 0, 0}, half)};
    EXPECT_TRUE(net::Decode(net::Encode(two), out));
    ASSERT_EQ(out.events.size(), 2u);
}

// Stage 7a (protocol 8): every chunk-sync message round-trips at its size and is refused when it is
// truncated, has trailing bytes, or breaks its rules (window size, coordinates, codec stream, counts,
// an ack whose runs do not fill its box, a baseline event that is not a spawn with a NetId).
TEST(Wire, ChunkSyncMessagesRoundTripAtTheirSizes) {
    auto refusedCut = [](const std::vector<uint8_t>& bytes, auto decoded) {
        std::vector<uint8_t> shorter(bytes.begin(), bytes.end() - 1), longer = bytes;
        longer.push_back(0);
        return !net::Decode(shorter, decoded) && !net::Decode(longer, decoded);
    };
    const net::WindowUpdate u{3, net::WindowUpdate::kReset, {-5, 0, 7}, {48, 44, 48}};
    const std::vector<uint8_t> ub = net::Encode(u);
    EXPECT_EQ(ub.size(), net::kWindowUpdateBytes);
    net::WindowUpdate u2;
    ASSERT_TRUE(net::Decode(ub, u2));
    EXPECT_EQ(u2.windowSeq, 3u);
    EXPECT_EQ(u2.flags, net::WindowUpdate::kReset);
    EXPECT_EQ(u2.minChunk, u.minChunk);
    EXPECT_EQ(u2.size, u.size);
    EXPECT_TRUE(refusedCut(ub, net::WindowUpdate{}));
    for (const net::WindowUpdate& bad : {net::WindowUpdate{1, 4, {0, 0, 0}, {4, 4, 4}},
                                         net::WindowUpdate{1, 0, {0, 0, 0}, {0, 4, 4}}, net::WindowUpdate{1, 0, {0, 0, 0}, {129, 4, 4}},
                                         net::WindowUpdate{1, 0, {1 << 25, 0, 0}, {4, 4, 4}}})
        EXPECT_FALSE(net::Decode(net::Encode(bad), u2));

    Rng rng(7);
    net::WindowAck ack;
    ack.windowSeq = 2;
    ack.asOfOpSeq = 99;
    ack.modified = RandomMask(rng, {30, 44, 30}, 0.01f);
    const std::vector<uint8_t> ab = net::Encode(ack);
    net::WindowAck ack2;
    ASSERT_TRUE(net::Decode(ab, ack2));
    EXPECT_EQ(ack2.windowSeq, 2u);
    EXPECT_EQ(ack2.asOfOpSeq, 99u);
    EXPECT_EQ(ack2.modified, ack.modified);
    EXPECT_TRUE(refusedCut(ab, net::WindowAck{}));
    std::printf("[ wire ] ack of a 30 x 44 x 30 window with %u modified chunks: %zu bytes\n", ack.modified.Count(), ab.size());

    // A chunk: every codec kind; a stream that does not add up to 32 768 voxels is refused.
    for (const std::vector<uint8_t>& rle : {std::vector<uint8_t>{0}, std::vector<uint8_t>{1, 7}, std::vector<uint8_t>{2, 3, 0x00, 0x40, 4, 0x00, 0x40}}) {
        const net::ChunkData d{{1, -2, 3}, 42, rle};
        const std::vector<uint8_t> db = net::Encode(d);
        EXPECT_EQ(db.size(), net::kChunkDataHeaderBytes + rle.size());
        net::ChunkData d2;
        ASSERT_TRUE(net::Decode(db, d2));
        EXPECT_EQ(d2.chunk, d.chunk);
        EXPECT_EQ(d2.asOfOpSeq, 42u);
        EXPECT_EQ(d2.rle, rle);
    }
    for (const std::vector<uint8_t>& bad : {std::vector<uint8_t>{}, std::vector<uint8_t>{3}, std::vector<uint8_t>{1, 0}, std::vector<uint8_t>{0, 0},
                                            std::vector<uint8_t>{2, 3, 0x00, 0x40}, std::vector<uint8_t>{2, 3, 0x00, 0x00, 4, 0x00, 0x80}}) {
        net::ChunkData d2;
        EXPECT_FALSE(net::Decode(net::Encode(net::ChunkData{{0, 0, 0}, 1, bad}), d2));
    }

    const net::ChunkRequest cr{{{1, 2, 3}, {-4, 5, -6}}};
    const std::vector<uint8_t> crb = net::Encode(cr);
    EXPECT_EQ(crb.size(), 4u + 12u * 2u);      // protocol 9: the reason byte
    net::ChunkRequest cr2;
    ASSERT_TRUE(net::Decode(crb, cr2));
    EXPECT_EQ(cr2.chunks, cr.chunks);
    EXPECT_EQ(cr2.reason, net::ChunkRequestReason::Sync);
    EXPECT_TRUE(refusedCut(crb, net::ChunkRequest{}));
    EXPECT_FALSE(net::Decode(net::Encode(net::ChunkRequest{}), cr2));
    EXPECT_FALSE(net::Decode(net::Encode(net::ChunkRequest{std::vector<glm::ivec3>(net::kMaxChunkRequest + 1)}), cr2));

    const net::ModelRequest mr{{5, 9}};
    const std::vector<uint8_t> mrb = net::Encode(mr);
    EXPECT_EQ(mrb.size(), 3u + 4u * 2u);
    net::ModelRequest mr2;
    ASSERT_TRUE(net::Decode(mrb, mr2));
    EXPECT_EQ(mr2.netIds, mr.netIds);
    EXPECT_FALSE(net::Decode(net::Encode(net::ModelRequest{{5, 0}}), mr2));
    EXPECT_FALSE(net::Decode(net::Encode(net::ModelRequest{std::vector<uint32_t>(net::kMaxModelRequest + 1, 3)}), mr2));

    net::BaselineEntity b;
    b.asOfOpSeq = 12;
    b.tick = 400;
    b.restTick = 390;
    b.spawn.netId = 8;
    b.spawn.kind = net::EntityKind::Object;
    b.spawn.object.position = glm::vec3(1.5f, 2.25f, -3.0f);
    b.spawn.object.flags = net::kObjectResting;
    b.spawn.object.volume = voxel::VoxelVolume(glm::ivec3(6));
    b.spawn.object.volume.FillBox(glm::ivec3(0), glm::ivec3(6), 3);
    const std::vector<uint8_t> bb = net::Encode(b);
    EXPECT_EQ(bb.size(), 13u + net::EventBytes(net::Event{b.spawn}));
    net::BaselineEntity b2;
    ASSERT_TRUE(net::Decode(bb, b2));
    EXPECT_EQ(b2.asOfOpSeq, 12u);
    EXPECT_EQ(b2.tick, 400u);
    EXPECT_EQ(b2.restTick, 390u);
    EXPECT_EQ(b2.spawn, b.spawn);
    EXPECT_TRUE(refusedCut(bb, net::BaselineEntity{}));
    net::BaselineEntity noId = b;
    noId.spawn.netId = 0;
    EXPECT_FALSE(net::Decode(net::Encode(noId), b2));

    const std::vector<uint8_t> eb = net::Encode(net::BaselineEnd{12, 34, {}});
    EXPECT_EQ(eb.size(), 10u);                                  // [mag-2]: + the zone count
    net::BaselineEnd e2;
    ASSERT_TRUE(net::Decode(eb, e2));
    EXPECT_EQ(e2.asOfOpSeq, 12u);
    EXPECT_EQ(e2.count, 34u);
    EXPECT_TRUE(refusedCut(eb, net::BaselineEnd{}));

    // Review of stage 7a: the near-set marker, BaselineEnd's first 9 bytes under type 15.
    const std::vector<uint8_t> nb = net::Encode(net::BaselineNear{56, 78});
    EXPECT_EQ(nb.size(), 9u);
    EXPECT_EQ(nb[0], 15u);
    net::BaselineNear n2;
    ASSERT_TRUE(net::Decode(nb, n2));
    EXPECT_EQ(n2.asOfOpSeq, 56u);
    EXPECT_EQ(n2.count, 78u);
    EXPECT_TRUE(refusedCut(nb, net::BaselineNear{}));
    EXPECT_FALSE(net::Decode(eb, n2)) << "a BaselineEnd is not a BaselineNear";
}

// Stage 7b (protocol 9, 10 since its review; networking.md "Stage 7b plan" and its amendments): the
// check and terrain-mode messages round-trip at their sizes and are refused when truncated, with
// trailing bytes, or when they break their rules - a check count of 0 or 33, unknown flag or reason
// bits, TerrainAck runs past the box or not adding up to their count (short or long), a run of 0,
// code 4, code 2 without its id, list NetIds not ascending. A decoder that took a short run would
// install chunks outside the box.
TEST(Wire, CheckAndTerrainMessagesRoundTripAtTheirSizes) {
    auto refusedCut = [](const std::vector<uint8_t>& bytes, auto decoded) {
        std::vector<uint8_t> shorter(bytes.begin(), bytes.end() - 1), longer = bytes;
        longer.push_back(0);
        return !net::Decode(shorter, decoded) && !net::Decode(longer, decoded);
    };
    EXPECT_EQ(net::kProtocolVersion, 15u);   // [mag-8]: casting by mana and gesture

    // [wp-7] The fight's messages round-trip and refuse a byte too few or too many (FightWire.* for the values).
    {
        net::Roster roster;
        roster.players = {{1, 2, 3, 4}};
        net::Roster r;
        ASSERT_TRUE(net::Decode(net::Encode(roster), r));
        EXPECT_EQ(r, roster);
        EXPECT_TRUE(refusedCut(net::Encode(roster), net::Roster{}));
        net::RoundUpdate round;
        round.phase = sim::RoundPhase::Live;
        round.score = {2, 1};
        net::RoundUpdate u2;
        ASSERT_TRUE(net::Decode(net::Encode(round), u2));
        EXPECT_EQ(u2, round);
        EXPECT_TRUE(refusedCut(net::Encode(round), net::RoundUpdate{}));
        const net::KillNotice kill{5, 0, 2, sim::DamageCause::Fall, 0};
        net::KillNotice k;
        ASSERT_TRUE(net::Decode(net::Encode(kill), k));
        EXPECT_EQ(k, kill);
        EXPECT_TRUE(refusedCut(net::Encode(kill), net::KillNotice{}));
    }

    // WindowUpdate accepts the terrain-mode bit (the server decides what it means).
    net::WindowUpdate u;
    ASSERT_TRUE(net::Decode(net::Encode(net::WindowUpdate{4, net::WindowUpdate::kReset | net::WindowUpdate::kTerrainMode, {0, 0, 0}, {4, 4, 4}}), u));
    EXPECT_EQ(u.flags, net::WindowUpdate::kReset | net::WindowUpdate::kTerrainMode);

    // ChunkRequest with its reason; an unknown reason is refused.
    const net::ChunkRequest cr{{{1, 2, 3}}, net::ChunkRequestReason::Repair};
    std::vector<uint8_t> crb = net::Encode(cr);
    EXPECT_EQ(crb.size(), 4u + 12u);
    net::ChunkRequest cr2;
    ASSERT_TRUE(net::Decode(crb, cr2));
    EXPECT_EQ(cr2.reason, net::ChunkRequestReason::Repair);
    crb[1] = 2;
    EXPECT_FALSE(net::Decode(crb, cr2));
    crb[1] = 7;
    EXPECT_FALSE(net::Decode(crb, cr2));

    // HashCheck: 6 + 21 n bytes, 1..32 entries, flag bit 0 only.
    net::HashCheck h;
    h.asOfOpSeq = 1234;
    for (int i = 0; i < 16; ++i) h.entries.push_back({{i, -i, 2 * i}, 0x0123456789abcdefull + static_cast<uint64_t>(i), i % 3 == 0});
    const std::vector<uint8_t> hb = net::Encode(h);
    EXPECT_EQ(hb.size(), net::kHashCheckHeaderBytes + 16u * net::kHashCheckEntryBytes);
    EXPECT_EQ(hb.size(), 342u);
    net::HashCheck h2;
    ASSERT_TRUE(net::Decode(hb, h2));
    EXPECT_EQ(h2.asOfOpSeq, 1234u);
    EXPECT_EQ(h2.entries, h.entries);
    EXPECT_TRUE(refusedCut(hb, net::HashCheck{}));
    EXPECT_FALSE(net::Decode(net::Encode(net::HashCheck{1, {}}), h2)) << "a check of 0 entries";
    EXPECT_FALSE(net::Decode(net::Encode(net::HashCheck{1, std::vector<net::HashCheckEntry>(33)}), h2)) << "33 entries";
    EXPECT_TRUE(net::Decode(net::Encode(net::HashCheck{1, std::vector<net::HashCheckEntry>(32)}), h2));
    std::vector<uint8_t> flagged = net::Encode(net::HashCheck{1, {{{1, 1, 1}, 5, true}}});
    flagged.back() = 2;
    EXPECT_FALSE(net::Decode(flagged, h2)) << "an unknown flag bit";
    EXPECT_FALSE(net::Decode(net::Encode(net::HashCheck{1, {{{1 << 25, 0, 0}, 5, false}}}), h2)) << "a coordinate out of range";

    // EntityCheck 21 bytes, EntityListRequest 5.
    const net::EntityCheck ec{77, 300, 0xdeadbeef, 0x1122334455667788ull};
    const std::vector<uint8_t> eb = net::Encode(ec);
    EXPECT_EQ(eb.size(), net::kEntityCheckBytes);
    net::EntityCheck ec2;
    ASSERT_TRUE(net::Decode(eb, ec2));
    EXPECT_EQ(ec2, ec);
    EXPECT_TRUE(refusedCut(eb, net::EntityCheck{}));
    const std::vector<uint8_t> lrb = net::Encode(net::EntityListRequest{88});
    EXPECT_EQ(lrb.size(), net::kEntityListRequestBytes);
    net::EntityListRequest lr2;
    ASSERT_TRUE(net::Decode(lrb, lr2));
    EXPECT_EQ(lr2.asOfOpSeq, 88u);
    EXPECT_TRUE(refusedCut(lrb, net::EntityListRequest{}));

    // EntityList: 9 + 12 n bytes, NetIds ascending and not 0.
    net::EntityList list;
    list.asOfOpSeq = 99;
    for (uint32_t i = 1; i <= 300; ++i) list.entries.push_back({i * 3, 0xabcdull * i});
    const std::vector<uint8_t> lb = net::Encode(list);
    EXPECT_EQ(lb.size(), net::kEntityListHeaderBytes + 300u * net::kEntityListEntryBytes);
    net::EntityList list2;
    ASSERT_TRUE(net::Decode(lb, list2));
    EXPECT_EQ(list2.asOfOpSeq, 99u);
    EXPECT_EQ(list2.entries, list.entries);
    EXPECT_TRUE(refusedCut(lb, net::EntityList{}));
    EXPECT_TRUE(net::Decode(net::Encode(net::EntityList{5, {}}), list2)) << "an empty list (no objects) is valid";
    EXPECT_FALSE(net::Decode(net::Encode(net::EntityList{5, {{0, 1}}}), list2));
    EXPECT_FALSE(net::Decode(net::Encode(net::EntityList{5, {{4, 1}, {4, 2}}}), list2));
    EXPECT_FALSE(net::Decode(net::Encode(net::EntityList{5, {{4, 1}, {3, 2}}}), list2));
    std::printf("[ wire ] HashCheck of 16 entries %zu B, EntityCheck %zu B, EntityList of 300 objects %zu B\n", hb.size(), eb.size(), lb.size());

    // TerrainAck (protocol 10): 15 + the cells it codes (LEB128) + runs; a run is LEB128 count, code,
    // and the id for code 2.
    net::TerrainAck ta;
    ta.windowSeq = 3;
    ta.asOfOpSeq = 40;
    ta.size = {4, 3, 2};    // 24 cells
    ta.runs = {{10, net::TerrainCode::Uniform, 0}, {200 - 190, net::TerrainCode::Data, 0}, {1, net::TerrainCode::HiddenData, 0},
               {2, net::TerrainCode::Uniform, 7}, {1, net::TerrainCode::NotInAck, 0}};
    const std::vector<uint8_t> tb = net::Encode(ta);
    EXPECT_EQ(tb.size(), 15u + 1u + 3u + 2u + 2u + 3u + 2u);
    net::TerrainAck ta2;
    ASSERT_TRUE(net::Decode(tb, ta2));
    EXPECT_EQ(ta2.windowSeq, 3u);
    EXPECT_EQ(ta2.asOfOpSeq, 40u);
    EXPECT_EQ(ta2.size, ta.size);
    EXPECT_EQ(ta2.runs, ta.runs);
    EXPECT_TRUE(refusedCut(tb, net::TerrainAck{}));
    // A run of more than 127 cells (two LEB128 bytes), over the game's whole window.
    net::TerrainAck big{1, 1, {48, 44, 48}, {{48u * 44u * 48u - 1u, net::TerrainCode::Uniform, 0}, {1, net::TerrainCode::Data, 0}}};
    EXPECT_EQ(net::Encode(big).size(), 15u + 3u + 3u + 2u + 2u);
    ASSERT_TRUE(net::Decode(net::Encode(big), ta2));
    EXPECT_EQ(ta2.runs, big.runs);
    auto refused = [&](std::vector<net::TerrainRun> runs) {
        net::TerrainAck bad = ta;
        bad.runs = std::move(runs);
        net::TerrainAck out;
        return !net::Decode(net::Encode(bad), out);
    };
    EXPECT_FALSE(refused({{23, net::TerrainCode::Uniform, 0}})) << "a step codes fewer cells than its box";
    EXPECT_FALSE(refused({})) << "an update that nothing entered with";
    EXPECT_TRUE(refused({{25, net::TerrainCode::Uniform, 0}})) << "runs past the box";
    // Runs that do not add up to the count they follow (byte 15 is the count, 24 here).
    std::vector<uint8_t> over = tb;
    ASSERT_EQ(over[15], 24u);
    over[15] = 23;
    EXPECT_FALSE(net::Decode(over, ta2)) << "the runs past their count";
    over[15] = 25;
    EXPECT_FALSE(net::Decode(over, ta2)) << "the runs short of their count";
    EXPECT_TRUE(refused({{20, net::TerrainCode::Uniform, 0}, {5, net::TerrainCode::Data, 0}})) << "the last run past the box";
    EXPECT_TRUE(refused({{0, net::TerrainCode::Data, 0}, {24, net::TerrainCode::Data, 0}})) << "a run of 0";
    EXPECT_TRUE(refused({{24, static_cast<net::TerrainCode>(4), 0}})) << "code 4";
    // Code 2 without its id: the id byte is missing at the end.
    std::vector<uint8_t> noId = net::Encode(net::TerrainAck{1, 1, {4, 3, 2}, {{24, net::TerrainCode::Uniform, 5}}});
    noId.pop_back();
    EXPECT_FALSE(net::Decode(noId, ta2));
    EXPECT_FALSE(net::Decode(net::Encode(net::TerrainAck{1, 1, {0, 3, 2}, {}}), ta2)) << "an empty box";
    EXPECT_FALSE(net::Decode(net::Encode(net::TerrainAck{1, 1, {129, 3, 2}, {{129u * 6u, net::TerrainCode::Data, 0}}}), ta2));

    // The model term of EntityCheck: two crates (one model) do not cancel.
    EXPECT_NE(net::EntityModelTerm(3, 42) ^ net::EntityModelTerm(4, 42), 0u);
    EXPECT_NE(net::EntityModelTerm(3, 42), net::EntityModelTerm(3, 43));
}
