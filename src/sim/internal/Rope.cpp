module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <utility>
#include <vector>

module vortex.sim;

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.physics;
import vortex.scene;

namespace vortex::sim {

    namespace {
        /// A point quantised to 1/16 voxel and back to metres (the cut tests decide on these, as CarveSphereQ does).
        glm::dvec3 Quantised(const glm::dvec3& p, double voxelSize) {
            const double scale = 16.0 / voxelSize;
            return glm::dvec3(std::llround(p.x * scale), std::llround(p.y * scale), std::llround(p.z * scale)) / scale;
        }

        /// The world point of a load's model point `attachVoxel` (current model voxels).
        bool LoadPoint(const scene::Scene& scene, scene::Entity load, const glm::vec3& attachVoxel, float voxelSize, glm::dvec3& out) {
            if (!scene.IsValid(load)) return false;
            const scene::Transform* t = scene.TryGet<scene::Transform>(load);
            if (!t) return false;
            out = glm::dvec3(t->current.position) + glm::dvec3(t->current.rotation * (attachVoxel * voxelSize));
            return true;
        }

        /// The closest points of two segments: parameters s on [p0, p1] and t on [q0, q1].
        void ClosestOnSegments(const glm::dvec3& p0, const glm::dvec3& p1, const glm::dvec3& q0, const glm::dvec3& q1, double& s, double& t) {
            const glm::dvec3 d1 = p1 - p0, d2 = q1 - q0, r = p0 - q0;
            const double a = glm::dot(d1, d1), e = glm::dot(d2, d2), f = glm::dot(d2, r);
            if (a <= 1e-12 && e <= 1e-12) {
                s = t = 0.0;
                return;
            }
            if (a <= 1e-12) {
                s = 0.0;
                t = std::clamp(f / e, 0.0, 1.0);
                return;
            }
            const double c = glm::dot(d1, r);
            if (e <= 1e-12) {
                t = 0.0;
                s = std::clamp(-c / a, 0.0, 1.0);
                return;
            }
            const double b = glm::dot(d1, d2), denom = a * e - b * b;
            s = denom > 1e-12 ? std::clamp((b * f - c * e) / denom, 0.0, 1.0) : 0.0;
            t = (b * s + f) / e;
            if (t < 0.0) {
                t = 0.0;
                s = std::clamp(-c / a, 0.0, 1.0);
            } else if (t > 1.0) {
                t = 1.0;
                s = std::clamp((b - c) / a, 0.0, 1.0);
            }
        }

        bool Solid(const voxel::VoxelVolume& volume, const glm::ivec3& v) {
            const glm::ivec3 size = volume.Size();
            return v.x >= 0 && v.y >= 0 && v.z >= 0 && v.x < size.x && v.y < size.y && v.z < size.z && volume.Get(v.x, v.y, v.z) != 0;
        }
    }

    glm::dvec3 RopeAnchorPoint(const voxel::World& world, const glm::ivec3& anchorVoxel) {
        const double vs = world.VoxelSize();
        return (glm::dvec3(anchorVoxel) + glm::dvec3(0.5, 0.0, 0.5)) * vs;
    }

    float RopeStubLength(const Simulation& simulation, const Rope& rope) {
        if (!rope.cut) return 0.0f;
        const voxel::World& world = simulation.World();
        if (world.IsChunkLoaded(voxel::VoxelToChunk(rope.anchorVoxel)) && world.GetVoxel(rope.anchorVoxel) == 0) return 0.0f;
        return rope.cutFraction * rope.length;
    }

    std::vector<scene::Entity> RopesInOrder(const scene::Scene& scene) {
        std::vector<scene::Entity> out;
        for (const scene::Entity e : scene.Registry().view<const scene::Rope>()) out.push_back(e);
        std::sort(out.begin(), out.end(), [](scene::Entity a, scene::Entity b) { return static_cast<uint32_t>(a) < static_cast<uint32_t>(b); });
        return out;
    }

