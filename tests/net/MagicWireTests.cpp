// docs/design/magic.md "Wire values" ([mag-2], protocol 14): the magic ops (MapShape 7, MapMask 8, CarveCapsule 9), the zone events
// (ZoneStart 38, ZoneCancel 39), CastFx 40, the input's spell byte and the Spell edge, the owner block's magic state,
// the remote record's status bits, the roster's elements, Attune, Welcome's zone lead and push decays, BaselineEnd's
// zones, the spell rejections and the build hash's magic tables. Every value round-trips at its designed size and
// every decoder refuses its limits, a byte too few and a byte too many.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <variant>
#include <vector>

import vortex.core;
import vortex.voxel;
import vortex.sim;
import vortex.net;

using namespace vortex;

namespace {
    std::vector<uint8_t> EventBytesOf(const net::Event& e) {
        net::ByteWriter w;
        net::WriteEvent(w, e);
        return w.Take();
    }

    /// One event read from `bytes`, which it must use up.
    bool DecodesWhole(const std::vector<uint8_t>& bytes, net::Event& out, uint64_t* budget = nullptr) {
        net::ByteReader r(bytes);
        return net::ReadEvent(r, out, budget) && r.AtEnd();
    }

    bool Decodes(const net::Event& e) {
        net::Event out;
        return DecodesWhole(EventBytesOf(e), out);
    }

    /// A byte too few never decodes; inside a batch, a byte too many is refused (the batch must end with its events).
    void RefusesShortAndLong(const net::Event& e) {
        const std::vector<uint8_t> bytes = EventBytesOf(e);
        net::Event out;
        net::ByteReader shorter(std::span(bytes).first(bytes.size() - 1));
        EXPECT_FALSE(net::ReadEvent(shorter, out));
        std::vector<uint8_t> batch = net::EncodeBatch(1, 2, 1, bytes);
        net::EventBatch b;
        EXPECT_TRUE(net::Decode(batch, b));
        batch.push_back(0);
        EXPECT_FALSE(net::Decode(batch, b));
    }

    voxel::ShapeQ TestShape(uint8_t kind) {
        voxel::ShapeQ s;
        s.kind = kind;
        switch (kind) {
            case voxel::kShapeSphere:
                s.a = {-100, 2000, 3000};
                s.radiusQ = 100;
                break;
            case voxel::kShapeCapsule:
                s.a = {-100, 2000, 3000};
                s.b = {500, 1980, 3040};
                s.radiusQ = 24;
                break;
            case voxel::kShapeBox:
                s.a = {-3, 4, 5};
                s.b = {10, 12, 9};
                break;
            default:
                s.a = {16, 1600, -48};
                s.radiusQ = 16;
                s.halfHeightQ = 160;
                break;
        }
        return s;
    }

    /// A 13 x 7 x 5 mask whose runs differ by axis.
    voxel::VoxelMask TestMask() {
        voxel::VoxelMask m({13, 7, 5});
        for (int z = 0; z < 5; ++z)
            for (int y = 0; y < 7; ++y)
                for (int x = 0; x < 13; ++x)
                    if ((x < 9 && y > 1) || (z == 2 && x % 3 == 0) || (y == 6 && z < 3)) m.Set(x, y, z, true);
        return m;
    }

    sim::Zone TestZone(uint8_t shapeKind, uint16_t id = 7) {
        sim::Zone z;
        z.zoneId = id;
        z.kind = sim::ZoneKind::Lift;
        z.teamMask = 0x06;
        z.shape = TestShape(shapeKind);
        z.startTick = 4000000000u;
        z.duration = 90;
        z.strength[0] = 900;
        z.strength[1] = -300;
        return z;
    }
}

// --- the ops ---------------------------------------------------------------------------------------------------

