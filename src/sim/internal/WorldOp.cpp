module;

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <vector>

module vortex.sim;

import vortex.voxel;

namespace vortex::sim {

    WorldOp WorldOp::Carve(const voxel::SphereQ& sphere, uint16_t power) {
        WorldOp op;
        op.kind = WorldOpKind::CarveSphere;
        op.sphere = sphere;
        op.power = power;
        return op;
    }

    WorldOp WorldOp::Fill(const voxel::SphereQ& sphere, uint8_t id) {
        WorldOp op;
        op.kind = WorldOpKind::FillSphere;
        op.sphere = sphere;
        op.id = id;
        return op;
    }

    WorldOp WorldOp::Swap(const voxel::SphereQ& sphere, uint8_t from, uint8_t to) {
        WorldOp op;
        op.kind = WorldOpKind::SwapSphere;
        op.sphere = sphere;
        op.from = from;
        op.id = to;
        return op;
    }

    WorldOp WorldOp::Box(const glm::ivec3& min, const glm::ivec3& max, uint8_t id) {
        WorldOp op;
        op.kind = WorldOpKind::FillBox;
        op.min = min;
        op.max = max;
        op.id = id;
        return op;
    }

    WorldOp WorldOp::Detach(const glm::ivec3& origin, voxel::VoxelMask mask, const glm::vec3& velocity, uint32_t netId) {
        WorldOp op;
        op.kind = WorldOpKind::Detach;
        op.min = origin;
        op.mask = std::move(mask);
        op.velocity = velocity;
        op.netId = netId;
        return op;
    }

    WorldOp WorldOp::Clear(const glm::ivec3& origin, voxel::VoxelMask mask) {
        WorldOp op;
        op.kind = WorldOpKind::ClearMask;
        op.min = origin;
        op.mask = std::move(mask);
        return op;
    }

    WorldOp WorldOp::Map(uint8_t map, const voxel::ShapeQ& shape) {
        WorldOp op;
        op.kind = WorldOpKind::MapShape;
        op.map = map;
        op.shape = shape;
        return op;
    }

    WorldOp WorldOp::MapMasked(uint8_t map, const glm::ivec3& origin, voxel::VoxelMask mask) {
        WorldOp op;
        op.kind = WorldOpKind::MapMask;
        op.map = map;
        op.min = origin;
        op.axis = voxel::ShortestRunAxis(mask);
        op.mask = std::move(mask);
        return op;
    }

    WorldOp WorldOp::Bore(const voxel::ShapeQ& capsule, uint16_t power) {
        WorldOp op;
        op.kind = WorldOpKind::CarveCapsule;
        op.shape = capsule;
        op.power = power;
        return op;
    }

    void WorldOp::Bounds(glm::ivec3& outMin, glm::ivec3& outMax) const {
        switch (kind) {
            case WorldOpKind::CarveSphere:
            case WorldOpKind::FillSphere:
            case WorldOpKind::SwapSphere:
                voxel::SphereVoxelBounds(sphere, outMin, outMax);
                return;
            case WorldOpKind::FillBox:
                outMin = min;
                outMax = glm::max(max, min);
                return;
            case WorldOpKind::Detach:
            case WorldOpKind::ClearMask:
            case WorldOpKind::MapMask:
                outMin = min;
                outMax = min + glm::max(mask.Size(), glm::ivec3(0));
                return;
            case WorldOpKind::MapShape:
            case WorldOpKind::CarveCapsule:
                voxel::ShapeVoxelBounds(shape, outMin, outMax);
                return;
        }
        outMin = outMax = glm::ivec3(0);
    }

