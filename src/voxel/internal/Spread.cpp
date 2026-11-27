module;

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cstdint>

module vortex.voxel;

namespace vortex::voxel {

    namespace {
        /// An ordered (Bayer) 4 x 4 pattern: rows are (a, c) pairs, columns y & 3.
        constexpr uint8_t kBayer[4][4] = {{0, 8, 2, 10}, {12, 4, 14, 6}, {3, 11, 1, 9}, {15, 7, 13, 5}};

        constexpr int kRow = kSpreadBoxEdge;                       // +1 in y
        constexpr int kSlab = kSpreadBoxEdge * kSpreadBoxEdge;     // +1 in z

        void SetClass(SpreadTable& t, SpreadClass c, const SpreadClassDef& def) {
            t.classes[c] = def;
            // Burning stages: k_1..k_K then b_1..b_B, consecutive ids each run.
            const auto stageId = [&def](uint32_t stage) {
                return stage <= def.K ? static_cast<uint32_t>(def.firstKindle) + stage - 1 : static_cast<uint32_t>(def.firstBurn) + (stage - def.K) - 1;
            };
            const uint32_t last = static_cast<uint32_t>(def.K) + def.B;
            for (uint32_t stage = 1; stage <= last; ++stage) {
                const uint32_t id = stageId(stage);
                if (id == 0 || id > 255) continue;
                t.kind[id] = SpreadKind::Burning;
                t.cls[id] = c;
                t.stage[id] = static_cast<uint8_t>(stage);
                t.next[id] = static_cast<uint8_t>(stage == last ? def.residue : stageId(stage + 1));
            }
        }
    }

    SpreadTable BuildSpreadTable(const WorldMaterials& ids) {
        SpreadTable t;
        for (uint32_t id = 0; id < 256; ++id) t.next[id] = static_cast<uint8_t>(id);
        t.bedrock = ids.bedrock;
        t.air = 0;
        t.charred = ids.charredWood;
        //                 K  B  pace  below reach slope side diag jump
        SpreadClassDef planks{6, 6, 10, 2, 4, 3, 3, 4, 10};
        planks.baseId = ids.planks;
        planks.firstKindle = ids.planksK1;
        planks.firstBurn = ids.planksB1;
        planks.residue = 0;
        planks.quenchEarly = ids.planks;
        planks.quenchLate = ids.charredWood;
        SpreadClassDef wood{8, 8, 10, 3, 5, 5, 6, 8, 12};
        wood.baseId = ids.wood;
        wood.firstKindle = ids.woodK1;
        wood.firstBurn = ids.woodB1;
        wood.residue = ids.charredWood;
        wood.quenchEarly = ids.wood;
        wood.quenchLate = ids.charredWood;
        SpreadClassDef foliage{3, 2, 3, 1, 2, 2, 0, 0, 0};
        foliage.baseId = ids.leaves;
        foliage.firstKindle = ids.foliageK1;
        foliage.firstBurn = ids.foliageB1;
        // Pitch (magic.md "New world-palette ids"): laid at b1, four burn stages, never lit by a neighbour; water puts it
        // out. As a source it lights timber by the target's delays like any burning voxel.
        SpreadClassDef pitch{0, 4, 10, 0, 0, 0, 0, 0, 0};
        pitch.firstBurn = ids.pitch;
        if (ids.planksK1 != 0) SetClass(t, Class_Planks, planks);
        if (ids.woodK1 != 0) SetClass(t, Class_Wood, wood);
        if (ids.foliageK1 != 0) SetClass(t, Class_Foliage, foliage);
        if (ids.pitch != 0) SetClass(t, Class_Pitch, pitch);
        const auto flammable = [&t](uint8_t id, SpreadClass c) {
            if (id == 0 || t.classes[c].firstKindle == 0) return;
            t.kind[id] = SpreadKind::Flammable;
            t.cls[id] = c;
        };
        flammable(ids.planks, Class_Planks);
        flammable(ids.wood, Class_Wood);
        flammable(ids.leaves, Class_Foliage);
        flammable(ids.leavesAutumn, Class_Foliage);
        if (ids.grass != 0) t.kind[ids.grass] = SpreadKind::GroundCover;
        if (ids.grassDry != 0) t.kind[ids.grassDry] = SpreadKind::GroundCover;
        if (ids.water != 0) t.kind[ids.water] = SpreadKind::WetSource;
        for (int a = 0; a < 2; ++a)
            for (int y = 0; y < 4; ++y)
                for (int c = 0; c < 2; ++c) t.kPhase[a][y][c] = kBayer[a * 2 + c][y];
        return t;
    }