TEST(Wire, MagicOpsRoundTrip) {
    constexpr size_t kShapeBytes[4] = {17, 29, 27, 19};
    for (uint8_t kind = 0; kind < 4; ++kind) {
        const sim::WorldOp op = sim::WorldOp::Map(voxel::Map_Knit, TestShape(kind));
        const std::vector<uint8_t> bytes = EventBytesOf(op);
        EXPECT_EQ(bytes.size(), kShapeBytes[kind]) << int(kind);
        EXPECT_EQ(bytes[0], 7u);
        net::Event back;
        ASSERT_TRUE(DecodesWhole(bytes, back)) << int(kind);
        EXPECT_EQ(std::get<sim::WorldOp>(back), op) << int(kind);
        RefusesShortAndLong(op);
    }
    const voxel::VoxelMask mask = TestMask();
    for (uint8_t axis = 0; axis < 3; ++axis) {
        sim::WorldOp op = sim::WorldOp::MapMasked(voxel::Map_Unknit, {-40, 90, 7}, mask);
        op.axis = axis;
        const std::vector<uint8_t> bytes = EventBytesOf(op);
        EXPECT_EQ(bytes.size(), 21u + voxel::MaskRunBytes(mask, axis)) << int(axis);
        EXPECT_EQ(bytes[0], 8u);
        net::Event back;
        ASSERT_TRUE(DecodesWhole(bytes, back)) << int(axis);
        EXPECT_EQ(std::get<sim::WorldOp>(back), op) << int(axis);
        RefusesShortAndLong(op);
    }
}

TEST(Wire, MagicOpsRefuseLimitsAndTrailingBytes) {
    sim::WorldOp op = sim::WorldOp::Map(voxel::Map_Knit, TestShape(voxel::kShapeSphere));
    for (const uint8_t map : {uint8_t{0}, uint8_t{16}}) {
        sim::WorldOp bad = op;
        bad.map = map;
        EXPECT_FALSE(Decodes(bad)) << "map " << int(map);
    }
    op.shape.radiusQ = sim::kMaxCastRadiusQ;
    EXPECT_TRUE(Decodes(op));
    op.shape.radiusQ = sim::kMaxCastRadiusQ + 1;
    EXPECT_FALSE(Decodes(op)) << "a sphere over the cast radius";
    sim::WorldOp cylinder = sim::WorldOp::Map(voxel::Map_Rime, TestShape(voxel::kShapeCylinder));
    cylinder.shape.radiusQ = sim::kMaxCastRadiusQ + 1;
    EXPECT_FALSE(Decodes(cylinder)) << "a cylinder over the cast radius";

    // A capsule: 64 m (8192 q at 0.125 m) long at most, its radius 1024 q at most.
    sim::WorldOp capsule = sim::WorldOp::Map(voxel::Map_Knit, TestShape(voxel::kShapeCapsule));
    capsule.shape.b = capsule.shape.a + glm::ivec3(net::kMaxCapsuleLengthQ, 0, 0);
    EXPECT_TRUE(Decodes(capsule));
    capsule.shape.b = capsule.shape.a + glm::ivec3(net::kMaxCapsuleLengthQ + 128, 0, 0);
    EXPECT_FALSE(Decodes(capsule)) << "a capsule of 65 m";
    capsule.shape.b = capsule.shape.a + glm::ivec3(6000, 6000, 0);   // under the limit per axis, over it in length
    EXPECT_FALSE(Decodes(capsule));
    capsule.shape.b = capsule.shape.a;
    capsule.shape.radiusQ = net::kMaxCapsuleRadiusQ + 1;
    EXPECT_FALSE(Decodes(capsule)) << "a capsule radius over 1024 q";

    sim::WorldOp box = sim::WorldOp::Map(voxel::Map_Knit, TestShape(voxel::kShapeBox));
    box.shape.b.x = net::kMaxSyncChunkCoordinate * voxel::kChunkSize + 1;
    EXPECT_FALSE(Decodes(box)) << "a box corner past the chunk coordinates";
    sim::WorldOp shape4 = op;
    shape4.shape.radiusQ = 10;
    std::vector<uint8_t> bytes = EventBytesOf(shape4);
    bytes[2] = 4;                                                       // shape kind 4
    net::Event out;
    EXPECT_FALSE(DecodesWhole(bytes, out));

    // MapMask: axis 3, map 0, and a mask over the batch's voxel budget.
    sim::WorldOp masked = sim::WorldOp::MapMasked(voxel::Map_Knit, {0, 0, 0}, TestMask());
    bytes = EventBytesOf(masked);
    bytes[2] = 3;
    EXPECT_FALSE(DecodesWhole(bytes, out)) << "axis 3";
    bytes = EventBytesOf(masked);
    bytes[1] = 0;
    EXPECT_FALSE(DecodesWhole(bytes, out)) << "map 0";
    bytes = EventBytesOf(masked);
    uint64_t budget = 13 * 7 * 5 - 1;
    EXPECT_FALSE(DecodesWhole(bytes, out, &budget)) << "the mask is charged before it is read";
    budget = 13 * 7 * 5;
    EXPECT_TRUE(DecodesWhole(bytes, out, &budget));
    EXPECT_EQ(budget, 0u);

}

