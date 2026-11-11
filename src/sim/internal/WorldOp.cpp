module;

#include <glm/glm.hpp>

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
                outMin = min;
                outMax = min + glm::max(mask.Size(), glm::ivec3(0));
                return;
        }
        outMin = outMax = glm::ivec3(0);
    }

    OpResult ApplyOp(voxel::World& world, const WorldOp& op, std::vector<voxel::VoxelEdit>* edits) {
        OpResult result;
        switch (op.kind) {
            case WorldOpKind::CarveSphere:
                result.changed = world.CarveSphereQ(op.sphere, op.power, edits, &result.report);
                break;
            case WorldOpKind::FillSphere:
                result.changed = world.FillSphereQ(op.sphere, op.id, edits, &result.report);
                break;
            case WorldOpKind::SwapSphere:
                result.changed = world.SwapSphereQ(op.sphere, op.from, op.id, edits, &result.report);
                break;
            case WorldOpKind::FillBox:
                result.changed = world.FillBox(op.min, op.max, op.id, edits, &result.report);
                break;
            case WorldOpKind::Detach:
                result.changed = world.ClearMask(op.min, op.mask, edits, &result.report);
                break;
        }
        return result;
    }

    voxel::VoxelVolume BuildDetachedPiece(const WorldOp& op, std::span<const voxel::VoxelEdit> removed) {
        voxel::VoxelVolume piece(op.mask.Size());
        for (const voxel::VoxelEdit& edit : removed) piece.Set(edit.voxel - op.min, edit.oldId);
        return piece;
    }
}