    const SpreadTable& WorldSpreadTable() {
        static const SpreadTable table = [] {
            MaterialLibrary scratch;
            return BuildSpreadTable(RegisterDefaultWorldMaterials(scratch));
        }();
        return table;
    }

    uint64_t SpreadTableHash(const SpreadTable& t) {
        uint64_t h = HashMix64(0x53505244ull, 1);   // "SPRD"
        for (const SpreadKind k : t.kind) h = HashMix64(h, static_cast<uint8_t>(k));
        for (const uint8_t v : t.cls) h = HashMix64(h, v);
        for (const uint8_t v : t.stage) h = HashMix64(h, v);
        for (const uint8_t v : t.next) h = HashMix64(h, v);
        for (const SpreadClassDef& c : t.classes) {
            for (const uint8_t v : {c.K, c.B, c.pace, c.dBelow, c.dReach, c.dSlope, c.dSide, c.dDiag, c.dJump, c.baseId, c.firstKindle, c.firstBurn,
                                    c.residue, c.quenchEarly, c.quenchLate})
                h = HashMix64(h, v);
        }
        for (const auto& plane : t.kPhase)
            for (const auto& row : plane)
                for (const uint8_t v : row) h = HashMix64(h, v);
        h = HashMix64(h, t.bedrock);
        h = HashMix64(h, t.air);
        h = HashMix64(h, t.charred);
        return h;
    }

    uint8_t SpreadPhase(const SpreadTable& table, const glm::ivec3& v) {
        const int x = v.x & 3, y = v.y & 3, z = v.z & 3;
        return table.kPhase[std::min(x, 3 - x)][y][std::min(z, 3 - z)];
    }

    void StepBox(const SpreadTable& t, const SpreadBox& in, const glm::ivec3& chunk, uint32_t s, const BlockMask& evalBlocks, uint8_t* out,
                 SpreadCounts& counts) {
        (void)chunk;   // a chunk's origin is a multiple of 4 on every axis: the phase depends on the local voxel only
        const uint8_t* b = in.ids.data();
        // The phase of each of a block's 64 voxels (Chunk::LocalIndex order).
        std::array<uint8_t, 64> phase{};
        for (int l = 0; l < 64; ++l) phase[l] = SpreadPhase(t, glm::ivec3(l & 3, (l >> 2) & 3, l >> 4));
        const auto burningAtLeast = [&t, b](uint32_t i, uint8_t delay) {
            const uint8_t id = b[i];
            return t.kind[id] == SpreadKind::Burning && t.stage[id] >= delay;
        };
        const auto wet = [&t, b](uint32_t i) {
            return t.kind[b[i - 1]] == SpreadKind::WetSource || t.kind[b[i + 1]] == SpreadKind::WetSource ||
                   t.kind[b[i - kRow]] == SpreadKind::WetSource || t.kind[b[i + kRow]] == SpreadKind::WetSource ||
                   t.kind[b[i - kSlab]] == SpreadKind::WetSource || t.kind[b[i + kSlab]] == SpreadKind::WetSource;
        };
        for (uint32_t block = 0; block < static_cast<uint32_t>(kBlocksPerChunk); ++block) {
            if (((evalBlocks[block >> 6] >> (block & 63u)) & 1u) == 0) continue;
            const int bx = static_cast<int>(block & 7u) * 4, by = static_cast<int>((block >> 3) & 7u) * 4, bz = static_cast<int>(block >> 6) * 4;
            for (uint32_t l = 0; l < 64; ++l) {
                ++counts.evaluated;
                const uint32_t i = SpreadBox::Index(bx + static_cast<int>(l & 3u), by + static_cast<int>((l >> 2) & 3u), bz + static_cast<int>(l >> 4));
                const uint8_t o = b[i];
                const SpreadKind kind = t.kind[o];
                uint8_t n = o;
                if (kind == SpreadKind::Flammable) {
                    const SpreadClassDef& c = t.classes[t.cls[o]];
                    if (wet(i)) continue;
                    // Sources lie on the target's layer or below it, never above (fire never goes down).
                    bool lit = c.dBelow != 0 && burningAtLeast(i - kRow, c.dBelow);
                    if (!lit && c.dReach != 0 && b[i - kRow] == t.air) lit = burningAtLeast(i - 2 * kRow, c.dReach);
                    if (!lit && c.dSlope != 0)
                        lit = burningAtLeast(i - kRow + 1, c.dSlope) || burningAtLeast(i - kRow - 1, c.dSlope) ||
                              burningAtLeast(i - kRow + kSlab, c.dSlope) || burningAtLeast(i - kRow - kSlab, c.dSlope);
                    if (!lit && c.dSide != 0)
                        lit = burningAtLeast(i + 1, c.dSide) || burningAtLeast(i - 1, c.dSide) || burningAtLeast(i + kSlab, c.dSide) ||
                              burningAtLeast(i - kSlab, c.dSide);
                    if (!lit && c.dDiag != 0)
                        lit = burningAtLeast(i + 1 + kSlab, c.dDiag) || burningAtLeast(i + 1 - kSlab, c.dDiag) ||
                              burningAtLeast(i - 1 + kSlab, c.dDiag) || burningAtLeast(i - 1 - kSlab, c.dDiag);
                    if (!lit && c.dJump != 0)
                        lit = (b[i + 1] == t.air && burningAtLeast(i + 2, c.dJump)) || (b[i - 1] == t.air && burningAtLeast(i - 2, c.dJump)) ||
                              (b[i + kSlab] == t.air && burningAtLeast(i + 2 * kSlab, c.dJump)) ||
                              (b[i - kSlab] == t.air && burningAtLeast(i - 2 * kSlab, c.dJump));
                    if (!lit) continue;
                    n = c.firstKindle;
                    ++counts.ignited;
                    counts.lowestIgnited = std::min(counts.lowestIgnited, by + static_cast<int>((l >> 2) & 3u));
                } else if (kind == SpreadKind::Burning) {
                    const uint8_t cls = t.cls[o];
                    const SpreadClassDef& c = t.classes[cls];
                    const uint8_t stage = t.stage[o];
                    if (wet(i)) {
                        n = stage <= c.K ? c.quenchEarly : c.quenchLate;
                        ++counts.quenched;
                    } else if (stage <= c.K || (s + phase[l]) % c.pace == 0u) {
                        n = t.next[o];
                        ++counts.advanced;
                        if (t.kind[n] != SpreadKind::Burning) ++counts.burnedOut;
                    } else {
                        continue;
                    }
                    if (n != o) {
                        const bool lastStage = t.kind[n] == SpreadKind::Burning && t.stage[n] == t.classes[t.cls[n]].K + t.classes[t.cls[n]].B;
                        // A film (pitch) holds nothing up: its burnout is no site.
                        if ((n == 0 && cls != Class_Pitch) || n == t.charred || (lastStage && cls != Class_Pitch)) counts.site = true;
                    }
                } else {
                    continue;
                }
                if (n == o) continue;
                if ((o != 0) != (n != 0)) counts.shapeChanged = true;
                out[block * 64u + l] = n;
            }
        }
    }

