module;

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <cstdint>

module vortex.fx;

namespace vortex::fx {

    namespace {
        constexpr float kPi = 3.14159265358979f;

        glm::vec3 SafeColor(const glm::vec3& c) {
            return glm::vec3(std::isfinite(c.x) ? std::max(c.x, 0.0f) : 1.0f,
                             std::isfinite(c.y) ? std::max(c.y, 0.0f) : 1.0f,
                             std::isfinite(c.z) ? std::max(c.z, 0.0f) : 1.0f);
        }

        /// base + perUnit * amount, rounded and clamped to [1, cap]: effect sizes never run away.
        uint32_t ScaledCount(float base, float perUnit, float amount, uint32_t cap) {
            const float n = std::round(base + perUnit * amount);
            return static_cast<uint32_t>(std::clamp(n, 1.0f, static_cast<float>(cap)));
        }
    }

    namespace effects {

        EmitterDesc Debris(const glm::vec3& color, float voxelSize) {
            const float size = (std::isfinite(voxelSize) && voxelSize > 0.0f) ? voxelSize : 0.1f;
            const glm::vec3 c = SafeColor(color);
            EmitterDesc d;
            d.particle.colorStart = glm::vec4(c, 1.0f);
            d.particle.colorEnd = glm::vec4(c, 1.0f);          // chips stay solid and shrink away
            d.particle.sizeStart = size * 0.45f;
            d.particle.sizeEnd = size * 0.12f;
            d.particle.lifetime = 1.6f;
            d.particle.gravity = 1.0f;
            d.particle.drag = 0.4f;
            d.particle.bounce = 0.35f;
            d.particle.emissive = 0.0f;
            d.particle.flags = ParticleFlag_Collide;
            d.positionJitter = glm::vec3(size * 0.4f);
            d.direction = glm::vec3(0.0f, 1.0f, 0.0f);
            d.spread = 1.2f;
            d.speedMin = 1.2f;
            d.speedMax = 3.5f;
            d.lifetimeJitter = 0.35f;
            d.sizeJitter = 0.4f;
            d.colorJitter = 0.12f;
            d.rate = 0.0f;
            return d;
        }

        EmitterDesc Dust(const glm::vec3& color) {
            // Airborne dust is paler than the material it came from.
            const glm::vec3 c = glm::mix(SafeColor(color), glm::vec3(0.6f), 0.35f);
            EmitterDesc d;
            d.particle.colorStart = glm::vec4(c, 0.35f);
            d.particle.colorEnd = glm::vec4(c, 0.0f);
            d.particle.sizeStart = 0.22f;
            d.particle.sizeEnd = 0.65f;
            d.particle.lifetime = 1.4f;
            d.particle.gravity = -0.02f;
            d.particle.drag = 2.5f;
            d.particle.bounce = 0.0f;
            d.particle.emissive = 0.0f;
            d.particle.flags = ParticleFlag_SoftDisc;
            d.positionJitter = glm::vec3(0.08f);
            d.direction = glm::vec3(0.0f, 1.0f, 0.0f);
            d.spread = kPi;
            d.speedMin = 0.2f;
            d.speedMax = 0.9f;
            d.lifetimeJitter = 0.3f;
            d.sizeJitter = 0.3f;
            d.colorJitter = 0.08f;
            d.rate = 0.0f;
            return d;
        }

        EmitterDesc Smoke() {
            EmitterDesc d;
            d.particle.colorStart = glm::vec4(0.05f, 0.05f, 0.055f, 0.55f);
            d.particle.colorEnd = glm::vec4(0.16f, 0.16f, 0.17f, 0.0f);
            d.particle.sizeStart = 0.35f;
            d.particle.sizeEnd = 1.4f;
            d.particle.lifetime = 3.0f;
            d.particle.gravity = -0.12f;      // buoyant: settles at about 1 m/s upwards against the drag
            d.particle.drag = 1.2f;
            d.particle.bounce = 0.0f;
            d.particle.emissive = 0.0f;
            d.particle.flags = ParticleFlag_SoftDisc;
            d.positionJitter = glm::vec3(0.15f);
            d.direction = glm::vec3(0.0f, 1.0f, 0.0f);
            d.spread = 0.6f;
            d.speedMin = 0.4f;
            d.speedMax = 1.2f;
            d.lifetimeJitter = 0.3f;
            d.sizeJitter = 0.3f;
            d.colorJitter = 0.15f;
            d.rate = 0.0f;
            return d;
        }