// CarveCapsule (9): kind, A and B (i32 x 3 each), radius u16, power u16 = 29 B; a capsule over 64 m or with a radius over
// 1024 q is refused, as are a byte too few and a byte too many.
TEST(Wire, CarveCapsuleRoundTripsAndRefusesItsLimits) {
    const sim::WorldOp bore = sim::WorldOp::Bore(TestShape(voxel::kShapeCapsule), 64);
    const std::vector<uint8_t> bytes = EventBytesOf(bore);
    ASSERT_EQ(bytes.size(), 29u);
    EXPECT_EQ(bytes[0], 9);
    EXPECT_EQ(sim::OpWireBytes(bore), 29u);
    net::Event out;
    ASSERT_TRUE(DecodesWhole(bytes, out));
    ASSERT_TRUE(std::holds_alternative<sim::WorldOp>(out));
    EXPECT_EQ(std::get<sim::WorldOp>(out), bore);
    RefusesShortAndLong(bore);

    sim::WorldOp wide = bore;
    wide.shape.radiusQ = 1025;
    EXPECT_FALSE(Decodes(wide)) << "radius over 1024 q";
    wide.shape.radiusQ = 1024;
    EXPECT_TRUE(Decodes(wide));
    sim::WorldOp longer = bore;
    longer.shape.b = longer.shape.a + glm::ivec3(8193, 0, 0);
    EXPECT_FALSE(Decodes(longer)) << "over 64 m on an axis";
    longer.shape.b = longer.shape.a + glm::ivec3(6000, 6000, 0);
    EXPECT_FALSE(Decodes(longer)) << "over 64 m along the diagonal";
    longer.shape.b = longer.shape.a + glm::ivec3(8192, 0, 0);
    EXPECT_TRUE(Decodes(longer));
}

TEST(Wire, OpWireBytesEqualsEventBytes) {
    std::vector<sim::WorldOp> ops;
    for (uint8_t kind = 0; kind < 4; ++kind) ops.push_back(sim::WorldOp::Map(voxel::Map_Knit, TestShape(kind)));
    ops.push_back(sim::WorldOp::MapMasked(voxel::Map_Knit, {3, 4, 5}, TestMask()));
    for (uint8_t axis = 0; axis < 3; ++axis) {
        sim::WorldOp m = ops.back();
        m.axis = axis;
        ops.push_back(m);
    }
    ops.push_back(sim::WorldOp::Carve({glm::ivec3(10, 20, 30), 40}, 96));
    ops.push_back(sim::WorldOp::Clear({1, 2, 3}, TestMask()));
    ops.push_back(sim::WorldOp::Bore(TestShape(voxel::kShapeCapsule), 64));
    for (const sim::WorldOp& op : ops) EXPECT_EQ(sim::OpWireBytes(op), net::EventBytes(op)) << int(op.kind) << " axis " << int(op.axis);
}

