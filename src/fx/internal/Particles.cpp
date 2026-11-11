module;

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

module vortex.fx;

import vortex.core;

namespace vortex::fx {

    namespace {
        constexpr float kPi = 3.14159265358979f;
        constexpr float kMinLifetime = 1.0e-3f;        // randomised lifetimes never collapse to zero
        constexpr float kTangentialDamping = 0.8f;     // friction applied to the free axes on contact

        bool IsFinite(const glm::dvec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }
        bool IsFinite(const glm::vec3& v) { return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z); }

        /// Replaces a non-finite value by a fallback so one bad input cannot poison the pool.
        float Sanitize(float v, float fallback) { return std::isfinite(v) ? v : fallback; }

        struct Emitter {
            EmitterDesc desc;
            glm::dvec3 position{0.0};
            double carry = 0.0;          // fractional particles owed from previous updates
            uint32_t generation = 0;
            bool active = false;
            bool enabled = true;
        };

        struct SortKey {
            double distanceSq;
            uint32_t index;
        };
    }

    struct ParticleSystem::Impl {
        uint32_t capacity = 0;
        uint32_t alive = 0;
        bool warnedFull = false;
        Rng rng;

        // Structure of arrays, all sized to `capacity`; [0, alive) is live.
        std::vector<glm::dvec3> position;
        std::vector<glm::vec3> velocity;
        std::vector<glm::vec4> colorStart;
        std::vector<glm::vec4> colorEnd;
        std::vector<float> sizeStart;
        std::vector<float> sizeEnd;
        std::vector<float> age;
        std::vector<float> lifetime;
        std::vector<float> gravity;
        std::vector<float> drag;
        std::vector<float> bounce;
        std::vector<float> emissive;
        std::vector<uint32_t> flags;

        std::vector<Emitter> emitters;
        std::vector<uint32_t> freeEmitters;

        std::vector<SortKey> sortScratch;   // reused by BuildRenderData: no per-frame allocation

        Impl(uint32_t cap, uint64_t seed) : capacity(cap), rng(seed) {
            position.resize(cap);
            velocity.resize(cap);
            colorStart.resize(cap);
            colorEnd.resize(cap);
            sizeStart.resize(cap);
            sizeEnd.resize(cap);
            age.resize(cap);
            lifetime.resize(cap);
            gravity.resize(cap);
            drag.resize(cap);
            bounce.resize(cap);
            emissive.resize(cap);
            flags.resize(cap);
            sortScratch.reserve(cap);
        }

        bool Spawn(const ParticleDesc& desc) {
            if (alive >= capacity) {
                if (!warnedFull && capacity > 0) {    // a pool of 0 is "no particles" on purpose (a session server)
                    warnedFull = true;
                    Log::Warn("ParticleSystem: pool of {} particles is full, new particles are dropped", capacity);
                }
                return false;
            }
            if (!IsFinite(desc.position) || !IsFinite(desc.velocity) || !(desc.lifetime > 0.0f) ||
                !std::isfinite(desc.lifetime)) {
                return false;
            }
            const uint32_t i = alive++;
            position[i] = desc.position;
            velocity[i] = desc.velocity;
            colorStart[i] = desc.colorStart;
            colorEnd[i] = desc.colorEnd;
            sizeStart[i] = std::max(Sanitize(desc.sizeStart, 0.0f), 0.0f);
            sizeEnd[i] = std::max(Sanitize(desc.sizeEnd, 0.0f), 0.0f);
            age[i] = 0.0f;
            lifetime[i] = desc.lifetime;
            gravity[i] = Sanitize(desc.gravity, 0.0f);
            drag[i] = std::max(Sanitize(desc.drag, 0.0f), 0.0f);
            bounce[i] = std::clamp(Sanitize(desc.bounce, 0.0f), 0.0f, 1.0f);
            emissive[i] = std::max(Sanitize(desc.emissive, 0.0f), 0.0f);
            flags[i] = desc.flags;
            return true;
        }

        /// Swap-remove: the last live particle takes the place of `i`.
        void Remove(uint32_t i) {
            const uint32_t last = --alive;
            if (i == last) return;
            position[i] = position[last];
            velocity[i] = velocity[last];
            colorStart[i] = colorStart[last];
            colorEnd[i] = colorEnd[last];
            sizeStart[i] = sizeStart[last];
            sizeEnd[i] = sizeEnd[last];
            age[i] = age[last];
            lifetime[i] = lifetime[last];
            gravity[i] = gravity[last];
            drag[i] = drag[last];
            bounce[i] = bounce[last];
            emissive[i] = emissive[last];
            flags[i] = flags[last];
        }

        float Signed() { return rng.NextFloat() * 2.0f - 1.0f; }

        /// Builds one randomised particle from an emitter recipe. Always draws the same number
        /// of random values, so the stream stays in step whatever the recipe contains.
        ParticleDesc Randomise(const EmitterDesc& desc, const glm::dvec3& origin) {
            ParticleDesc p = desc.particle;

            // Direction: uniform over the spherical cap of half angle `spread` around the axis.
            glm::vec3 axis = desc.direction;
            const float axisLength = glm::length(axis);
            axis = (std::isfinite(axisLength) && axisLength > 1.0e-6f) ? axis / axisLength : glm::vec3(0.0f, 1.0f, 0.0f);
            const float cosMax = !(desc.spread < kPi) ? -1.0f : std::cos(std::max(desc.spread, 0.0f));
            const float cosTheta = 1.0f - rng.NextFloat() * (1.0f - cosMax);
            const float sinTheta = std::sqrt(std::max(0.0f, 1.0f - cosTheta * cosTheta));
            const float phi = 2.0f * kPi * rng.NextFloat();
            const glm::vec3 helper = std::abs(axis.y) < 0.99f ? glm::vec3(0.0f, 1.0f, 0.0f) : glm::vec3(1.0f, 0.0f, 0.0f);
            const glm::vec3 tangent = glm::normalize(glm::cross(helper, axis));
            const glm::vec3 bitangent = glm::cross(axis, tangent);
            const glm::vec3 direction =
                tangent * (std::cos(phi) * sinTheta) + bitangent * (std::sin(phi) * sinTheta) + axis * cosTheta;

            const float speed = rng.Range(desc.speedMin, desc.speedMax);
            p.velocity = direction * speed;

            const glm::vec3 offset(desc.positionJitter.x * Signed(), desc.positionJitter.y * Signed(),
                                   desc.positionJitter.z * Signed());
            p.position = origin + glm::dvec3(offset);

            p.lifetime = std::max(p.lifetime * (1.0f + desc.lifetimeJitter * Signed()), kMinLifetime);

            const float sizeScale = std::max(1.0f + desc.sizeJitter * Signed(), 0.0f);
            p.sizeStart *= sizeScale;
            p.sizeEnd *= sizeScale;

            const float brightness = std::max(1.0f + desc.colorJitter * Signed(), 0.0f);
            p.colorStart = glm::vec4(glm::vec3(p.colorStart) * brightness, p.colorStart.a);
            p.colorEnd = glm::vec4(glm::vec3(p.colorEnd) * brightness, p.colorEnd.a);
            return p;
        }

        uint32_t Burst(const EmitterDesc& desc, const glm::dvec3& origin, uint32_t count) {
            // A recipe with non-finite numbers can only produce particles that Spawn rejects:
            // refuse it up front instead of drawing `count` random particles for nothing.
            if (!std::isfinite(desc.particle.lifetime) || !std::isfinite(desc.speedMin) ||
                !std::isfinite(desc.speedMax) || !IsFinite(desc.positionJitter) ||
                !std::isfinite(desc.lifetimeJitter) || !std::isfinite(desc.sizeJitter) ||
                !std::isfinite(desc.colorJitter)) {
                return 0;
            }
            uint32_t spawned = 0;
            for (uint32_t n = 0; n < count; ++n) {
                if (alive >= capacity) {
                    Spawn(desc.particle);   // reports the full pool (once); spawns nothing
                    break;
                }
                if (Spawn(Randomise(desc, origin))) ++spawned;
            }
            return spawned;
        }

        Emitter* Find(EmitterId id) {
            if (!id.IsValid() || id.index >= emitters.size()) return nullptr;
            Emitter& e = emitters[id.index];
            return (e.active && e.generation == id.generation) ? &e : nullptr;
        }

        /// Moves particle `i` from `from` to `to` where `to` is inside solid matter.
        void Collide(uint32_t i, const glm::dvec3& from, const glm::dvec3& to, const SolidQuery& solid) {
            // Already embedded (spawned inside matter, or the world changed around it): there is
            // no free position to keep, so let it travel until it leaves the solid instead of
            // freezing it in place for the rest of its life.
            if (solid(from)) {
                position[i] = to;
                return;
            }

            // Which single-axis moves from the old position run into something?
            bool blocked[3] = {false, false, false};
            bool any = false;
            for (int a = 0; a < 3; ++a) {
                if (to[a] == from[a]) continue;
                glm::dvec3 probe = from;
                probe[a] = to[a];
                blocked[a] = solid(probe);
                any = any || blocked[a];
            }

            glm::dvec3 resolved = from;
            if (any) {
                for (int a = 0; a < 3; ++a) {
                    if (!blocked[a]) resolved[a] = to[a];
                }
            }
            // No single axis is blocked (the move cuts into an edge or corner), or the free axes
            // together still end inside matter: stay put and treat every moving axis as blocked.
            if (!any || (resolved != from && solid(resolved))) {
                resolved = from;
                for (int a = 0; a < 3; ++a) blocked[a] = blocked[a] || to[a] != from[a];
            }

            glm::vec3& v = velocity[i];
            for (int a = 0; a < 3; ++a) {
                if (blocked[a]) v[a] = -v[a] * bounce[i];
                else v[a] *= kTangentialDamping;
            }
            position[i] = resolved;
        }
    };

    ParticleSystem::ParticleSystem(uint32_t capacity, uint64_t seed) : m_Impl(std::make_unique<Impl>(capacity, seed)) {}

    ParticleSystem::~ParticleSystem() = default;

    bool ParticleSystem::Emit(const ParticleDesc& desc) { return m_Impl->Spawn(desc); }

    uint32_t ParticleSystem::Burst(const EmitterDesc& desc, const glm::dvec3& position, uint32_t count) {
        if (!IsFinite(position)) return 0;
        return m_Impl->Burst(desc, position, count);
    }

    EmitterId ParticleSystem::AddEmitter(const EmitterDesc& desc, const glm::dvec3& position) {
        Impl& s = *m_Impl;
        uint32_t index;
        if (!s.freeEmitters.empty()) {
            index = s.freeEmitters.back();
            s.freeEmitters.pop_back();
        } else {
            index = static_cast<uint32_t>(s.emitters.size());
            s.emitters.emplace_back();
        }
        Emitter& e = s.emitters[index];
        e.desc = desc;
        e.position = position;
        e.carry = 0.0;
        e.active = true;
        e.enabled = true;
        return EmitterId{index, e.generation};
    }

    void ParticleSystem::SetEmitterPosition(EmitterId id, const glm::dvec3& position) {
        if (Emitter* e = m_Impl->Find(id)) e->position = position;
    }

    void ParticleSystem::SetEmitterEnabled(EmitterId id, bool enabled) {
        if (Emitter* e = m_Impl->Find(id)) e->enabled = enabled;
    }

    void ParticleSystem::RemoveEmitter(EmitterId id) {
        Emitter* e = m_Impl->Find(id);
        if (!e) return;
        e->active = false;
        ++e->generation;   // every outstanding id of this slot is stale from now on
        m_Impl->freeEmitters.push_back(id.index);
    }

    bool ParticleSystem::IsEmitterValid(EmitterId id) const { return m_Impl->Find(id) != nullptr; }

    void ParticleSystem::Update(float dt, const SolidQuery& solid, float gravity) {
        if (!(dt > 0.0f) || !std::isfinite(dt)) return;
        Impl& s = *m_Impl;
        if (!std::isfinite(gravity)) gravity = 0.0f;
        const bool hasSolid = static_cast<bool>(solid);

        for (uint32_t i = 0; i < s.alive;) {
            s.age[i] += dt;
            if (s.age[i] >= s.lifetime[i]) {
                s.Remove(i);
                continue;   // the particle swapped into slot i still needs its update
            }

            glm::vec3& v = s.velocity[i];
            v.y += gravity * s.gravity[i] * dt;
            v *= std::exp(-s.drag[i] * dt);

            const glm::dvec3 from = s.position[i];
            const glm::dvec3 to = from + glm::dvec3(v) * static_cast<double>(dt);

            if (hasSolid && (s.flags[i] & ParticleFlag_Collide) != 0 && solid(to)) {
                if ((s.flags[i] & ParticleFlag_DieOnCollide) != 0) {
                    s.Remove(i);
                    continue;
                }
                s.Collide(i, from, to, solid);
            } else {
                s.position[i] = to;
            }
            ++i;
        }

        // Continuous emitters, in slot order (deterministic). New particles start at age 0.
        for (Emitter& e : s.emitters) {
            if (!e.active || !e.enabled) continue;
            const float rate = e.desc.rate;
            if (!(rate > 0.0f) || !std::isfinite(rate) || !IsFinite(e.position)) continue;
            e.carry += static_cast<double>(rate) * static_cast<double>(dt);
            const double whole = std::floor(e.carry);
            e.carry -= whole;
            // What does not fit in the pool is dropped, not owed: no burst when room appears.
            const double room = static_cast<double>(s.capacity - s.alive);
            const uint32_t count = static_cast<uint32_t>(std::min(whole, room));
            if (count > 0) s.Burst(e.desc, e.position, count);
        }
    }

    uint32_t ParticleSystem::AliveCount() const { return m_Impl->alive; }

    uint32_t ParticleSystem::Capacity() const { return m_Impl->capacity; }

    void ParticleSystem::Clear() { m_Impl->alive = 0; }

    void ParticleSystem::BuildRenderData(const glm::dvec3& renderOrigin, const glm::dvec3& cameraPosition,
                                         std::vector<GpuParticle>& out) const {
        Impl& s = *m_Impl;
        out.clear();
        if (s.alive == 0) return;

        std::vector<SortKey>& keys = s.sortScratch;
        keys.clear();
        for (uint32_t i = 0; i < s.alive; ++i) {
            const glm::dvec3 d = s.position[i] - cameraPosition;
            keys.push_back(SortKey{glm::dot(d, d), i});
        }
        // Back to front; the index breaks ties so the order is fully deterministic.
        std::sort(keys.begin(), keys.end(), [](const SortKey& a, const SortKey& b) {
            if (a.distanceSq != b.distanceSq) return a.distanceSq > b.distanceSq;
            return a.index < b.index;
        });

        out.reserve(s.alive);
        for (const SortKey& key : keys) {
            const uint32_t i = key.index;
            const float t = std::clamp(s.age[i] / s.lifetime[i], 0.0f, 1.0f);
            const uint32_t flags = s.flags[i];

            GpuParticle g;
            const float size = glm::mix(s.sizeStart[i], s.sizeEnd[i], t);
            g.positionSize = glm::vec4(glm::vec3(s.position[i] - renderOrigin), 0.5f * size);

            glm::vec4 color = glm::mix(s.colorStart[i], s.colorEnd[i], t);
            float emissive = s.emissive[i];
            if ((flags & ParticleFlag_Additive) != 0) {
                // Pure light: the opacity becomes the intensity of the light, so the particle
                // still fades out, and the blend never darkens what is behind it.
                color = glm::vec4(glm::vec3(color) * std::clamp(color.a, 0.0f, 1.0f), 0.0f);
                emissive = std::max(emissive, 1.0f);
            }
            g.color = color;
            g.params = glm::vec4(emissive, (flags & ParticleFlag_SoftDisc) != 0 ? 1.0f : 0.0f,
                                 (flags & ParticleFlag_Unlit) != 0 ? 1.0f : 0.0f, 0.0f);
            out.push_back(g);
        }
    }
}
