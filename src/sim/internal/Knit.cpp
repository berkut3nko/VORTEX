module;

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <tuple>
#include <utility>
#include <vector>

module vortex.sim;

import vortex.core;
import vortex.voxel;

namespace vortex::sim {

    namespace {
        bool BoxesMeet(const glm::ivec3& aMin, const glm::ivec3& aMax, const glm::ivec3& bMin, const glm::ivec3& bMax) {
            return glm::all(glm::lessThan(aMin, bMax)) && glm::all(glm::lessThan(bMin, aMax));
        }

        uint16_t LocalIndex(const glm::ivec3& v) {
            const glm::ivec3 l = v & glm::ivec3(voxel::kChunkMask);
            return static_cast<uint16_t>(l.x + voxel::kChunkSize * (l.y + voxel::kChunkSize * l.z));
        }

        glm::ivec3 LocalVoxel(uint16_t index) {
            return {index % voxel::kChunkSize, (index / voxel::kChunkSize) % voxel::kChunkSize, index / (voxel::kChunkSize * voxel::kChunkSize)};
        }
    }

    uint32_t KnitRegistry::NewOwner(const KnitOwnerInfo& info) {
        const uint32_t id = m_NextOwner++;
        Owner owner;
        owner.info = info;
        if (info.hasShape) voxel::ShapeVoxelBounds(info.shape, owner.reachMin, owner.reachMax);
        m_Owners.emplace(id, owner);
        return id;
    }

    void KnitRegistry::Record(uint32_t ownerId, std::span<const voxel::VoxelEdit> edits, const voxel::MaterialLibrary& materials) {
        const auto found = m_Owners.find(ownerId);
        if (found == m_Owners.end()) return;
        Owner& owner = found->second;
        const size_t layer = owner.info.layer < 2 ? owner.info.layer : 1;
        bool recorded = false;
        for (const voxel::VoxelEdit& edit : edits) {
            // The conjure layer records what became conjured matter, the turn layer every voxel the op turned.
            if (layer == 0 && (materials.Get(voxel::kWorldPalette, edit.newId).flags & voxel::MaterialFlag_Conjured) == 0) continue;
            if (layer == 1 && edit.newId == edit.oldId) continue;
            const glm::ivec3 c = voxel::VoxelToChunk(edit.voxel);
            std::vector<Entry>& entries = m_Chunks[ChunkKey(c.x, c.y, c.z)][layer];
            const uint16_t index = LocalIndex(edit.voxel);
            const auto at = std::lower_bound(entries.begin(), entries.end(), index,
                                             [](const Entry& e, uint16_t i) { return e.first < i; });
            if (at != entries.end() && at->first == index) {
                if (at->second == ownerId) continue;
                if (const auto old = m_Owners.find(at->second); old != m_Owners.end() && old->second.live > 0) --old->second.live;
                at->second = ownerId;
            } else {
                entries.insert(at, Entry(index, ownerId));
            }
            ++owner.live;
            if (!owner.any) {
                owner.boxMin = edit.voxel;
                owner.boxMax = edit.voxel + glm::ivec3(1);
            } else {
                owner.boxMin = glm::min(owner.boxMin, edit.voxel);
                owner.boxMax = glm::max(owner.boxMax, edit.voxel + glm::ivec3(1));
            }
            owner.any = true;
            recorded = true;
        }
        if (!recorded) return;
        if (owner.info.hasShape && owner.reachMin != owner.reachMax) {
            owner.reachMin = glm::min(owner.reachMin, owner.boxMin);
            owner.reachMax = glm::max(owner.reachMax, owner.boxMax);
        } else {
            owner.reachMin = owner.boxMin;
            owner.reachMax = owner.boxMax;
        }
        // An expiry by shape or box is exact only while no other owner of the layer shares the box.
        for (auto& [id, other] : m_Owners) {
            if (id == ownerId || !other.any || other.info.layer != owner.info.layer) continue;
            if (!BoxesMeet(owner.reachMin, owner.reachMax, other.reachMin, other.reachMax)) continue;
            owner.overlapped = true;
            other.overlapped = true;
        }
    }