// The axis changes only the order the runs are read in: one mask sent in the three axes decodes to the same voxels.
TEST(MapMask, AxisOrderGivesTheSameVoxels) {
    const voxel::VoxelMask mask = TestMask();
    std::vector<std::vector<uint8_t>> encodings;
    for (uint8_t axis = 0; axis < 3; ++axis) {
        sim::WorldOp op = sim::WorldOp::MapMasked(voxel::Map_Knit, {-7, 3, 11}, mask);
        op.axis = axis;
        encodings.push_back(EventBytesOf(op));
        net::Event back;
        ASSERT_TRUE(DecodesWhole(encodings.back(), back));
        const sim::WorldOp& decoded = std::get<sim::WorldOp>(back);
        EXPECT_EQ(decoded.mask, mask) << int(axis);
        EXPECT_EQ(decoded.min, glm::ivec3(-7, 3, 11));
    }
    EXPECT_NE(encodings[0], encodings[1]);
    EXPECT_NE(encodings[0], encodings[2]);
}

// --- the events ------------------------------------------------------------------------------------------------

TEST(Wire, ZoneEventsAndCastFxRoundTrip) {
    constexpr size_t kZoneBytes[4] = {30, 42, 40, 32};
    for (uint8_t kind = 0; kind < 4; ++kind) {
        const net::ZoneStartEvent e{TestZone(kind)};
        const std::vector<uint8_t> bytes = EventBytesOf(e);
        EXPECT_EQ(bytes.size(), kZoneBytes[kind]) << int(kind);
        EXPECT_EQ(bytes[0], 38u);
        net::Event back;
        ASSERT_TRUE(DecodesWhole(bytes, back));
        EXPECT_EQ(std::get<net::ZoneStartEvent>(back), e);
        RefusesShortAndLong(e);
    }
    for (uint8_t zoneKind = 1; zoneKind <= net::kMaxZoneKind; ++zoneKind) {
        net::ZoneStartEvent e{TestZone(voxel::kShapeCapsule)};
        e.zone.kind = static_cast<sim::ZoneKind>(zoneKind);
        EXPECT_TRUE(Decodes(e)) << "zone kind " << int(zoneKind) << " is reserved in this protocol";
    }
    const net::ZoneCancelEvent cancel{65535, 4000000123u};
    std::vector<uint8_t> bytes = EventBytesOf(cancel);
    EXPECT_EQ(bytes.size(), 7u);
    EXPECT_EQ(bytes[0], 39u);
    net::Event back;
    ASSERT_TRUE(DecodesWhole(bytes, back));
    EXPECT_EQ(std::get<net::ZoneCancelEvent>(back), cancel);
    RefusesShortAndLong(cancel);

    const net::CastFx fx{4, sim::kDash, {-1, 2000000, 3}, {-160, 1600, 48}, 5, 123456, 0, {}};
    bytes = EventBytesOf(fx);
    EXPECT_EQ(bytes.size(), 34u);   // [mag-8]: + the flight and the point count
    EXPECT_EQ(bytes[0], 40u);
    ASSERT_TRUE(DecodesWhole(bytes, back));
    EXPECT_EQ(std::get<net::CastFx>(back), fx);
    RefusesShortAndLong(fx);
    for (uint8_t spell = 0; spell <= net::kMaxSpellId; ++spell) EXPECT_TRUE(Decodes(net::CastFx{1, spell, {}, {}, 0, 0, 0, {}})) << int(spell);
    // [mag-8] A drawn shape's CastFx: its flight and up to 16 points.
    net::CastFx drawn = fx;
    drawn.flightTicks = net::CastFx::kMaxFlightTicks;
    for (int i = 0; i < 16; ++i) drawn.pointsQ.push_back({i * 64, -5, 1 << 20});
    bytes = EventBytesOf(drawn);
    EXPECT_EQ(bytes.size(), 34u + 16u * 12u);
    ASSERT_TRUE(DecodesWhole(bytes, back));
    EXPECT_EQ(std::get<net::CastFx>(back), drawn);
    RefusesShortAndLong(drawn);
    net::CastFx late = fx;
    late.flightTicks = net::CastFx::kMaxFlightTicks + 1;
    EXPECT_FALSE(Decodes(late)) << "a flight over 60 ticks";
    bytes = EventBytesOf(drawn);
    bytes[33] = 17;   // the point count
    EXPECT_FALSE(DecodesWhole(bytes, back)) << "17 points";
}

