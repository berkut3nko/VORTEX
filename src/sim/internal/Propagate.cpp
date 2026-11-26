module;

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <map>
#include <memory>
#include <queue>
#include <set>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

module vortex.sim;

import vortex.math;
import vortex.voxel;

namespace vortex::sim {

    OperatorWalls WindowWalls(const voxel::World& world) {
        OperatorWalls walls;
        walls.min = voxel::ChunkToVoxel(world.WindowMinChunk());
        walls.max = voxel::ChunkToVoxel(world.WindowMinChunk() + world.WindowSize());
        return walls;
    }

    VoxelCache::VoxelCache(const voxel::World& world, const OperatorWalls& walls) : m_World(world), m_Walls(walls) {}

    uint8_t VoxelCache::Get(const glm::ivec3& v) {
        if (glm::any(glm::lessThan(v, m_Walls.min)) || glm::any(glm::greaterThanEqual(v, m_Walls.max))) return kWallId;
        const glm::ivec3 c = voxel::VoxelToChunk(v);
        const auto key = std::make_tuple(c.x, c.y, c.z);
        auto it = m_Chunks.find(key);
        if (it == m_Chunks.end()) {
            std::unique_ptr<voxel::Chunk> chunk;
            if (m_World.IsChunkLoaded(c)) {
                chunk = std::make_unique<voxel::Chunk>();
                if (!m_World.ReadChunk(c, *chunk)) chunk.reset();
            }
            it = m_Chunks.emplace(key, std::move(chunk)).first;
        }
        if (!it->second) return kWallId;
        const glm::ivec3 local = v & glm::ivec3(voxel::kChunkMask);
        return it->second->Get(local.x, local.y, local.z);
    }

    FaceAxes FaceAxesOf(const glm::ivec3& normal) {
        FaceAxes axes;
        axes.n = normal;
        if (normal.x != 0) {
            axes.u = {0, 1, 0};
            axes.v = {0, 0, 1};
        } else if (normal.y != 0) {
            axes.u = {1, 0, 0};
            axes.v = {0, 0, 1};
        } else {
            axes.u = {1, 0, 0};
            axes.v = {0, 1, 0};
        }
        return axes;
    }

    namespace {
        int64_t Dot(const glm::ivec3& a, const glm::ivec3& b) {
            return static_cast<int64_t>(a.x) * b.x + static_cast<int64_t>(a.y) * b.y + static_cast<int64_t>(a.z) * b.z;
        }
        int64_t Abs64(int64_t v) { return v < 0 ? -v : v; }
    }

    BurstResult BurstStrip(const voxel::World& world, const OperatorWalls& walls, const BurstParams& params) {
        BurstResult out;
        VoxelCache cache(world, walls);
        const FaceAxes& ax = params.axes;
        const int64_t p = params.plane;
        const int64_t nMin = params.normalMin, nMax = params.normalMax;
        if (nMax < nMin) return out;

        int64_t du0 = Dot(params.aim, ax.u), dv0 = Dot(params.aim, ax.v);
        int64_t dp = static_cast<int64_t>(math::ISqrt64(static_cast<uint64_t>(du0 * du0 + dv0 * dv0)));
        if (dp == 0) {   // the aim along the normal: the strip lies across u
            du0 = 1;
            dv0 = 0;
            dp = 1;
        }
        const int64_t Du = du0, Dv = dv0, Dp = dp;
        const int64_t width = static_cast<int64_t>(params.halfWidth) * Dp;
        const int64_t thickness = static_cast<int64_t>(params.halfThickness) * Dp;

        struct Offsets {
            int64_t du, dv, dn;
        };
        const auto offsets = [&](const glm::ivec3& c) {
            const glm::ivec3 d = c - params.seed;
            return Offsets{Dot(d, ax.u), Dot(d, ax.v), Dot(d, ax.n)};
        };
        const auto inBox = [&](const Offsets& o) { return Abs64(o.du) <= p && Abs64(o.dv) <= p && o.dn >= nMin && o.dn <= nMax; };
        const auto inFootprint = [&](const Offsets& o) {
            return Abs64(o.du * Dv - o.dv * Du) <= width && Abs64(o.du * Du + o.dv * Dv) <= thickness;
        };
        // Outside the box every cell reads as a wall (support, never open).
        const auto read = [&](const glm::ivec3& c) { return inBox(offsets(c)) ? cache.Get(c) : VoxelCache::kWallId; };
        const auto open = [&](const glm::ivec3& c) { return IsOpenId(read(c)); };
        const auto burstCell = [&](const glm::ivec3& c) { return open(c) && !open(c - ax.n); };

        const int64_t side = 2 * p + 1;
        std::vector<uint64_t> visited(static_cast<size_t>((side * side * (nMax - nMin + 1) + 63) / 64), 0u);
        const auto bit = [&](const Offsets& o) {
            return static_cast<size_t>(((o.dn - nMin) * side + (o.dv + p)) * side + (o.du + p));
        };
        const auto seen = [&](const Offsets& o) { const size_t i = bit(o); return (visited[i >> 6] >> (i & 63)) & 1u; };
        const auto mark = [&](const Offsets& o) { const size_t i = bit(o); visited[i >> 6] |= uint64_t{1} << (i & 63); };

        using Key = std::tuple<int64_t, int64_t, int64_t, int64_t, int32_t, int32_t, int32_t>;
        std::priority_queue<Key, std::vector<Key>, std::greater<Key>> heap;
        const auto push = [&](const glm::ivec3& c, const Offsets& o) {
            mark(o);
            heap.emplace(o.du * o.du + o.dv * o.dv, o.du, o.dv, o.dn, c.z, c.y, c.x);
        };

        if (!inBox(offsets(params.seed)) || !burstCell(params.seed)) {
            out.chunksRead = cache.ChunksRead();
            return out;
        }
        push(params.seed, offsets(params.seed));
        const std::array<glm::ivec3, 4> steps = {ax.u, -ax.u, ax.v, -ax.v};
        const int stepMax = params.stepMax;
        while (!heap.empty() && out.cells.size() < params.pops && out.visits < params.visitCap) {
            const Key key = heap.top();
            heap.pop();
            const glm::ivec3 c(std::get<6>(key), std::get<5>(key), std::get<4>(key));
            out.cells.push_back(c);
            out.lateral.push_back(static_cast<int32_t>(Abs64(std::get<1>(key) * Dv - std::get<2>(key) * Du) / Dp));
            for (const glm::ivec3& e : steps) {
                const glm::ivec3 pc = c + e;
                bool found = false;
                glm::ivec3 q(0);
                // k = 0, +1, -1, +2, -2, ... +stepMax, -stepMax: the first cell with a clear way.
                for (int i = 0; i <= 2 * stepMax && !found; ++i) {
                    const int k = i == 0 ? 0 : ((i & 1) ? (i + 1) / 2 : -(i / 2));
                    const glm::ivec3 candidate = pc + ax.n * k;
                    ++out.visits;
                    if (!burstCell(candidate)) continue;
                    bool clear = true;
                    if (k > 0) {
                        for (int j = 1; j <= k && clear; ++j) clear = open(c + ax.n * j);
                    } else if (k < 0) {
                        for (int j = 0; j > k && clear; --j) clear = open(pc + ax.n * j);
                    }
                    if (!clear) continue;
                    found = true;
                    q = candidate;
                }
                if (!found) continue;
                const Offsets o = offsets(q);
                if (!inBox(o) || !inFootprint(o) || seen(o)) continue;
                push(q, o);
            }
        }
        out.chunksRead = cache.ChunksRead();
        return out;
    }