    scene::Entity CreateLoadRope(Simulation& simulation, const glm::ivec3& anchorVoxel, scene::Entity load, const glm::vec3& attachVoxel,
                                 uint8_t flags) {
        scene::Scene& scene = simulation.Scene();
        const scene::RigidBody* rigid = scene.IsValid(load) ? scene.TryGet<scene::RigidBody>(load) : nullptr;
        if (!rigid || rigid->motion != physics::MotionType::Dynamic) return scene::kNoEntity;
        const float vs = simulation.World().VoxelSize();
        const glm::dvec3 anchor = RopeAnchorPoint(simulation.World(), anchorVoxel);
        glm::dvec3 attach;
        if (!LoadPoint(scene, load, attachVoxel, vs, attach)) return scene::kNoEntity;
        const float length = static_cast<float>(glm::length(attach - anchor));
        const physics::ConstraintId constraint =
            simulation.Physics().CreateDistanceConstraint(glm::vec3(anchor), rigid->body, glm::vec3(attach), 0.0f, length);
        if (!constraint.IsValid()) return scene::kNoEntity;
        scene::Rope rope;
        rope.anchorVoxel = anchorVoxel;
        rope.load = load;
        rope.attach = attachVoxel;
        rope.length = length;
        rope.flags = static_cast<uint8_t>(flags & ~scene::RopeFlag_Chain);
        rope.constraint = constraint;
        rope.body = rigid->body;
        rope.lastAttach = attach;
        const scene::Entity e = scene.Registry().create();
        scene.Registry().emplace<scene::Rope>(e, rope);
        return e;
    }

    scene::Entity CreateChainRope(Simulation& simulation, const glm::ivec3& anchorVoxel, scene::Entity mechanism, const glm::vec3& attachVoxel) {
        scene::Scene& scene = simulation.Scene();
        if (!scene.IsValid(mechanism) || !scene.TryGet<scene::Mechanism>(mechanism)) return scene::kNoEntity;
        scene::Rope rope;
        rope.anchorVoxel = anchorVoxel;
        rope.load = mechanism;
        rope.attach = attachVoxel;
        rope.flags = scene::RopeFlag_Chain;
        const scene::Entity e = scene.Registry().create();
        scene.Registry().emplace<scene::Rope>(e, rope);
        glm::dvec3 a, b;
        if (RopeEnds(simulation, e, a, b)) {
            scene::Rope& made = scene.Registry().get<scene::Rope>(e);
            made.length = static_cast<float>(glm::length(b - a));
            made.lastAttach = b;
        }
        return e;
    }

    bool RopeEnds(const Simulation& simulation, scene::Entity rope, glm::dvec3& anchor, glm::dvec3& attach) {
        const scene::Scene& scene = simulation.Scene();
        const scene::Rope* r = scene.TryGet<scene::Rope>(rope);
        if (!r) return false;
        const float vs = simulation.World().VoxelSize();
        anchor = RopeAnchorPoint(simulation.World(), r->anchorVoxel);
        if ((r->flags & scene::RopeFlag_Chain) != 0) {
            const scene::Mechanism* m = scene.IsValid(r->load) ? scene.TryGet<scene::Mechanism>(r->load) : nullptr;
            if (!m) return false;
            const MechanismPose pose = PoseAt(*m, PositionAt(*m, simulation.Tick()), vs);
            attach = pose.position + glm::dvec3(pose.rotation * ((r->attach - glm::vec3(m->modelOffset)) * vs));
            return true;
        }
        return LoadPoint(scene, r->load, r->attach, vs, attach);
    }