    void KnitRegistry::Forget(uint32_t id, const Owner& owner) {
        if (!owner.any) return;
        const glm::ivec3 lo = voxel::VoxelToChunk(owner.boxMin), hi = voxel::VoxelToChunk(owner.boxMax - glm::ivec3(1));
        const size_t layer = owner.info.layer;
        for (int z = lo.z; z <= hi.z; ++z)
            for (int y = lo.y; y <= hi.y; ++y)
                for (int x = lo.x; x <= hi.x; ++x) {
                    const auto it = m_Chunks.find(ChunkKey(x, y, z));
                    if (it == m_Chunks.end()) continue;
                    std::vector<Entry>& entries = it->second[layer];
                    std::erase_if(entries, [id](const Entry& e) { return e.second == id; });
                    if (it->second[0].empty() && it->second[1].empty()) m_Chunks.erase(it);
                }
    }

    void KnitRegistry::Expire(Simulation& simulation, uint32_t id, std::vector<AppliedOp>& out) {
        const auto found = m_Owners.find(id);
        if (found == m_Owners.end()) return;
        const Owner owner = found->second;
        m_Owners.erase(found);
        if (owner.live == 0) {
            Forget(id, owner);
            return;
        }
        voxel::World& world = simulation.World();
        AppliedOp applied;
        if (!owner.overlapped && owner.info.hasShape) {
            applied.op = WorldOp::Map(owner.info.expiryMap, owner.info.shape);
        } else if (!owner.overlapped) {
            voxel::ShapeQ box;
            box.kind = voxel::kShapeBox;
            box.a = owner.boxMin;
            box.b = owner.boxMax;
            applied.op = WorldOp::Map(owner.info.expiryMap, box);
        } else {
            // The voxels it still owns that still hold an id its map changes.
            const uint8_t mapId = owner.info.expiryMap;
            if (mapId == 0 || mapId >= voxel::kTurningMapCount) {
                Forget(id, owner);
                return;
            }
            const voxel::TurningMap& map = voxel::WorldTurningTable().maps[mapId];
            std::vector<glm::ivec3> cells;
            const glm::ivec3 lo = voxel::VoxelToChunk(owner.boxMin), hi = voxel::VoxelToChunk(owner.boxMax - glm::ivec3(1));
            for (int z = lo.z; z <= hi.z; ++z)
                for (int y = lo.y; y <= hi.y; ++y)
                    for (int x = lo.x; x <= hi.x; ++x) {
                        const auto it = m_Chunks.find(ChunkKey(x, y, z));
                        if (it == m_Chunks.end()) continue;
                        const glm::ivec3 origin = voxel::ChunkToVoxel(glm::ivec3(x, y, z));
                        for (const Entry& e : it->second[owner.info.layer]) {
                            if (e.second != id) continue;
                            const glm::ivec3 v = origin + LocalVoxel(e.first);
                            const uint8_t now = world.GetVoxel(v);
                            if (map[now] != now) cells.push_back(v);
                        }
                    }
            if (cells.empty()) {
                Forget(id, owner);
                return;
            }
            applied.op = MapCellsOp(mapId, cells);
        }
        applied.result = ApplyOp(world, applied.op, &applied.edits);
        out.push_back(std::move(applied));
        Forget(id, owner);
    }

    std::vector<AppliedOp> KnitRegistry::RunExpiries(Simulation& simulation, uint32_t tick) {
        std::vector<std::pair<uint32_t, uint32_t>> due;   // (dueTick, owner)
        for (const auto& [id, owner] : m_Owners)
            if (owner.info.dueTick <= tick) due.emplace_back(owner.info.dueTick, id);
        std::sort(due.begin(), due.end());
        std::vector<AppliedOp> out;
        for (const auto& [dueTick, id] : due) Expire(simulation, id, out);
        return out;
    }