    PlumeColumn PlumeSingleColumn(const voxel::World& world, const OperatorWalls& walls, const glm::ivec3& seed, uint16_t riseMax) {
        PlumeColumn out;
        VoxelCache cache(world, walls);
        out.bottom = out.top = seed;
        if (IsOpenId(cache.Get(seed))) {
            while (out.top.y - seed.y < static_cast<int32_t>(riseMax) && IsOpenId(cache.Get(out.top + glm::ivec3(0, 1, 0)))) {
                ++out.top.y;
                ++out.visits;
            }
        }
        out.chunksRead = cache.ChunksRead();
        return out;
    }

    FillBands BarricadeFill(const voxel::World& world, const OperatorWalls& walls, const BurstResult& strip, const FaceAxes& axes,
                            uint8_t rimHeight, uint8_t bandCount, uint16_t halfWidth) {
        FillBands out;
        VoxelCache cache(world, walls);
        const int64_t bandsUsed = std::clamp<int64_t>(bandCount, 1, static_cast<int64_t>(out.bands.size()));
        std::set<std::tuple<int, int, int>> taken;
        const auto add = [&](const glm::ivec3& c, size_t band) {
            if (!taken.emplace(c.x, c.y, c.z).second) return;
            out.bands[band].push_back(c);
            ++out.cells;
        };
        for (size_t i = 0; i < strip.cells.size(); ++i) {
            const glm::ivec3 c = strip.cells[i];
            const int64_t lateral = i < strip.lateral.size() ? strip.lateral[i] : 0;
            const size_t band = static_cast<size_t>(std::min(bandsUsed - 1, lateral * bandsUsed / (static_cast<int64_t>(halfWidth) + 1)));
            add(c, band);
            for (int j = 1; j < rimHeight; ++j) {
                const glm::ivec3 above = c + axes.n * j;
                if (!IsOpenId(cache.Get(above))) break;
                add(above, band);
            }
        }
        return out;
    }

    WorldOp MapCellsOp(uint8_t map, std::span<const glm::ivec3> cells) {
        if (cells.empty()) return WorldOp::MapMasked(map, glm::ivec3(0), voxel::VoxelMask(glm::ivec3(0)));
        glm::ivec3 lo(std::numeric_limits<int32_t>::max()), hi(std::numeric_limits<int32_t>::min());
        for (const glm::ivec3& c : cells) {
            lo = glm::min(lo, c);
            hi = glm::max(hi, c);
        }
        voxel::VoxelMask mask(hi - lo + glm::ivec3(1));
        for (const glm::ivec3& c : cells) mask.Set(c - lo, true);
        return WorldOp::MapMasked(map, lo, std::move(mask));
    }
}
