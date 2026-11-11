module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

module vortex.sim;

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.fx;
import vortex.physics;
import vortex.scene;

namespace vortex::sim {

    voxel::EditReport BlastResult::Report() const {
        voxel::EditReport out;
        for (const AppliedOp& applied : ops) {
            out.skipped.insert(out.skipped.end(), applied.result.report.skipped.begin(), applied.result.report.skipped.end());
            out.unsynced.insert(out.unsynced.end(), applied.result.report.unsynced.begin(), applied.result.report.unsynced.end());
        }
        return out;
    }

    uint32_t BlastResult::IslandCount() const {
        uint32_t n = 0;
        for (const AppliedOp& applied : ops) n += applied.op.kind == WorldOpKind::Detach ? 1u : 0u;
        return n;
    }

    uint32_t BlastResult::CrumbVoxels() const {
        uint32_t n = 0;
        for (const AppliedOp& applied : ops) n += applied.op.kind == WorldOpKind::ClearMask ? applied.result.changed : 0u;
        return n;
    }

    glm::vec3 BlastPush(const glm::vec3& center, float radius, const glm::vec3& position) {
        const glm::vec3 d = position - center;
        const float dist = glm::length(d);
        const float reach = radius * 3.0f;
        if (!(dist <= reach) || !(reach > 0.0f)) return glm::vec3(0.0f);
        const float falloff = 1.0f - dist / reach;
        return (dist > 1e-3f ? d / dist : glm::vec3(0.0f, 1.0f, 0.0f)) * (6.0f * falloff);
    }

    namespace {
        glm::ivec3 IslandBoxMin(const voxel::SphereQ& sphere) {
            const glm::ivec3 c = math::FloorDiv(sphere.center, voxel::kQuantaPerVoxel);
            return c - glm::ivec3(static_cast<int32_t>(sphere.radius / voxel::kQuantaPerVoxel) + kIslandSearchMargin);
        }
        glm::ivec3 IslandBoxMax(const voxel::SphereQ& sphere) {
            const glm::ivec3 c = math::FloorDiv(sphere.center, voxel::kQuantaPerVoxel);
            return c + glm::ivec3(static_cast<int32_t>(sphere.radius / voxel::kQuantaPerVoxel) + kIslandSearchMargin);
        }

        /// Step 2 of Blast: islands in the box of `sphere` become Detach operations, crumbs one
        /// ClearMask, each applied at once and appended to `result.ops`. False (and nothing done)
        /// when a chunk of the box is not loaded. The box is derived from the integer sphere only.
        bool SearchIslands(Simulation& simulation, const voxel::SphereQ& sphere, BlastResult& result) {
            voxel::World& world = simulation.World();
            const glm::vec3 center(result.center);
            const float radius = result.radius;
            bool searched = false;
            std::vector<voxel::FloatingIsland> islands = voxel::ExtractFloatingIslands(
                world, &simulation.Materials(), IslandBoxMin(sphere), IslandBoxMax(sphere), kMaxIslandVoxels, false, voxel::MaterialFlag_Liquid,
                &searched);
            // Islands under the piece threshold are crumbs: no Detach each, one ClearMask for all.
            glm::ivec3 crumbMin(std::numeric_limits<int32_t>::max()), crumbMax(std::numeric_limits<int32_t>::min());
            for (voxel::FloatingIsland& island : islands) {
                if (island.voxelCount < scene::kMinPieceVoxels) {
                    ++result.crumbs;
                    crumbMin = glm::min(crumbMin, island.origin);
                    crumbMax = glm::max(crumbMax, island.origin + island.volume.Size());
                    continue;
                }
                AppliedOp detach;
                const glm::vec3 corner(world.VoxelToWorld(island.origin));
                detach.op = WorldOp::Detach(island.origin, voxel::VoxelMask::FromVolume(island.volume), BlastPush(center, radius, corner));
                detach.result = ApplyOp(world, detach.op, &detach.edits);
                result.ops.push_back(std::move(detach));
            }
            if (result.crumbs > 0) {
                // The box lies inside the island box, which ExtractFloatingIslands keeps under 192
                // voxels per axis. Bits are set from the islands' own voxels, so the mask does not
                // depend on the order the islands come in.
                voxel::VoxelMask mask(crumbMax - crumbMin);
                for (const voxel::FloatingIsland& island : islands) {
                    if (island.voxelCount >= scene::kMinPieceVoxels) continue;
                    const glm::ivec3 size = island.volume.Size();
                    const glm::ivec3 offset = island.origin - crumbMin;
                    for (int z = 0; z < size.z; ++z)
                        for (int y = 0; y < size.y; ++y)
                            for (int x = 0; x < size.x; ++x) {
                                if (island.volume.Get(x, y, z) != 0) mask.Set(offset + glm::ivec3(x, y, z), true);
                            }
                }
                AppliedOp clear;
                clear.op = WorldOp::Clear(crumbMin, std::move(mask));
                clear.result = ApplyOp(world, clear.op, &clear.edits);
                result.ops.push_back(std::move(clear));
            }
            return searched;
        }
    }