    std::vector<AppliedOp> KnitRegistry::MakeRoom(Simulation& simulation, uint8_t caster, uint32_t incoming) {
        std::vector<AppliedOp> out;
        uint32_t crumbled = 0;
        bool expired = false;
        while (LiveVoxelsOf(caster) + incoming > kMaxConjuredPerPlayer) {
            uint32_t oldest = 0;
            for (const auto& [id, owner] : m_Owners) {
                if (owner.info.caster == caster && owner.info.layer == 0) {
                    oldest = id;
                    break;   // the map is in id order
                }
            }
            if (oldest == 0) break;
            crumbled += m_Owners.at(oldest).live;
            Expire(simulation, oldest, out);
            expired = true;
        }
        if (expired) Log::Info("Magic: knit cap: player {}: oldest conjuring crumbled ({} voxels)", static_cast<unsigned>(caster), crumbled);
        return out;
    }

    uint32_t KnitRegistry::LiveVoxels(uint8_t layer) const {
        uint32_t n = 0;
        for (const auto& [id, owner] : m_Owners)
            if (owner.info.layer == layer) n += owner.live;
        return n;
    }

    uint32_t KnitRegistry::LiveVoxelsOf(uint8_t caster) const {
        uint32_t n = 0;
        for (const auto& [id, owner] : m_Owners)
            if (owner.info.caster == caster && owner.info.layer == 0) n += owner.live;
        return n;
    }

    const KnitOwnerInfo* KnitRegistry::OwnerAt(const glm::ivec3& v, uint8_t layer) const {
        const glm::ivec3 c = voxel::VoxelToChunk(v);
        const auto chunk = m_Chunks.find(ChunkKey(c.x, c.y, c.z));
        if (chunk == m_Chunks.end()) return nullptr;
        const std::vector<Entry>& entries = chunk->second[layer < 2 ? layer : 1];
        const uint16_t index = LocalIndex(v);
        const auto at = std::lower_bound(entries.begin(), entries.end(), index, [](const Entry& e, uint16_t i) { return e.first < i; });
        if (at == entries.end() || at->first != index) return nullptr;
        const auto owner = m_Owners.find(at->second);
        return owner == m_Owners.end() ? nullptr : &owner->second.info;
    }

    bool IsCollider(const voxel::MaterialLibrary& materials, uint8_t id) {
        return id != 0 && (materials.Get(voxel::kWorldPalette, id).flags & voxel::MaterialFlag_Liquid) == 0;
    }

    void StructureCredit::QueueSite(const glm::ivec3& chunk) {
        if (std::find(queue.begin(), queue.end(), chunk) != queue.end()) return;
        if (queue.size() >= kMaxQueued) {
            ++dropped;
            return;
        }
        queue.push_back(chunk);
    }

    void StructureCredit::QueueFromEdits(std::span<const voxel::VoxelEdit> edits, const voxel::MaterialLibrary& materials) {
        for (const voxel::VoxelEdit& edit : edits)
            if (IsCollider(materials, edit.oldId) && !IsCollider(materials, edit.newId)) QueueSite(voxel::VoxelToChunk(edit.voxel));
    }

    uint32_t StructureCredit::Run(Simulation& simulation, std::vector<BlastResult>& out) {
        credit = std::clamp<int64_t>(credit + kWorkPerTick, -6 * kWorkPerTick, kWorkPerTick);
        uint32_t ran = 0;
        BlastOptions options;
        options.noCarve = true;
        while (credit > 0 && !queue.empty()) {
            const glm::ivec3 chunk = queue.front();
            queue.pop_front();
            BlastResult result = Blast(simulation, ChunkSite(simulation.World(), chunk), options);
            credit -= static_cast<int64_t>(CastWorkOf(result).Total());
            out.push_back(std::move(result));
            ++ran;
        }
        return ran;
    }
}