        EmitterDesc FireBurst() {
            EmitterDesc d;
            d.particle.colorStart = glm::vec4(1.0f, 0.62f, 0.2f, 1.0f);
            d.particle.colorEnd = glm::vec4(0.9f, 0.12f, 0.02f, 0.0f);
            d.particle.sizeStart = 0.3f;
            d.particle.sizeEnd = 0.55f;
            d.particle.lifetime = 0.55f;
            d.particle.gravity = -0.15f;
            d.particle.drag = 3.0f;
            d.particle.bounce = 0.0f;
            d.particle.emissive = 6.0f;
            d.particle.flags = ParticleFlag_Additive | ParticleFlag_SoftDisc | ParticleFlag_Unlit;
            d.positionJitter = glm::vec3(0.1f);
            d.direction = glm::vec3(0.0f, 1.0f, 0.0f);
            d.spread = kPi;
            d.speedMin = 1.5f;
            d.speedMax = 6.0f;
            d.lifetimeJitter = 0.3f;
            d.sizeJitter = 0.3f;
            d.colorJitter = 0.15f;
            d.rate = 0.0f;
            return d;
        }

        EmitterDesc Sparks(const glm::vec3& color) {
            const glm::vec3 c = SafeColor(color);
            EmitterDesc d;
            d.particle.colorStart = glm::vec4(c, 1.0f);
            d.particle.colorEnd = glm::vec4(c * glm::vec3(1.0f, 0.4f, 0.15f), 0.0f);   // cools towards red
            d.particle.sizeStart = 0.03f;
            d.particle.sizeEnd = 0.012f;
            d.particle.lifetime = 0.9f;
            d.particle.gravity = 1.0f;
            d.particle.drag = 0.6f;
            d.particle.bounce = 0.5f;
            d.particle.emissive = 10.0f;
            d.particle.flags = ParticleFlag_Additive | ParticleFlag_Collide | ParticleFlag_Unlit;
            d.positionJitter = glm::vec3(0.03f);
            d.direction = glm::vec3(0.0f, 1.0f, 0.0f);
            d.spread = kPi;
            d.speedMin = 3.0f;
            d.speedMax = 9.0f;
            d.lifetimeJitter = 0.4f;
            d.sizeJitter = 0.3f;
            d.colorJitter = 0.2f;
            d.rate = 0.0f;
            return d;
        }

        EmitterDesc Flame() {
            EmitterDesc d;
            d.particle.colorStart = glm::vec4(1.0f, 0.55f, 0.15f, 1.0f);
            d.particle.colorEnd = glm::vec4(0.8f, 0.1f, 0.02f, 0.0f);
            d.particle.sizeStart = 0.14f;
            d.particle.sizeEnd = 0.03f;
            d.particle.lifetime = 0.7f;
            d.particle.gravity = -0.25f;
            d.particle.drag = 1.5f;
            d.particle.bounce = 0.0f;
            d.particle.emissive = 4.0f;
            d.particle.flags = ParticleFlag_Additive | ParticleFlag_SoftDisc | ParticleFlag_Unlit;
            d.positionJitter = glm::vec3(0.05f, 0.01f, 0.05f);
            d.direction = glm::vec3(0.0f, 1.0f, 0.0f);
            d.spread = 0.25f;
            d.speedMin = 0.4f;
            d.speedMax = 1.0f;
            d.lifetimeJitter = 0.3f;
            d.sizeJitter = 0.3f;
            d.colorJitter = 0.15f;
            d.rate = 40.0f;
            return d;
        }

        EmitterDesc Magic(const glm::vec3& color) {
            const glm::vec3 c = SafeColor(color);
            EmitterDesc d;
            d.particle.colorStart = glm::vec4(c, 1.0f);
            d.particle.colorEnd = glm::vec4(c, 0.0f);
            d.particle.sizeStart = 0.05f;
            d.particle.sizeEnd = 0.01f;
            d.particle.lifetime = 1.4f;
            d.particle.gravity = -0.03f;
            d.particle.drag = 1.0f;
            d.particle.bounce = 0.0f;
            d.particle.emissive = 5.0f;
            d.particle.flags = ParticleFlag_Additive | ParticleFlag_SoftDisc | ParticleFlag_Unlit;
            d.positionJitter = glm::vec3(0.25f);
            d.direction = glm::vec3(0.0f, 1.0f, 0.0f);
            d.spread = kPi;
            d.speedMin = 0.1f;
            d.speedMax = 0.5f;
            d.lifetimeJitter = 0.4f;
            d.sizeJitter = 0.4f;
            d.colorJitter = 0.2f;
            d.rate = 20.0f;
            return d;
        }