TEST(Wire, ZoneStartRefusesKind0And10AndALongerDurationThanARune) {
    for (const uint8_t kind : {uint8_t{0}, uint8_t{10}}) {
        net::ZoneStartEvent e{TestZone(voxel::kShapeSphere)};
        e.zone.kind = static_cast<sim::ZoneKind>(kind);
        EXPECT_FALSE(Decodes(e)) << "zone kind " << int(kind);
    }
    for (const uint16_t duration : {uint16_t{0}, uint16_t{3601}}) {
        net::ZoneStartEvent e{TestZone(voxel::kShapeSphere)};
        e.zone.duration = duration;
        EXPECT_FALSE(Decodes(e)) << "duration " << duration;
    }
    net::ZoneStartEvent longest{TestZone(voxel::kShapeSphere)};
    longest.zone.duration = net::kMaxZoneDuration;
    EXPECT_TRUE(Decodes(longest));
    net::ZoneStartEvent wide{TestZone(voxel::kShapeCylinder)};
    wide.zone.shape.radiusQ = sim::kMaxCastRadiusQ + 1;
    EXPECT_FALSE(Decodes(wide)) << "the shape's limits are an op's";
    EXPECT_FALSE(Decodes(net::CastFx{1, static_cast<uint8_t>(net::kMaxSpellId + 1), {}, {}, 0, 0})) << "spell 18";
    EXPECT_FALSE(Decodes(net::CastFx{1, 0, {}, {}, 6, 0})) << "face 6";
}

// --- the input and the owner block ------------------------------------------------------------------------------

TEST(Wire, InputCarriesSpellAndRefusesUnknownButtons) {
    EXPECT_EQ(sim::PlayerButton_Spell, 32);
    EXPECT_EQ(sim::kPlayerButtonMask, 63);
    net::InputBatch batch;
    batch.firstTick = 900;
    sim::PlayerInput in;
    in.moveX = -127;
    in.buttons = static_cast<uint8_t>(sim::PlayerButton_Spell | sim::PlayerButton_Run);
    in.spell = sim::kPunch;
    batch.inputs = {in, in};
    batch.inputs[1].spell = 17;
    const std::vector<uint8_t> bytes = net::Encode(batch);
    EXPECT_EQ(bytes.size(), net::kInputBatchHeaderBytes + 2 * net::kInputBytes);
    EXPECT_EQ(net::kInputBytes, 8u);
    net::InputBatch back;
    ASSERT_TRUE(net::Decode(bytes, back));
    ASSERT_EQ(back.inputs.size(), 2u);
    EXPECT_EQ(back.inputs[0].spell, sim::kPunch);
    EXPECT_EQ(back.inputs[1].spell, 17u) << "a spell over 15 is the server's to refuse (NotAttuned), never the decoder's";
    EXPECT_TRUE(back.inputs[0].SameControls(in));
    sim::PlayerInput other = in;
    other.spell = sim::kDash;
    EXPECT_FALSE(other.SameControls(in));
    EXPECT_FALSE(net::Decode(std::span(bytes).first(bytes.size() - 1), back)) << "the last input's spell byte missing";
    std::vector<uint8_t> longer = bytes;
    longer.push_back(0);
    EXPECT_FALSE(net::Decode(longer, back)) << "a byte too many";
    net::InputBatch unknown = batch;
    unknown.inputs[1].buttons = static_cast<uint8_t>(in.buttons | 64);
    EXPECT_FALSE(net::Decode(net::Encode(unknown), back)) << "button 64";
}