    BlastResult Blast(Simulation& simulation, const BlastDesc& desc) {
        const double vs = static_cast<double>(simulation.World().VoxelSize());
        return Blast(simulation, voxel::QuantizeSphere(desc.center / vs, static_cast<double>(desc.radius) / vs),
                     voxel::QuantizePower(desc.power));
    }

    BlastResult Blast(Simulation& simulation, const voxel::SphereQ& sphere, uint16_t power) {
        voxel::World& world = simulation.World();
        physics::PhysicsWorld& physics = simulation.Physics();
        scene::Scene& scene = simulation.Scene();
        const double vs = static_cast<double>(world.VoxelSize());

        BlastResult result;
        constexpr double kInvQ = 1.0 / static_cast<double>(voxel::kQuantaPerVoxel);
        result.center = glm::dvec3(sphere.center) * (kInvQ * vs);
        result.radius = static_cast<float>(static_cast<double>(sphere.radius) * kInvQ * vs);
        const glm::vec3 center(result.center);
        const float radius = result.radius;

        // 1. Carve.
        {
            AppliedOp carve;
            carve.op = WorldOp::Carve(sphere, power);
            carve.result = ApplyOp(world, carve.op, &carve.edits);
            result.ops.push_back(std::move(carve));
        }

        // 2. Terrain that lost its support. Without the whole box loaded nothing can be proven:
        //    the world loads the box first and the search runs later (RetryIslandSearches).
        result.islandsSearched = SearchIslands(simulation, sphere, result);
        if (!result.islandsSearched) {
            world.RequestLoad(voxel::VoxelToChunk(IslandBoxMin(sphere)), voxel::VoxelToChunk(IslandBoxMax(sphere) - glm::ivec3(1)));
            std::vector<DeferredIslandSearch>& deferred = simulation.DeferredIslandSearches();
            if (deferred.size() == kMaxDeferredIslandSearches) deferred.erase(deferred.begin());
            deferred.push_back({sphere, 0});
        }

        // 3. Objects caught in the blast. The list is a snapshot: pieces made here are not hit twice.
        if (radius > 0.0f) {
            std::vector<scene::Entity> caught;
            scene.QueryBodies(math::AABB(center - glm::vec3(radius), center + glm::vec3(radius)), caught);
            for (const scene::Entity entity : caught) {
                scene::DamageResult damage = scene.Damage(entity, result.center, radius);
                if (damage.removed == 0) continue;
                ++result.objectsHit;
                result.objectVoxels += damage.removed;
                for (scene::DamageFragment& fragment : damage.fragments) {
                    fragment.linearVelocity += BlastPush(center, radius, fragment.pose.position);
                    result.fragments.push_back(std::move(fragment));
                }
            }
        }

        // 4. Push bodies outwards (the new pieces included) and wake what rested nearby.
        for (const auto [entity, body] : scene.Registry().view<const scene::RigidBody>().each()) {
            if (!physics.IsValid(body.body)) continue;
            const glm::vec3 push = BlastPush(center, radius, physics.GetState(body.body).position);
            if (push == glm::vec3(0.0f)) continue;
            physics.AddImpulse(body.body, push * physics.GetMass(body.body));
        }
        physics.ActivateInBox(math::AABB(center - glm::vec3(radius * 2.0f), center + glm::vec3(radius * 2.0f)));
        return result;
    }

    std::vector<BlastResult> RetryIslandSearches(Simulation& simulation) {
        std::vector<BlastResult> out;
        std::vector<DeferredIslandSearch>& deferred = simulation.DeferredIslandSearches();
        if (deferred.empty()) return out;
        const double vs = static_cast<double>(simulation.World().VoxelSize());
        constexpr double kInvQ = 1.0 / static_cast<double>(voxel::kQuantaPerVoxel);
        for (size_t i = 0; i < deferred.size();) {
            DeferredIslandSearch& search = deferred[i];
            BlastResult result;
            result.deferred = true;
            result.center = glm::dvec3(search.sphere.center) * (kInvQ * vs);
            result.radius = static_cast<float>(static_cast<double>(search.sphere.radius) * kInvQ * vs);
            result.islandsSearched = SearchIslands(simulation, search.sphere, result);
            if (result.islandsSearched) {
                if (!result.ops.empty()) out.push_back(std::move(result));
            } else if (++search.attempts < kIslandRetryLimit) {
                ++i;
                continue;
            }
            deferred.erase(deferred.begin() + static_cast<std::ptrdiff_t>(i));
        }
        return out;
    }

