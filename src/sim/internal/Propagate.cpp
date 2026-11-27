module;

#include <glm/glm.hpp>

#include <algorithm>
#include <cstdlib>
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
        const bool alongNormal = dp == 0 || (du0 == 1 && dv0 == 0 && Dot(params.aim, ax.u) == 0 && Dot(params.aim, ax.v) == 0);
        const int64_t width = static_cast<int64_t>(params.halfWidth) * Dp;
        const int64_t thickness = static_cast<int64_t>(params.halfThickness) * Dp;
        const int64_t disc = params.discRadius;

        struct Offsets {
            int64_t du, dv, dn;
        };
        const auto offsets = [&](const glm::ivec3& c) {
            const glm::ivec3 d = c - params.seed;
            return Offsets{Dot(d, ax.u), Dot(d, ax.v), Dot(d, ax.n)};
        };
        const auto inBox = [&](const Offsets& o) { return Abs64(o.du) <= p && Abs64(o.dv) <= p && o.dn >= nMin && o.dn <= nMax; };
        const auto inFootprint = [&](const Offsets& o) {
            if (disc > 0) return o.du * o.du + o.dv * o.dv <= disc * disc;
            return Abs64(o.du * Dv - o.dv * Du) <= width && Abs64(o.du * Du + o.dv * Dv) <= thickness;
        };
        const auto isFront = [&](int64_t du, int64_t dv) {
            if (disc == 0) return true;
            if (du * du + dv * dv <= (disc - 1) * (disc - 1)) return false;
            return alongNormal || du * Du + dv * Dv >= 0;
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
            out.front.push_back(isFront(std::get<1>(key), std::get<2>(key)) ? uint8_t{1} : uint8_t{0});
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

    PlumeResult PlumeOperator(const voxel::World& world, const OperatorWalls& walls, const PlumeParams& params) {
        PlumeResult out;
        VoxelCache cache(world, walls);
        const glm::ivec3 s0 = params.seed;
        const int32_t rf = params.seedRadius;
        const int32_t side = static_cast<int32_t>(params.lateral) + rf + 1;
        const glm::ivec3 boxMin = s0 - glm::ivec3(side, rf + 4, side);
        const glm::ivec3 boxMax = s0 + glm::ivec3(side, static_cast<int32_t>(params.riseMax) + rf + 2, side);   // inclusive
        const glm::ivec3 size = boxMax - boxMin + glm::ivec3(1);
        std::vector<uint64_t> visited(static_cast<size_t>((int64_t{size.x} * size.y * size.z + 63) / 64), 0u);
        const auto inBox = [&](const glm::ivec3& c) { return glm::all(glm::greaterThanEqual(c, boxMin)) && glm::all(glm::lessThanEqual(c, boxMax)); };
        const auto bit = [&](const glm::ivec3& c) {
            const glm::ivec3 l = c - boxMin;
            return static_cast<size_t>((int64_t{l.z} * size.y + l.y) * size.x + l.x);
        };
        const auto seen = [&](const glm::ivec3& c) { const size_t i = bit(c); return ((visited[i >> 6] >> (i & 63)) & 1u) != 0; };
        const auto mark = [&](const glm::ivec3& c) { const size_t i = bit(c); visited[i >> 6] |= uint64_t{1} << (i & 63); };
        const auto open = [&](const glm::ivec3& c) {
            ++out.visits;
            return inBox(c) && IsOpenId(cache.Get(c));
        };
        const int32_t topY = s0.y + static_cast<int32_t>(params.riseMax);

        using Key = std::tuple<int32_t, int64_t, int32_t, int32_t>;   // (-y, d2, z, x); y follows from -y
        std::priority_queue<Key, std::vector<Key>, std::greater<Key>> heap;
        const auto push = [&](const glm::ivec3& c) {
            mark(c);
            const int64_t dx = c.x - s0.x, dz = c.z - s0.z;
            heap.emplace(-c.y, dx * dx + dz * dz, c.z, c.x);
        };

        // 1. Rise: each seed of the disc, in (a, b) order, while the cell above is open.
        for (int32_t a = -rf; a <= rf; ++a)
            for (int32_t b = -rf; b <= rf; ++b) {
                if (a * a + b * b > rf * rf || out.visits >= params.visitCap) continue;
                const glm::ivec3 seed = s0 + params.axes.u * a + params.axes.v * b;
                if (!inBox(seed) || seen(seed) || !open(seed)) continue;
                mark(seed);
                glm::ivec3 c = seed;
                bool ceiling = false;
                while (true) {
                    const glm::ivec3 up = c + glm::ivec3(0, 1, 0);
                    if (up.y > seed.y + static_cast<int32_t>(params.riseMax)) break;
                    if (!open(up)) {
                        ceiling = true;
                        break;
                    }
                    if (seen(up)) break;
                    out.rise.push_back(c);
                    mark(up);
                    c = up;
                }
                out.front.push_back(c);
                if (ceiling) {
                    const int64_t dx = c.x - s0.x, dz = c.z - s0.z;
                    heap.emplace(-c.y, dx * dx + dz * dz, c.z, c.x);
                }
            }

        // 2. Spread under the ceilings: highest first, then nearest the hit column.
        const std::array<glm::ivec3, 4> sides = {glm::ivec3(1, 0, 0), glm::ivec3(-1, 0, 0), glm::ivec3(0, 0, 1), glm::ivec3(0, 0, -1)};
        uint32_t pops = 0;
        const size_t columnTops = out.front.size();
        while (!heap.empty() && pops < params.runPops && out.visits < params.visitCap) {
            const Key key = heap.top();
            heap.pop();
            ++pops;
            const glm::ivec3 c(std::get<3>(key), -std::get<0>(key), std::get<2>(key));
            const glm::ivec3 up = c + glm::ivec3(0, 1, 0);
            const bool upOpen = up.y <= topY && open(up);
            if (upOpen && !seen(up)) {
                push(up);
                if (std::find(out.front.begin(), out.front.begin() + static_cast<std::ptrdiff_t>(columnTops), c) ==
                    out.front.begin() + static_cast<std::ptrdiff_t>(columnTops))
                    out.run.push_back(c);
                continue;
            }
            const bool underCeiling = !upOpen && up.y <= topY;
            const bool isTop = std::find(out.front.begin(), out.front.begin() + static_cast<std::ptrdiff_t>(columnTops), c) !=
                               out.front.begin() + static_cast<std::ptrdiff_t>(columnTops);
            if (!isTop) {
                if (underCeiling) out.front.push_back(c);
                else out.run.push_back(c);
            }
            for (const glm::ivec3& e : sides) {
                const glm::ivec3 n = c + e;
                if (std::abs(n.x - s0.x) > static_cast<int32_t>(params.lateral) || std::abs(n.z - s0.z) > static_cast<int32_t>(params.lateral))
                    continue;
                if (!inBox(n) || seen(n) || !open(n)) continue;
                push(n);
            }
        }
        out.chunksRead = cache.ChunksRead();
        return out;
    }

    namespace {
        int64_t Dot64(const glm::i64vec3& a, const glm::i64vec3& b) { return a.x * b.x + a.y * b.y + a.z * b.z; }

        /// Half away from zero: (2n + d) / (2d) for n >= 0, (2n - d) / (2d) below.
        int64_t RoundDiv(int64_t n, int64_t d) { return n >= 0 ? (2 * n + d) / (2 * d) : (2 * n - d) / (2 * d); }
    }

    bool IsOpenId(uint8_t id) {
        static const std::array<bool, 256> open = [] {
            voxel::MaterialLibrary scratch;
            voxel::RegisterDefaultWorldMaterials(scratch);
            std::array<bool, 256> out{};
            out[0] = true;
            for (uint32_t i = 1; i < 255; ++i)
                out[i] = !scratch.Name(voxel::kWorldPalette, static_cast<uint8_t>(i)).empty() &&
                         (scratch.Get(voxel::kWorldPalette, static_cast<uint8_t>(i)).flags & voxel::MaterialFlag_Film) != 0;
            return out;
        }();
        return open[id];
    }

    PierceResult Pierce(const voxel::World& world, const OperatorWalls& walls, const PierceParams& params) {
        PierceResult out;
        VoxelCache cache(world, walls);
        const int64_t r = std::min<int64_t>(params.radiusQ, 16 * kMaxPierceRadiusVoxels);
        const glm::i64vec3 aim(params.aim);
        const int64_t len = static_cast<int64_t>(math::ISqrt64(static_cast<uint64_t>(Dot64(aim, aim))));
        out.capsule.kind = voxel::kShapeCapsule;
        out.capsule.radiusQ = static_cast<uint16_t>(r);
        out.budgetLeft = params.budget;
        if (len == 0 || params.boreCost == nullptr) return out;
        const glm::i64vec3 S(RoundDiv(16 * aim.x, len), RoundDiv(16 * aim.y, len), RoundDiv(16 * aim.z, len));
        out.step = glm::ivec3(S);
        const glm::i64vec3 hit(params.hitQ);
        const glm::i64vec3 A = hit - (S * (2 * r)) / int64_t{16};
        const std::array<bool, 256> carvable = voxel::CarvableIdsFor(params.materials, params.power);
        const int64_t maxLen = int64_t{16} * kMaxBoreVoxels;
        const int64_t stubSlices = (2 * r) / 16;

        const auto capsule = [&](const glm::i64vec3& b) {
            voxel::ShapeQ s;
            s.kind = voxel::kShapeCapsule;
            s.a = glm::ivec3(A);
            s.b = glm::ivec3(b);
            s.radiusQ = static_cast<uint16_t>(r);
            return s;
        };
        // Liquids and films cost nothing; the element's pre-map (Thermal's Heat on metal) decides the rest.
        const auto isLiquid = [&](uint8_t id) {
            return params.materials != nullptr &&
                   (params.materials->Get(voxel::kWorldPalette, id).flags & (voxel::MaterialFlag_Liquid | voxel::MaterialFlag_Film)) != 0;
        };
        const auto pre = [&](uint8_t id) { return params.preMap != nullptr && id != VoxelCache::kWallId ? (*params.preMap)[id] : id; };
        std::vector<glm::ivec3> slice;
        glm::i64vec3 prev = A;
        for (int64_t k = 0;; ++k) {
            const glm::i64vec3 b = A + S * k;
            if (k > params.maxSlices) {
                out.stop = PierceStop::Slices;
                break;
            }
            if (k >= 1 && Dot64(b - hit, b - hit) > maxLen * maxLen) {
                out.stop = PierceStop::Length;
                break;
            }
            // The new voxels: in Capsule(A, B_k) and not in Capsule(A, B_{k-1}), over Capsule(B_{k-1}, B_k)'s box.
            const voxel::ShapeQ now = capsule(b), before = capsule(prev);
            voxel::ShapeQ cap = now;
            cap.a = glm::ivec3(prev);
            glm::ivec3 lo, hi;
            voxel::ShapeVoxelBounds(cap, lo, hi);
            ++out.scanned;
            slice.clear();
            for (int z = lo.z; z < hi.z; ++z)
                for (int y = lo.y; y < hi.y; ++y)
                    for (int x = lo.x; x < hi.x; ++x) {
                        const glm::ivec3 v(x, y, z);
                        if (!voxel::InShapeQ(now, v) || (k > 0 && voxel::InShapeQ(before, v))) continue;
                        slice.push_back(v);
                    }
            if (out.visits + slice.size() > kPierceVisitCap) {
                out.stop = PierceStop::VisitCap;
                break;
            }
            uint64_t cost = 0;
            uint32_t solid = 0;
            bool stop = false;
            for (const glm::ivec3& v : slice) {
                const uint8_t id = pre(cache.Get(v));
                if (id == VoxelCache::kWallId) {
                    out.stop = PierceStop::Wall;
                    stop = true;
                    break;
                }
                if (id == 0 || isLiquid(id)) continue;
                if (!carvable[id] || (*params.boreCost)[id] == kBoreStop) {
                    out.stop = PierceStop::TooHard;
                    stop = true;
                    break;
                }
                cost += (*params.boreCost)[id];
                ++solid;
            }
            if (stop) break;
            if (cost > out.budgetLeft) {
                out.stop = PierceStop::Budget;
                break;
            }
            out.budgetLeft -= static_cast<uint32_t>(cost);
            out.visits += static_cast<uint32_t>(slice.size());
            out.paidVoxels += solid;
            if (k <= stubSlices) out.frontVoxels += static_cast<uint32_t>(slice.size());
            if (params.paidOut != nullptr)
                for (const glm::ivec3& v : slice) {
                    const uint8_t id = pre(cache.Get(v));
                    if (id != 0 && !isLiquid(id)) params.paidOut->push_back(v);
                }
            out.bored = true;
            out.slices = static_cast<uint32_t>(k);
            out.capsule = now;
            prev = b;
        }
        out.chunksRead = cache.ChunksRead();
        return out;
    }

    ShatterResult ShatterFlood(const voxel::World& world, const OperatorWalls& walls, const voxel::SphereQ& sphere, uint8_t maxDepth,
                               uint32_t visitCap) {
        ShatterResult out;
        VoxelCache cache(world, walls);
        const voxel::TurningMap& shatter = voxel::WorldTurningTable().maps[voxel::Map_Shatter];
        const auto shatterable = [&](uint8_t id) { return id != VoxelCache::kWallId && shatter[id] != id; };
        const std::array<glm::ivec3, 6> faces = {glm::ivec3(1, 0, 0), glm::ivec3(-1, 0, 0), glm::ivec3(0, 1, 0),
                                                 glm::ivec3(0, -1, 0), glm::ivec3(0, 0, 1), glm::ivec3(0, 0, -1)};
        using Key = std::tuple<int32_t, int32_t, int32_t, int32_t>;   // (depth, z, y, x)
        std::priority_queue<Key, std::vector<Key>, std::greater<Key>> heap;
        std::set<std::tuple<int32_t, int32_t, int32_t>> seen;
        glm::ivec3 lo, hi;
        voxel::SphereVoxelBounds(sphere, lo, hi);
        for (int z = lo.z; z < hi.z; ++z)
            for (int y = lo.y; y < hi.y; ++y)
                for (int x = lo.x; x < hi.x; ++x) {
                    const glm::ivec3 v(x, y, z);
                    if (!voxel::InSphereQ(sphere, v) || !shatterable(cache.Get(v))) continue;
                    bool exposed = false;
                    for (const glm::ivec3& f : faces) exposed = exposed || cache.Get(v + f) == 0;
                    if (!exposed) continue;
                    seen.emplace(x, y, z);
                    heap.emplace(0, z, y, x);
                }
        while (!heap.empty() && out.visits < visitCap) {
            const auto [depth, z, y, x] = heap.top();
            heap.pop();
            ++out.visits;
            const glm::ivec3 v(x, y, z);
            out.cells.push_back(v);
            if (depth + 1 >= maxDepth) continue;
            for (const glm::ivec3& f : faces) {
                const glm::ivec3 n = v + f;
                ++out.visits;
                if (!voxel::InSphereQ(sphere, n) || seen.contains({n.x, n.y, n.z}) || !shatterable(cache.Get(n))) continue;
                seen.emplace(n.x, n.y, n.z);
                heap.emplace(depth + 1, n.z, n.y, n.x);
            }
        }
        out.chunksRead = cache.ChunksRead();
        return out;
    }

    namespace {
        int64_t FloorShift(int64_t v, int s) { return v >> s; }   // arithmetic: rounds toward minus infinity

        int64_t RoundDivArc(int64_t n, int64_t d) { return n >= 0 ? (2 * n + d) / (2 * d) : (2 * n - d) / (2 * d); }
    }

    glm::i64vec3 ArcLaunch(const glm::ivec3& aim, int64_t speedFine) {
        return {RoundDivArc(int64_t{aim.x} * speedFine, 16384), RoundDivArc(int64_t{aim.y} * speedFine, 16384),
                RoundDivArc(int64_t{aim.z} * speedFine, 16384)};
    }

    glm::i64vec3 LobLaunch(const glm::ivec3& fromQ, const glm::ivec3& toQ, int64_t horizontalFine, int64_t gravity, uint32_t minTicks,
                           uint32_t maxTicks, uint32_t& ticks) {
        const int64_t dx = (int64_t{toQ.x} - fromQ.x) << kArcFineShift, dz = (int64_t{toQ.z} - fromQ.z) << kArcFineShift;
        const int64_t dy = (int64_t{toQ.y} - fromQ.y) << kArcFineShift;
        const int64_t dist = static_cast<int64_t>(math::ISqrt64(static_cast<uint64_t>(dx * dx + dz * dz)));
        const int64_t speed = std::max<int64_t>(horizontalFine, 1);
        const int64_t t = std::clamp<int64_t>((dist + speed - 1) / speed, std::max<uint32_t>(minTicks, 1u), std::max<uint32_t>(maxTicks, 1u));
        ticks = static_cast<uint32_t>(t);
        return {RoundDivArc(dx, t), RoundDivArc(2 * dy + gravity * t * t, 2 * t), RoundDivArc(dz, t)};
    }

    ArcHit TraceArc(const voxel::World& world, const OperatorWalls& walls, const ArcParams& params) {
        ArcHit out;
        VoxelCache cache(world, walls);
        glm::i64vec3 p = glm::i64vec3(params.startQ) * int64_t{1 << kArcFineShift};
        glm::i64vec3 v = params.velocity;
        v.y -= params.gravity / 2;
        glm::ivec3 voxel(static_cast<int32_t>(FloorShift(params.startQ.x, 4)), static_cast<int32_t>(FloorShift(params.startQ.y, 4)),
                         static_cast<int32_t>(FloorShift(params.startQ.z, 4)));
        out.apexQ = params.startQ.y;
        const auto stop = [&](const glm::ivec3& at, const glm::ivec3& normal, const glm::ivec3& pointQ, uint32_t tick, uint8_t id) {
            out.hit = true;
            out.wall = id == VoxelCache::kWallId;
            out.voxel = at;
            out.normal = normal;
            out.hitQ = pointQ;
            out.ticks = tick;
        };
        ++out.visits;
        if (const uint8_t id = cache.Get(voxel); !IsOpenId(id)) {
            stop(voxel, glm::ivec3(0, 1, 0), params.startQ, 0, id);
            out.chunksRead = cache.ChunksRead();
            return out;
        }
        for (uint32_t tick = 1; tick <= params.maxTicks && !out.hit; ++tick) {
            const glm::i64vec3 aFine = p, bFine = p + v;
            p = bFine;
            v.y -= params.gravity;
            const glm::i64vec3 a(FloorShift(aFine.x, kArcFineShift), FloorShift(aFine.y, kArcFineShift), FloorShift(aFine.z, kArcFineShift));
            const glm::i64vec3 b(FloorShift(bFine.x, kArcFineShift), FloorShift(bFine.y, kArcFineShift), FloorShift(bFine.z, kArcFineShift));
            out.apexQ = std::max<int32_t>(out.apexQ, static_cast<int32_t>(b.y));
            // The segment a-b (q) from the current voxel: t = num[i] / den[i] of the next boundary on each axis.
            const glm::i64vec3 d = b - a;
            glm::i64vec3 num(0), den(0);
            glm::ivec3 step(0);
            for (int i = 0; i < 3; ++i) {
                den[i] = d[i] < 0 ? -d[i] : d[i];
                step[i] = d[i] > 0 ? 1 : d[i] < 0 ? -1 : 0;
                if (step[i] > 0) num[i] = (int64_t{voxel[i]} + 1) * 16 - a[i];
                else if (step[i] < 0) num[i] = a[i] - int64_t{voxel[i]} * 16;
            }
            while (true) {
                // The axis whose boundary comes first (num / den smallest, within the segment: num <= den).
                int axis = -1;
                for (int i = 0; i < 3; ++i) {
                    if (step[i] == 0 || num[i] > den[i]) continue;
                    if (axis < 0 || num[i] * den[axis] < num[axis] * den[i]) axis = i;
                }
                if (axis < 0) break;
                voxel[axis] += step[axis];
                ++out.visits;
                const uint8_t id = cache.Get(voxel);
                if (!IsOpenId(id)) {
                    glm::ivec3 normal(0);
                    normal[axis] = -step[axis];
                    const glm::i64vec3 at = a + d * num[axis] / std::max<int64_t>(den[axis], 1);
                    stop(voxel, normal, glm::ivec3(at), tick, id);
                    break;
                }
                num[axis] += 16;
            }
        }
        out.chunksRead = cache.ChunksRead();
        return out;
    }

    FlowKey FlowKeyOf(const glm::ivec3& c, const glm::ivec3& seed) {
        const int64_t dx = c.x - seed.x, dz = c.z - seed.z;
        return {c.y, dx * dx + dz * dz, c.z, c.x};
    }

    FlowResult FlowOperator(const voxel::World& world, const OperatorWalls& walls, const FlowParams& params) {
        FlowResult out;
        VoxelCache cache(world, walls);
        const glm::ivec3 s0 = params.seed;
        const int32_t rf = params.seedRadius;
        const int32_t side = static_cast<int32_t>(params.lateral) + rf + 1;
        const glm::ivec3 boxMin = s0 - glm::ivec3(side, static_cast<int32_t>(params.fallMax) + rf + 1, side);
        const glm::ivec3 boxMax = s0 + glm::ivec3(side, rf + 1, side);   // inclusive
        const glm::ivec3 size = boxMax - boxMin + glm::ivec3(1);
        std::vector<uint64_t> visited(static_cast<size_t>((int64_t{size.x} * size.y * size.z + 63) / 64), 0u);
        const auto inBox = [&](const glm::ivec3& c) { return glm::all(glm::greaterThanEqual(c, boxMin)) && glm::all(glm::lessThanEqual(c, boxMax)); };
        const auto bit = [&](const glm::ivec3& c) {
            const glm::ivec3 l = c - boxMin;
            return static_cast<size_t>((int64_t{l.z} * size.y + l.y) * size.x + l.x);
        };
        const auto seen = [&](const glm::ivec3& c) { const size_t i = bit(c); return ((visited[i >> 6] >> (i & 63)) & 1u) != 0; };
        const auto mark = [&](const glm::ivec3& c) { const size_t i = bit(c); visited[i >> 6] |= uint64_t{1} << (i & 63); };
        const auto open = [&](const glm::ivec3& c) {
            ++out.visits;
            return inBox(c) && IsOpenId(cache.Get(c));
        };
        std::priority_queue<FlowKey, std::vector<FlowKey>, std::greater<FlowKey>> heap;
        const glm::ivec3 down(0, -1, 0);

        // 1. Fall: each seed of the disc, in (a, b) order, down while the cell below is open.
        for (int32_t a = -rf; a <= rf; ++a)
            for (int32_t b = -rf; b <= rf; ++b) {
                if (a * a + b * b > rf * rf || out.visits >= params.visitCap) continue;
                const glm::ivec3 seed = s0 + params.axes.u * a + params.axes.v * b;
                if (!inBox(seed) || seen(seed) || !open(seed)) continue;
                mark(seed);
                glm::ivec3 c = seed;
                bool landed = false;
                for (uint32_t fell = 0;; ++fell) {
                    const glm::ivec3 below = c + down;
                    if (!open(below)) {
                        landed = true;
                        break;
                    }
                    // Another column passed or landed here: it carries on from there.
                    if (fell >= params.fallMax || seen(below)) break;
                    out.fall.push_back(c);
                    mark(below);
                    c = below;
                }
                if (!landed) continue;
                out.landings.push_back(c);
                heap.push(FlowKeyOf(c, s0));
            }

        // 2. Spread: lowest first, then nearest the hit column; a cell over open space falls.
        const std::array<glm::ivec3, 4> sides = {glm::ivec3(1, 0, 0), glm::ivec3(-1, 0, 0), glm::ivec3(0, 0, 1), glm::ivec3(0, 0, -1)};
        std::vector<FlowKey> popped;
        while (!heap.empty() && popped.size() < params.runPops && out.visits < params.visitCap) {
            const FlowKey key = heap.top();
            heap.pop();
            popped.push_back(key);
            const glm::ivec3 c(std::get<3>(key), std::get<0>(key), std::get<2>(key));
            const glm::ivec3 below = c + down;
            if (inBox(below) && !seen(below) && open(below)) {
                mark(below);
                heap.push(FlowKeyOf(below, s0));
                continue;
            }
            for (const glm::ivec3& e : sides) {
                const glm::ivec3 n = c + e;
                if (std::abs(n.x - s0.x) > static_cast<int32_t>(params.lateral) || std::abs(n.z - s0.z) > static_cast<int32_t>(params.lateral))
                    continue;
                if (!inBox(n) || seen(n) || !open(n)) continue;
                mark(n);
                heap.push(FlowKeyOf(n, s0));
            }
        }

        // 3. Roles: the frontCount smallest keys are the pool; the rest ran.
        std::sort(popped.begin(), popped.end());
        for (size_t i = 0; i < popped.size(); ++i) {
            const glm::ivec3 c(std::get<3>(popped[i]), std::get<0>(popped[i]), std::get<2>(popped[i]));
            (i < params.frontCount ? out.front : out.run).push_back(c);
        }
        out.chunksRead = cache.ChunksRead();
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