TEST(Wire, SnapshotOwnerBlockCarriesMagicState) {
    net::Snapshot s;
    s.tick = 77;
    s.hasOwner = true;
    sim::PlayerState& o = s.owner;
    o.position = {1.0f, 2.0f, 3.0f};
    o.push[0] = 1500;
    o.push[1] = -1500;
    o.push[2] = 7;
    o.status = 0x3F;
    o.slowQ = 16;
    o.slowLeft = 255;
    o.rootLeft = 1;
    o.pinLeft = 2;
    o.ccGuardLeft = 90;
    o.dashLeft = 18;
    o.dashDir[0] = 127;
    o.dashDir[1] = -127;
    s.mana = 5999;
    s.spellGap = 12;
    s.attune = 0x1E;
    net::RemotePlayer other;
    other.slot = 2;
    other.statusBits = 0x3F;
    s.others.push_back(other);
    const std::vector<uint8_t> bytes = net::Encode(s);
    net::Snapshot back;
    ASSERT_TRUE(net::Decode(bytes, back));
    EXPECT_EQ(back.owner, s.owner);
    EXPECT_EQ(back.mana, 5999);
    EXPECT_EQ(back.spellGap, 12);
    EXPECT_EQ(back.attune, 0x1E);
    ASSERT_EQ(back.others.size(), 1u);
    EXPECT_EQ(back.others[0], other);

    // The magic block is the owner block's last 19 bytes: push at +0, the counters at +6, mana at +15, the gap at +17,
    // attune at +18.
    const size_t magicAt = net::kSnapshotHeaderBytes - 2 + 44;
    const auto refused = [&](size_t at, uint8_t value) {
        std::vector<uint8_t> bad = bytes;
        bad[at] = value;
        net::Snapshot b;
        return !net::Decode(bad, b);
    };
    {
        std::vector<uint8_t> bad = bytes;
        bad[magicAt] = 0xDD;
        bad[magicAt + 1] = 0x05;
        net::Snapshot b;
        EXPECT_FALSE(net::Decode(bad, b)) << "push 1501";
    }
    EXPECT_TRUE(refused(magicAt + 18, 0x20)) << "attune bit 5";
    EXPECT_FALSE(refused(magicAt + 18, 0x1F));
    EXPECT_FALSE(refused(magicAt + 17, 255)) << "any gap";
    net::Snapshot overfull = s;
    overfull.mana = net::Snapshot::kMaxMana + 1;
    EXPECT_FALSE(net::Decode(net::Encode(overfull), back)) << "mana over the pool";
    net::Snapshot retired = s;
    retired.castRejected = static_cast<net::CastRejection>(net::kRetiredCastRejection);
    EXPECT_FALSE(net::Decode(net::Encode(retired), back)) << "rejection 6 is retired";
    EXPECT_TRUE(refused(bytes.size() - 2, 0x40)) << "a remote status bit 6 (the last byte is the body count)";
    net::Snapshot cut = s;
    cut.others.clear();
    const std::vector<uint8_t> noOthers = net::Encode(cut);
    EXPECT_FALSE(net::Decode(std::span(noOthers).first(noOthers.size() - 1), back));
}

TEST(Wire, AttuneRoundTrip) {
    EXPECT_EQ(static_cast<uint8_t>(net::MessageType::Attune), 24u);
    const net::Attune a{3, 1};
    const std::vector<uint8_t> bytes = net::Encode(a);
    EXPECT_EQ(bytes.size(), 3u);
    net::Attune back;
    ASSERT_TRUE(net::Decode(bytes, back));
    EXPECT_EQ(back, a);
    EXPECT_FALSE(net::Decode(std::span(bytes).first(2), back));
    std::vector<uint8_t> longer = bytes;
    longer.push_back(0);
    EXPECT_FALSE(net::Decode(longer, back));
    EXPECT_FALSE(net::Decode(net::Encode(net::Attune{4, 0}), back)) << "element 4";
    EXPECT_TRUE(net::Decode(net::Encode(net::Attune{2, 2}), back)) << "two equal elements are the server's to refuse";
}