    OpResult ApplyOp(voxel::World& world, const WorldOp& op, std::vector<voxel::VoxelEdit>* edits, const voxel::ChunkClip* clip) {
        OpResult result;
        switch (op.kind) {
            case WorldOpKind::CarveSphere:
                result.changed = world.CarveSphereQ(op.sphere, op.power, edits, &result.report, clip);
                break;
            case WorldOpKind::FillSphere:
                result.changed = world.FillSphereQ(op.sphere, op.id, edits, &result.report, clip);
                break;
            case WorldOpKind::SwapSphere:
                result.changed = world.SwapSphereQ(op.sphere, op.from, op.id, edits, &result.report, clip);
                break;
            case WorldOpKind::FillBox:
                result.changed = world.FillBox(op.min, op.max, op.id, edits, &result.report, clip);
                break;
            case WorldOpKind::Detach:
            case WorldOpKind::ClearMask:
                result.changed = world.ClearMask(op.min, op.mask, edits, &result.report, clip);
                break;
            case WorldOpKind::MapShape:
            case WorldOpKind::MapMask: {
                // Map 0 is the identity and unknown maps change nothing: no voxel is visited.
                if (op.map == 0 || op.map >= voxel::kTurningMapCount) break;
                const voxel::TurningMap& map = voxel::WorldTurningTable().maps[op.map];
                result.changed = op.kind == WorldOpKind::MapShape ? world.MapShapeQ(op.shape, map, edits, &result.report, clip)
                                                                  : world.MapMaskQ(op.min, op.mask, map, edits, &result.report, clip);
                break;
            }
            case WorldOpKind::CarveCapsule:
                result.changed = world.CarveCapsuleQ(op.shape, op.power, edits, &result.report, clip);
                break;
        }
        return result;
    }

    bool OpChunkRange(const WorldOp& op, glm::ivec3& minChunk, glm::ivec3& maxChunk) {
        glm::ivec3 lo, hi;
        op.Bounds(lo, hi);
        if (hi.x <= lo.x || hi.y <= lo.y || hi.z <= lo.z) return false;
        minChunk = voxel::VoxelToChunk(lo);
        maxChunk = voxel::VoxelToChunk(hi - glm::ivec3(1));
        return true;
    }

    bool OpTouchesChunk(const WorldOp& op, const glm::ivec3& chunk) {
        glm::ivec3 lo, hi;
        op.Bounds(lo, hi);
        const glm::ivec3 origin = voxel::ChunkToVoxel(chunk);
        lo = glm::max(lo, origin);
        hi = glm::min(hi, origin + glm::ivec3(voxel::kChunkSize));
        if (hi.x <= lo.x || hi.y <= lo.y || hi.z <= lo.z) return false;
        switch (op.kind) {
            case WorldOpKind::CarveSphere:
            case WorldOpKind::FillSphere:
            case WorldOpKind::SwapSphere: return voxel::SphereTouchesBox(op.sphere, lo, hi);
            case WorldOpKind::FillBox:
            case WorldOpKind::MapShape:
            case WorldOpKind::CarveCapsule: return true;
            case WorldOpKind::Detach:
            case WorldOpKind::ClearMask:
            case WorldOpKind::MapMask:
                for (int z = lo.z; z < hi.z; ++z)
                    for (int y = lo.y; y < hi.y; ++y) {
                        const size_t i = op.mask.Index(lo.x - op.min.x, y - op.min.y, z - op.min.z);
                        if (op.mask.AnyInRange(i, i + static_cast<size_t>(hi.x - lo.x))) return true;
                    }
                return false;
        }
        return false;
    }

    size_t OpWireBytes(const WorldOp& op) {
        switch (op.kind) {
            case WorldOpKind::CarveSphere: return 17;
            case WorldOpKind::FillSphere: return 16;
            case WorldOpKind::SwapSphere: return 17;
            case WorldOpKind::FillBox: return 26;
            case WorldOpKind::Detach: return 29 + voxel::MaskRunBytes(op.mask, 0);
            case WorldOpKind::ClearMask: return 19 + voxel::MaskRunBytes(op.mask, 0);
            case WorldOpKind::MapShape: {
                // kind, map, shape kind, then sphere 14 / capsule 26 / box 24 / cylinder 16 bytes of parameters.
                constexpr size_t kBytes[4] = {17, 29, 27, 19};
                return op.shape.kind < 4 ? kBytes[op.shape.kind] : 3;
            }
            case WorldOpKind::MapMask: return 21 + voxel::MaskRunBytes(op.mask, op.axis);
            case WorldOpKind::CarveCapsule: return 29;   // kind, A and B (i32 x 3 each), radius u16, power u16
        }
        return 0;
    }

    voxel::VoxelVolume BuildDetachedPiece(const WorldOp& op, std::span<const voxel::VoxelEdit> removed) {
        voxel::VoxelVolume piece(op.mask.Size());
        for (const voxel::VoxelEdit& edit : removed) piece.Set(edit.voxel - op.min, edit.oldId);
        return piece;
    }
}