    void CutRope(Simulation& simulation, scene::Entity rope, float fraction, RopeCutCause cause, uint32_t tick, std::vector<RopeCutEvent>* events) {
        scene::Scene& scene = simulation.Scene();
        scene::Rope* r = scene.TryGet<scene::Rope>(rope);
        if (!r || r->cut) return;
        simulation.Physics().DestroyConstraint(r->constraint);
        r->constraint = physics::ConstraintId{};
        r->cut = true;
        r->cutFraction = std::clamp(fraction, 0.0f, 1.0f);
        r->cutTick = tick;
        if (const scene::RigidBody* rigid = scene.IsValid(r->load) ? scene.TryGet<scene::RigidBody>(r->load) : nullptr)
            simulation.Physics().Activate(rigid->body);
        if (events) events->push_back({rope, tick, r->cutFraction, cause});

        // A cut chain: its portcullis drops at its drop speed and jams shut.
        if ((r->flags & scene::RopeFlag_Chain) != 0 && scene.IsValid(r->load)) {
            if (scene::Mechanism* m = scene.TryGet<scene::Mechanism>(r->load); m && m->track.count > 0) {
                const MechanismSegment newest = m->track.Newest();
                const uint32_t start = std::max(tick + 1, newest.startTick + 1);
                const int16_t p0 = PositionAt(*m, start);
                PushSegment(m->track, {start, p0, m->desc.minPos, SegmentDuration(p0, m->desc.minPos, m->desc),
                                       static_cast<uint8_t>(newest.flags | scene::MechanismFlag_Jammed)});
            }
        }
    }

    uint32_t CutRopesAlongRay(Simulation& simulation, const glm::dvec3& eye, const glm::vec3& dir, float maxDistance, uint32_t tick,
                              std::vector<RopeCutEvent>* events) {
        const double vs = simulation.World().VoxelSize();
        const glm::dvec3 q0 = Quantised(eye, vs);
        const glm::dvec3 q1 = Quantised(eye + glm::dvec3(glm::normalize(dir)) * static_cast<double>(maxDistance), vs);
        const double reach = static_cast<double>(kRopeRadius + kRopeAimForgiveness);
        std::vector<std::pair<double, std::pair<scene::Entity, double>>> hits;   // (distance along the ray, (rope, fraction))
        for (const scene::Entity e : RopesInOrder(simulation.Scene())) {
            const scene::Rope& r = simulation.Scene().Registry().get<scene::Rope>(e);
            glm::dvec3 a, b;
            if (r.cut || !RopeEnds(simulation, e, a, b)) continue;
            a = Quantised(a, vs);
            b = Quantised(b, vs);
            double s = 0.0, t = 0.0;
            ClosestOnSegments(a, b, q0, q1, s, t);
            const glm::dvec3 pr = a + (b - a) * s, pq = q0 + (q1 - q0) * t;
            if (glm::length(pr - pq) <= reach) hits.push_back({t, {e, s}});
        }
        std::sort(hits.begin(), hits.end(), [](const auto& x, const auto& y) {
            return x.first < y.first || (x.first == y.first && static_cast<uint32_t>(x.second.first) < static_cast<uint32_t>(y.second.first));
        });
        uint32_t cut = 0;
        for (const auto& hit : hits) {
            if (cut == kMaxRopeCutsPerCast) break;
            CutRope(simulation, hit.second.first, static_cast<float>(hit.second.second), RopeCutCause::Ray, tick, events);
            ++cut;
        }
        return cut;
    }

    uint32_t CutRopesBySphere(Simulation& simulation, const voxel::SphereQ& sphere, uint32_t tick, std::vector<RopeCutEvent>* events) {
        const double vs = simulation.World().VoxelSize();
        const glm::dvec3 centre = glm::dvec3(sphere.center) / 16.0 * vs;
        const double radius = static_cast<double>(sphere.radius) / 16.0 * vs + static_cast<double>(kRopeRadius);
        uint32_t cut = 0;
        for (const scene::Entity e : RopesInOrder(simulation.Scene())) {
            const scene::Rope& r = simulation.Scene().Registry().get<scene::Rope>(e);
            glm::dvec3 a, b;
            if (r.cut || !RopeEnds(simulation, e, a, b)) continue;
            a = Quantised(a, vs);
            b = Quantised(b, vs);
            const glm::dvec3 ab = b - a;
            const double len2 = glm::dot(ab, ab);
            const double s = len2 > 1e-12 ? std::clamp(glm::dot(centre - a, ab) / len2, 0.0, 1.0) : 0.0;
            if (glm::length(a + ab * s - centre) > radius) continue;
            CutRope(simulation, e, static_cast<float>(s), RopeCutCause::Sphere, tick, events);
            ++cut;
        }
        return cut;
    }