// [mag-8] SpellGesture 25: the input tick, the kind, the form (or AUTO) and up to 16 points of int32 x 3.
TEST(Wire, SpellGestureRoundTrip) {
    EXPECT_EQ(static_cast<uint8_t>(net::MessageType::SpellGesture), 25u);
    net::SpellGesture m;
    m.inputTick = 0xA1B2C3D4u;
    m.gesture.kind = sim::GestureKind::Rise;
    m.gesture.form = static_cast<uint8_t>(sim::Form::Burst);
    for (int i = 0; i < 16; ++i) m.gesture.pointsQ.push_back({-i * 100000, i, 1 << 24});
    const std::vector<uint8_t> bytes = net::Encode(m);
    EXPECT_EQ(bytes.size(), net::kSpellGestureHeaderBytes + 16u * 12u);
    net::SpellGesture back;
    ASSERT_TRUE(net::Decode(bytes, back));
    EXPECT_EQ(back, m);
    EXPECT_FALSE(net::Decode(std::span(bytes).first(bytes.size() - 1), back));
    std::vector<uint8_t> longer = bytes;
    longer.push_back(0);
    EXPECT_FALSE(net::Decode(longer, back));

    net::SpellGesture tap;
    const std::vector<uint8_t> none = net::Encode(tap);
    EXPECT_EQ(none.size(), net::kSpellGestureHeaderBytes);
    ASSERT_TRUE(net::Decode(none, back));
    EXPECT_EQ(back.gesture.form, sim::kAutoForm);
    EXPECT_TRUE(back.gesture.pointsQ.empty());
    const auto refused = [&](size_t at, uint8_t value) {
        std::vector<uint8_t> bad = none;
        bad[at] = value;
        return !net::Decode(bad, back);
    };
    EXPECT_TRUE(refused(5, 4)) << "kind 4";
    EXPECT_FALSE(refused(5, 3));
    EXPECT_TRUE(refused(6, 5)) << "form 5";
    EXPECT_FALSE(refused(6, 4)) << "AUTO";
    std::vector<uint8_t> seventeen = net::Encode(m);
    seventeen[7] = 17;
    for (int i = 0; i < 12; ++i) seventeen.push_back(0);
    EXPECT_FALSE(net::Decode(seventeen, back)) << "17 points";
}

TEST(Wire, RosterCarriesTheElements) {
    net::Roster r;
    r.players = {{1, 2, 3, 4, 0x8}, {3, 1, 0, 0, 0xF}};
    const std::vector<uint8_t> bytes = net::Encode(r);
    EXPECT_EQ(bytes.size(), 2u + 2u * 7u);
    net::Roster back;
    ASSERT_TRUE(net::Decode(bytes, back));
    EXPECT_EQ(back, r);
}

TEST(Wire, BaselineEndCarriesZones) {
    net::BaselineEnd none{12, 34, {}};
    std::vector<uint8_t> bytes = net::Encode(none);
    EXPECT_EQ(bytes.size(), 10u);
    net::BaselineEnd back;
    ASSERT_TRUE(net::Decode(bytes, back));
    EXPECT_TRUE(back.zones.empty());

    net::BaselineEnd full{12, 34, {}};
    for (uint16_t i = 0; i < 33; ++i) full.zones.push_back(TestZone(static_cast<uint8_t>(i % 4), static_cast<uint16_t>(i + 1)));
    bytes = net::Encode(full);                               // the encoder keeps 32
    ASSERT_TRUE(net::Decode(bytes, back));
    ASSERT_EQ(back.zones.size(), net::kMaxBaselineZones);
    for (size_t i = 0; i < back.zones.size(); ++i) EXPECT_EQ(back.zones[i], full.zones[i]) << i;
    EXPECT_FALSE(net::Decode(std::span(bytes).first(bytes.size() - 1), back));
    std::vector<uint8_t> longer = bytes;
    longer.push_back(0);
    EXPECT_FALSE(net::Decode(longer, back));
    // 33 zones are refused.
    std::vector<uint8_t> over = bytes;
    over[9] = 33;
    net::ByteWriter w;
    net::WriteZone(w, full.zones[32]);
    over.insert(over.end(), w.Data().begin(), w.Data().end());
    EXPECT_FALSE(net::Decode(over, back));
}

TEST(Wire, WelcomeCarriesTheZoneLeadAndThePushDecays) {
    net::Welcome w;
    w.region.terrain.voxelSize = 0.125f;
    w.player.pushDecayGround = 33;
    w.player.pushDecayAir = 2;
    w.zoneLeadTicks = 30;
    std::vector<uint8_t> bytes = net::Encode(w);
    net::Welcome back;
    ASSERT_TRUE(net::Decode(bytes, back));
    EXPECT_EQ(back.player, w.player);
    EXPECT_EQ(back.zoneLeadTicks, 30u);
    bytes.back() = net::Welcome::kMaxZoneLeadTicks + 1;
    EXPECT_FALSE(net::Decode(bytes, back)) << "a lead over 60 ticks";
    sim::PlayerTuning t;
    t.pushDecayGround = sim::kPushLimitCmS + 1;
    EXPECT_FALSE(sim::IsValid(t));
}