    uint32_t LiveBlocks(const SpreadTable& table, const Chunk& chunk, BlockMask& out) {
        out.fill(0);
        uint32_t burning = 0;
        for (uint32_t block = 0; block < static_cast<uint32_t>(kBlocksPerChunk); ++block) {
            // Burning ids are solid: an empty block holds none.
            if (chunk.occ.voxelMask[block] == 0) continue;
            const uint8_t* ids = chunk.ids + static_cast<size_t>(block) * 64u;
            uint32_t n = 0;
            for (uint32_t l = 0; l < 64; ++l) n += table.kind[ids[l]] == SpreadKind::Burning ? 1u : 0u;
            if (n == 0) continue;
            burning += n;
            out[block >> 6] |= uint64_t{1} << (block & 63u);
        }
        return burning;
    }

    BlockMask DilateBlocks(const BlockMask& mask) {
        BlockMask out{};
        for (uint32_t w = 0; w < 8; ++w) {
            uint64_t bits = mask[w];
            while (bits != 0) {
                const uint32_t block = w * 64u + static_cast<uint32_t>(std::countr_zero(bits));
                bits &= bits - 1;
                const int x = static_cast<int>(block & 7u), y = static_cast<int>((block >> 3) & 7u), z = static_cast<int>(block >> 6);
                for (int dz = -1; dz <= 1; ++dz)
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) {
                            const int nx = x + dx, ny = y + dy, nz = z + dz;
                            if (nx < 0 || ny < 0 || nz < 0 || nx > 7 || ny > 7 || nz > 7) continue;
                            const uint32_t nb = static_cast<uint32_t>(nx + ny * 8 + nz * 64);
                            out[nb >> 6] |= uint64_t{1} << (nb & 63u);
                        }
            }
        }
        return out;
    }
}