    void PresentBlast(Simulation& simulation, const BlastResult& blast) {
        voxel::World& world = simulation.World();
        voxel::MaterialLibrary& materials = simulation.Materials();
        fx::ParticleSystem& particles = simulation.Particles();
        scene::Scene& scene = simulation.Scene();
        const float vs = world.VoxelSize();
        const glm::dvec3 center = blast.center;
        const float radius = blast.radius;
        constexpr size_t kMaxBursts = 400;

        for (const AppliedOp& applied : blast.ops) {
            const std::vector<voxel::VoxelEdit>& removed = applied.edits;
            if (applied.op.kind == WorldOpKind::CarveSphere) {
                // Debris for a bounded sample of the removed voxels, in their material's colour.
                const size_t stride = std::max<size_t>(1, removed.size() / kMaxBursts);
                for (size_t i = 0; i < removed.size(); i += stride) {
                    const glm::dvec3 p = world.VoxelCenter(removed[i].voxel);
                    const glm::vec3 away = glm::normalize(glm::vec3(p - center) + glm::vec3(0.0f, 0.2f, 0.0f)) * 3.0f;
                    fx::SpawnVoxelBreak(particles, p, materials.Get(voxel::kWorldPalette, removed[i].oldId).color, vs, away);
                }
            } else if (applied.op.kind == WorldOpKind::Detach && applied.op.netId == 0) {
                // Terrain that broke off, made from the voxels this world just removed. Rubble:
                // small lumps are cleaned up by the scene.
                voxel::VoxelVolume piece = BuildDetachedPiece(applied.op, applied.edits);
                if (piece.IsEmpty()) continue;
                auto model = std::make_shared<voxel::VoxelModel>(std::move(piece), voxel::kWorldPalette, "debris");
                scene.CreateVoxelObject({.model = std::move(model),
                                         .position = glm::vec3(world.VoxelToWorld(applied.op.min)),
                                         .linearVelocity = applied.op.velocity,
                                         .debris = true});
            } else if (applied.op.kind == WorldOpKind::ClearMask) {
                // Crumbs: particles where the voxels were, as Scene::CreateVoxelObject crumbles a
                // piece under minPieceVoxels, but without a model per crumb.
                const size_t stride = std::max<size_t>({1, scene.Config().debrisParticleStride, (removed.size() + kMaxBursts - 1) / kMaxBursts});
                for (size_t i = 0; i < removed.size(); i += stride) {
                    const glm::dvec3 p = world.VoxelCenter(removed[i].voxel);
                    fx::SpawnVoxelBreak(particles, p, materials.Get(voxel::kWorldPalette, removed[i].oldId).color, vs,
                                        BlastPush(glm::vec3(center), radius, glm::vec3(p)));
                }
            }
        }
        if (blast.deferred) return;     // terrain that fell later: no second explosion, no flash
        fx::SpawnExplosion(particles, center, radius);

        // Object pieces below the replicated threshold (none offline unless it is set).
        for (const scene::DamageFragment& fragment : blast.fragments) {
            scene.CreateVoxelObject({.model = fragment.model,
                                     .position = fragment.pose.position,
                                     .rotation = fragment.pose.rotation,
                                     .linearVelocity = fragment.linearVelocity,
                                     .angularVelocity = fragment.angularVelocity,
                                     .debris = true});
        }

        // A flash of light that fades out quickly and removes itself once it is too dim to see.
        constexpr float kFlashDecay = 9.0f, kFlashCutoff = 0.05f;
        if (!(radius > 0.0f)) return;
        scene::LightDesc flash;
        flash.position = center;
        flash.color = glm::vec3(1.0f, 0.7f, 0.35f);
        flash.intensity = 40.0f * radius;
        flash.radius = radius * 8.0f;
        flash.decay = kFlashDecay;
        flash.lifetime = std::log(flash.intensity / kFlashCutoff) / kFlashDecay;
        if (flash.lifetime > 0.0f) scene.CreateLight(flash);
    }
}