TEST(Wire, SpellRefusalsMapToRejections) {
    using R = net::CastRejection;
    using S = sim::SpellRefusal;
    EXPECT_EQ(net::kMaxCastRejection, 14u);
    EXPECT_EQ(net::RejectionOf(S::None), R::None);
    EXPECT_EQ(net::RejectionOf(S::NotBuilt), R::NotAttuned);
    EXPECT_EQ(net::RejectionOf(S::NotAttuned), R::NotAttuned);
    EXPECT_EQ(net::RejectionOf(S::TooSoon), R::TooSoon);
    EXPECT_EQ(net::RejectionOf(S::NoMana), R::NoMana);
    EXPECT_EQ(net::RejectionOf(S::BadGesture), R::BadGesture);
    EXPECT_EQ(net::RejectionOf(S::Missed), R::Missed);
    EXPECT_EQ(net::RejectionOf(S::Outside), R::Outside);
    EXPECT_EQ(net::RejectionOf(S::TooMuchConjured), R::TooMuchConjured);
    EXPECT_EQ(net::RejectionOf(S::TooManyZones), R::TooManyZones);
    EXPECT_EQ(net::RejectionOf(S::NearSpawn), R::NearSpawn);
    EXPECT_EQ(net::RejectionOf(S::Rooted), R::Missed);
    EXPECT_EQ(net::kRetiredCastRejection, 6u);
    EXPECT_EQ(static_cast<uint8_t>(R::NoMana), 12u);
    EXPECT_EQ(static_cast<uint8_t>(R::TooSoon), 13u);
    EXPECT_EQ(static_cast<uint8_t>(R::BadGesture), 14u);
    EXPECT_EQ(static_cast<uint8_t>(R::TooMuchFire), 10u);
    EXPECT_EQ(static_cast<uint8_t>(R::NearSpawn), 11u);
    EXPECT_STREQ(net::ToString(R::NoMana), "no mana");
    EXPECT_STREQ(net::ToString(R::BadGesture), "bad gesture");
    EXPECT_STREQ(net::ToString(R::NearSpawn), "near spawn");
}

// The build hash sees the spell table (operator parameters, zones, cooldowns, damage) and the Turning table: a build
// that would cast or map differently refuses the session at the Hello ("another build").
TEST(SessionFile, BuildHashSeesTheSpellAndTurningTables) {
    voxel::MaterialLibrary materials;
    voxel::RegisterDefaultWorldMaterials(materials);
    const uint64_t table = voxel::MaterialTableHash(materials);
    const sim::SpellTable spells = sim::BuildSpellTable(voxel::kDefaultVoxelSize);
    const voxel::TurningTable& turning = voxel::WorldTurningTable();
    const uint64_t spellHash = sim::SpellTableHash(spells), turningHash = voxel::TurningTableHash(turning);
    const auto hashOf = [&](uint64_t s, uint64_t t) {
        return net::BuildHashOf(net::kProtocolVersion, voxel::kGeneratorGoldenHash, table, sim::kPlayerRuleRevision, voxel::kLandGoldenHash, s, t);
    };
    EXPECT_EQ(net::BuildHash(materials), hashOf(spellHash, turningHash));
    sim::SpellTable changed = spells;
    ++changed.spells[sim::kDash].dashTicks;
    EXPECT_NE(sim::SpellTableHash(changed), spellHash);
    EXPECT_NE(hashOf(sim::SpellTableHash(changed), turningHash), net::BuildHash(materials));
    voxel::TurningTable mapped = turning;
    ++mapped.maps[voxel::Map_Knit][1];
    EXPECT_NE(voxel::TurningTableHash(mapped), turningHash);
    EXPECT_NE(hashOf(spellHash, voxel::TurningTableHash(mapped)), net::BuildHash(materials));
}
