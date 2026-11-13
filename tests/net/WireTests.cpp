// docs/design/networking.md stage 3: the bytes of the protocol. Events (world operations and
// entity spawns), masks as run lengths, RegionDesc with raw float bits, messages; every decoder
// refuses truncated, oversized and trailing input, and random bytes never crash it.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <bit>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <variant>
#include <vector>

import vortex.core;
import vortex.voxel;
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
    welcome.logEnd = 77;
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
        EXPECT_EQ(w.logEnd, 77u);
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

        // Snapshots: header 21, owner 27, 16 per other player; positions bit for bit.
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
        snap.others = {{2, glm::vec3(1.0f, 2.0f, 3.0f), 1234, 1}, {9, glm::vec3(-4.0f, 5.0f, -6.0f), 65535, 2}};
        const std::vector<uint8_t> sb = net::Encode(snap);
        EXPECT_EQ(sb.size(), net::kSnapshotHeaderBytes + net::kSnapshotOwnerBytes + 2 * net::kSnapshotPlayerBytes);
        net::Snapshot sback;
        ASSERT_TRUE(net::Decode(sb, sback));
        EXPECT_EQ(sback.tick, 120u);
        EXPECT_EQ(sback.lastOpSeq, 4567u);
        EXPECT_EQ(sback.ackInputTick, 125u);
        EXPECT_EQ(sback.marginTick, 131u);
        EXPECT_EQ(sback.inputMargin, -3);
        ASSERT_TRUE(sback.hasOwner);
        EXPECT_EQ(sback.owner, snap.owner);
        EXPECT_EQ(sback.others, snap.others);
        EXPECT_FALSE(net::Decode(std::span(sb).first(sb.size() - 1), sback));
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
    const std::vector<std::vector<uint8_t>> valid = {net::Encode(batch), net::Encode(net::Welcome{}), net::Encode(net::Hello{}),
                                                     net::Encode(inputs), net::Encode(snap)};
    uint32_t decoded = 0;
    for (int i = 0; i < 4000; ++i) {
        std::vector<uint8_t> bytes;
        if (i % 2 == 0) {
            bytes.resize(rng.NextBounded(200));
            for (uint8_t& b : bytes) b = static_cast<uint8_t>(rng.NextU32());
            if (!bytes.empty()) bytes[0] = static_cast<uint8_t>(1 + rng.NextBounded(7));
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