    void StepRopes(Simulation& simulation, uint32_t tick, std::vector<RopeCutEvent>* events) {
        scene::Scene& scene = simulation.Scene();
        voxel::World& world = simulation.World();
        const float vs = world.VoxelSize();
        for (const scene::Entity e : RopesInOrder(scene)) {
            scene::Rope* r = scene.TryGet<scene::Rope>(e);
            if (!r || r->cut) continue;
            // The anchor turned to air (a loaded chunk only: an unloaded one is not known to be air).
            if (world.IsChunkLoaded(voxel::VoxelToChunk(r->anchorVoxel)) && world.GetVoxel(r->anchorVoxel) == 0) {
                CutRope(simulation, e, 0.0f, RopeCutCause::Anchor, tick, events);
                continue;
            }
            // The load or the mechanism is gone.
            const bool chain = (r->flags & scene::RopeFlag_Chain) != 0;
            const bool present = scene.IsValid(r->load) && (chain ? scene.TryGet<scene::Mechanism>(r->load) != nullptr
                                                                  : scene.TryGet<scene::RigidBody>(r->load) != nullptr);
            if (!present) {
                CutRope(simulation, e, 1.0f, RopeCutCause::LoadGone, tick, events);
                continue;
            }
            if (chain) continue;
            // Scene::Damage re-made the load's body: the rope holds on when the attach point still lies in its model.
            const scene::RigidBody& rigid = scene.Registry().get<scene::RigidBody>(r->load);
            if (rigid.body != r->body || !simulation.Physics().IsValid(r->constraint)) {
                const scene::Transform* t = scene.TryGet<scene::Transform>(r->load);
                const scene::VoxelBody* v = scene.TryGet<scene::VoxelBody>(r->load);
                bool held = false;
                if (t && v && v->model) {
                    const glm::vec3 local = glm::conjugate(t->current.rotation) * (glm::vec3(r->lastAttach) - t->current.position) / vs;
                    if (Solid(v->model->Volume(), glm::ivec3(glm::floor(local)))) {
                        const glm::dvec3 anchor = RopeAnchorPoint(world, r->anchorVoxel);
                        r->constraint = simulation.Physics().CreateDistanceConstraint(glm::vec3(anchor), rigid.body, glm::vec3(r->lastAttach),
                                                                                       0.0f, r->length);
                        r->attach = local;
                        r->body = rigid.body;
                        held = r->constraint.IsValid();
                    }
                }
                if (!held) {
                    CutRope(simulation, e, 1.0f, RopeCutCause::LoadGone, tick, events);
                    continue;
                }
            }
            glm::dvec3 a, b;
            if (RopeEnds(simulation, e, a, b)) r->lastAttach = b;
        }
    }

    math::AABB RopeCleatBox(const voxel::World& world, const Rope& rope) {
        const float vs = world.VoxelSize();
        const glm::vec3 lo = glm::vec3(rope.anchorVoxel - glm::ivec3(2)) * vs;
        const glm::vec3 hi = glm::vec3(rope.anchorVoxel + glm::ivec3(3)) * vs;
        return math::AABB(lo, hi);
    }

    UseResult ReleaseRope(Simulation& simulation, scene::Entity rope, uint32_t tick, std::vector<RopeCutEvent>* events) {
        scene::Rope* r = simulation.Scene().TryGet<scene::Rope>(rope);
        if (!r || r->cut || (r->flags & scene::RopeFlag_Cleat) == 0) return UseResult::NoTarget;
        if (r->anyDecision && tick - r->lastDecisionTick < kRopeBusyTicks) return UseResult::Busy;
        r->anyDecision = true;
        r->lastDecisionTick = tick;
        CutRope(simulation, rope, 0.0f, RopeCutCause::Released, tick, events);
        return UseResult::None;
    }
}