        EmitterDesc Precipitation(bool snow) {
            // Place the emitter some metres above the viewer: particles spawn in a wide flat
            // box around it and die on the first solid voxel they touch.
            EmitterDesc d;
            d.particle.bounce = 0.0f;
            d.particle.emissive = 0.0f;
            d.direction = glm::vec3(0.0f, -1.0f, 0.0f);
            d.positionJitter = glm::vec3(12.0f, 1.5f, 12.0f);
            if (snow) {
                d.particle.colorStart = glm::vec4(0.95f, 0.96f, 1.0f, 0.9f);
                d.particle.colorEnd = glm::vec4(0.95f, 0.96f, 1.0f, 0.9f);
                d.particle.sizeStart = 0.04f;
                d.particle.sizeEnd = 0.04f;
                d.particle.lifetime = 9.0f;
                d.particle.gravity = 0.3f;        // terminal speed g * 0.3 / drag = about 1 m/s
                d.particle.drag = 3.0f;
                d.particle.flags = ParticleFlag_Collide | ParticleFlag_DieOnCollide | ParticleFlag_SoftDisc;
                d.spread = 0.5f;
                d.speedMin = 0.6f;
                d.speedMax = 1.2f;
                d.lifetimeJitter = 0.1f;
                d.sizeJitter = 0.4f;
                d.colorJitter = 0.05f;
                d.rate = 250.0f;
            } else {
                d.particle.colorStart = glm::vec4(0.6f, 0.7f, 0.85f, 0.45f);
                d.particle.colorEnd = glm::vec4(0.6f, 0.7f, 0.85f, 0.45f);
                d.particle.sizeStart = 0.03f;
                d.particle.sizeEnd = 0.03f;
                d.particle.lifetime = 2.5f;
                d.particle.gravity = 1.0f;        // terminal speed g / drag = about 9 m/s
                d.particle.drag = 1.1f;
                d.particle.flags = ParticleFlag_Collide | ParticleFlag_DieOnCollide;
                d.spread = 0.05f;
                d.speedMin = 8.0f;
                d.speedMax = 10.0f;
                d.lifetimeJitter = 0.1f;
                d.sizeJitter = 0.2f;
                d.colorJitter = 0.08f;
                d.rate = 600.0f;
            }
            return d;
        }
    }

    void SpawnExplosion(ParticleSystem& particles, const glm::dvec3& position, float radius) {
        if (!std::isfinite(radius) || !(radius > 0.0f)) return;
        // The visual scale saturates: a huge blast gets more of the screen, not bigger sprites
        // or an unbounded number of them.
        const float r = std::clamp(radius, 0.1f, 8.0f);

        // Fireball: fills the radius. With drag 3 a particle travels about 0.27 * v in its life.
        EmitterDesc fire = effects::FireBurst();
        fire.particle.sizeStart *= r;
        fire.particle.sizeEnd *= r;
        fire.positionJitter = glm::vec3(0.2f * r);
        fire.speedMin = 1.0f * r;
        fire.speedMax = 3.7f * r;
        particles.Burst(fire, position, ScaledCount(16.0f, 28.0f, r * r, 400));

        // Sparks: thrown well past the fireball.
        EmitterDesc sparks = effects::Sparks();
        const float throwScale = std::sqrt(r);
        sparks.positionJitter = glm::vec3(0.15f * r);
        sparks.speedMin *= throwScale;
        sparks.speedMax *= throwScale;
        sparks.particle.lifetime *= std::min(throwScale, 2.0f);
        particles.Burst(sparks, position, ScaledCount(12.0f, 24.0f, r, 200));

        // Smoke: what is left behind, drifting up.
        EmitterDesc smoke = effects::Smoke();
        smoke.particle.sizeStart *= r;
        smoke.particle.sizeEnd *= r;
        smoke.particle.lifetime *= 1.0f + 0.15f * r;
        smoke.positionJitter = glm::vec3(0.4f * r);
        smoke.spread = 1.4f;
        smoke.speedMin = 0.3f * r;
        smoke.speedMax = 1.0f * r;
        particles.Burst(smoke, position, ScaledCount(8.0f, 10.0f, r * r, 200));
    }

    void SpawnVoxelBreak(ParticleSystem& particles, const glm::dvec3& voxelCenter, const glm::vec3& color,
                         float voxelSize, const glm::vec3& impulse) {
        const float size = (std::isfinite(voxelSize) && voxelSize > 0.0f) ? voxelSize : 0.1f;
        const float scale = std::clamp(size / 0.1f, 0.25f, 4.0f);   // relative to the reference voxel

        float push = glm::length(impulse);
        if (!std::isfinite(push)) push = 0.0f;
        const float kick = std::min(push, 20.0f);

        EmitterDesc debris = effects::Debris(color, size);
        if (push > 1.0e-4f) {
            // Chips fly along the hit, lifted a little so they arc instead of diving.
            debris.direction = impulse / push + glm::vec3(0.0f, 0.35f, 0.0f);
            debris.spread = 0.7f;
            debris.speedMin += 0.3f * kick;
            debris.speedMax += kick;
        }
        particles.Burst(debris, voxelCenter, ScaledCount(4.0f, 2.0f, scale, 12));

        EmitterDesc dust = effects::Dust(color);
        dust.particle.sizeStart *= scale;
        dust.particle.sizeEnd *= scale;
        dust.positionJitter = glm::vec3(size * 0.5f);
        particles.Burst(dust, voxelCenter, ScaledCount(1.0f, 1.0f, scale, 4));
    }
}
