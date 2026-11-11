// =============================================================================
//  Shading building blocks - shared by the GPU shaders and the C++ engine.
// =============================================================================
//  (Written in the GLSL/C++ common subset, see shared/vx_cpp.h: `f` suffixed literals, no
//  swizzles, VX_OUT / VX_INOUT, VX_CONST_* constants, no global state - every input is a
//  parameter, so the CPU reference renderer compiles this very file.)
//
//  All colours are linear RGB, all directions are unit vectors unless stated otherwise.
//
//  Random numbers
//    uint  VxHashPcg(v)                         PCG integer hash (one round)
//    uint  VxRandomSeed(x, y, frame)            per-pixel, per-frame generator state
//    float VxRandom(state)                      next float in [0, 1), advances the state
//
//  Sampling
//    void  VxOrthonormalBasis(n, t, b)          tangent frame around a unit vector
//    vec3  VxCosineHemisphere(n, u1, u2)        cosine-weighted direction around a normal
//    vec3  VxConeDirection(axis, tanR, u1, u2)  direction inside a cone (soft shadows)
//
//  Environment
//    vec3  VxSkyRadiance(dir, sunDir, sunTanRadius, sunRadiance, zenith, horizon, ground,
//                        sunAmount)             gradient sky + sun disc and glow
//                                               (sunAmount 0 = gradient only, 1 = with sun)
//    float VxFogAmount(ro, rd, dist, density, heightFalloff, baseHeight, startDistance,
//                      maxOpacity)              opacity (0..maxOpacity) of exponential height
//                                               fog along the segment ro .. ro + rd * dist
//
//  Surfaces and lights
//    vec3  VxAlbedo(color, variationAmount, variation)   material colour with per-voxel jitter
//    float VxSpecular(n, v, l, roughness)       normalised Blinn-Phong lobe times N.L
//    float VxLightFalloff(dist, radius)         inverse-square falloff windowed to reach
//                                               exactly zero at `radius`
//    float VxLuminance(c)                       Rec.709 luminance
//
//  Output transform
//    vec3  VxTonemapAces(c)                     ACES filmic fit, result in [0, 1]
//    vec3  VxLinearToSrgb(c)                    sRGB transfer function (input clamped to [0, 1])
//    vec3  VxHeatColor(x)                       blue -> green -> red ramp for x in [0, 1]
// =============================================================================
#ifndef VX_SHARED_SHADING_GLSL
#define VX_SHARED_SHADING_GLSL

VX_CONST_FLOAT(VX_PI, 3.14159265358979f)
VX_CONST_FLOAT(VX_SUN_DISC_BOOST, 24.0f)        // disc radiance relative to the sun radiance
VX_CONST_FLOAT(VX_SUN_GLOW, 0.35f)              // strength of the tight halo around the disc
VX_CONST_FLOAT(VX_SUN_HAZE, 0.06f)              // strength of the wide forward-scattering lobe
VX_CONST_FLOAT(VX_LIGHT_MIN_DISTANCE, 0.1f)     // m, keeps 1 / d^2 finite next to a light
VX_CONST_FLOAT(VX_SPECULAR_MAX_EXPONENT, 1024.0f)

// ---- Random numbers ------------------------------------------------------------------------

// PCG hash (Jarzynski & Olano, "Hash Functions for GPU Rendering").
uint VxHashPcg(uint v) {
    uint state = v * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

// Decorrelated generator state for pixel (x, y) in frame `frame`.
uint VxRandomSeed(uint x, uint y, uint frame) {
    return VxHashPcg(x + VxHashPcg(y + VxHashPcg(frame)));
}

// Uniform float in [0, 1): the top 24 bits of the next state, which a float holds exactly.
float VxRandom(VX_INOUT(uint) state) {
    state = VxHashPcg(state);
    return float(state >> 8u) * (1.0f / 16777216.0f);
}

// ---- Sampling ------------------------------------------------------------------------------

// Branch-light orthonormal basis (Duff et al., "Building an Orthonormal Basis, Revisited").
// `n` must be normalised; the denominator is never smaller than 1 in magnitude.
void VxOrthonormalBasis(vec3 n, VX_OUT(vec3) tangent, VX_OUT(vec3) bitangent) {
    float s = (n.z >= 0.0f) ? 1.0f : -1.0f;
    float a = -1.0f / (s + n.z);
    float b = n.x * n.y * a;
    tangent = vec3(1.0f + s * n.x * n.x * a, s * b, -s * n.x);
    bitangent = vec3(b, s + n.y * n.y * a, -n.y);
}

// Cosine-weighted direction in the hemisphere around the unit normal `n` (pdf = cos / pi).
// u1, u2 are uniform numbers in [0, 1).
vec3 VxCosineHemisphere(vec3 n, float u1, float u2) {
    vec3 t;
    vec3 b;
    VxOrthonormalBasis(n, t, b);
    float r = sqrt(clamp(u1, 0.0f, 1.0f));
    float phi = 2.0f * VX_PI * u2;
    float z = sqrt(max(1.0f - r * r, 0.0f));
    vec3 d = t * (r * cos(phi)) + b * (r * sin(phi)) + n * z;
    return d * inversesqrt(max(dot(d, d), 1.0e-12f));
}

// Direction inside the cone around the unit vector `axis` whose half angle has the tangent
// `tanRadius` (uniform over the cone's cross-section). tanRadius = 0 returns the axis.
vec3 VxConeDirection(vec3 axis, float tanRadius, float u1, float u2) {
    vec3 t;
    vec3 b;
    VxOrthonormalBasis(axis, t, b);
    float r = max(tanRadius, 0.0f) * sqrt(clamp(u1, 0.0f, 1.0f));
    float phi = 2.0f * VX_PI * u2;
    vec3 d = axis + t * (r * cos(phi)) + b * (r * sin(phi));
    return d * inversesqrt(max(dot(d, d), 1.0e-12f));
}

// ---- Environment ---------------------------------------------------------------------------

float VxLuminance(vec3 c) {
    return dot(c, vec3(0.2126f, 0.7152f, 0.0722f));
}

// Radiance of the sky in direction `dir`.
//   zenith / horizon / ground   gradient colours: horizon -> zenith above, horizon -> ground
//                               just below the horizon
//   sunDir, sunTanRadius        direction towards the sun and tangent of its angular radius
//   sunRadiance                 radiance the sun contributes to a surface facing it
//   sunAmount                   scales the sun disc and glow; pass 0 for diffuse (ambient)
//                               rays, whose sun contribution is handled by the direct term
vec3 VxSkyRadiance(vec3 dir, vec3 sunDir, float sunTanRadius, vec3 sunRadiance,
                   vec3 zenith, vec3 horizon, vec3 ground, float sunAmount) {
    float up = dir.y;
    float skyT = sqrt(clamp(up, 0.0f, 1.0f));
    float groundT = smoothstep(0.0f, 0.12f, -up);
    vec3 color = mix(mix(horizon, zenith, skyT), ground, groundT);

    // Sun: a soft-edged disc, a tight halo and a wide haze, all fading out below the horizon.
    float cosSun = clamp(dot(dir, sunDir), -1.0f, 1.0f);
    float tanR = max(sunTanRadius, 1.0e-4f);
    float cosRadius = inversesqrt(1.0f + tanR * tanR);
    float edge = max((1.0f - cosRadius) * 0.35f, 1.0e-6f);
    float disc = smoothstep(cosRadius - edge, cosRadius + edge, cosSun);
    float c = max(cosSun, 0.0f);
    float c2 = c * c;
    float c8 = c2 * c2 * c2 * c2;
    float c64 = c8 * c8 * c8 * c8 * c8 * c8 * c8 * c8;
    float halo = c64 * c64 * c64 * c64;
    float visible = (1.0f - groundT) * max(sunAmount, 0.0f);
    float sun = disc * VX_SUN_DISC_BOOST + halo * VX_SUN_GLOW + c8 * VX_SUN_HAZE;
    return color + sunRadiance * (sun * visible);
}

// Opacity of exponential height fog between `ro` and `ro + rd * dist`.
//   density         extinction (1/m) at height `baseHeight`
//   heightFalloff   how fast the density decays with height (1/m); 0 = uniform fog
//   startDistance   the fog only begins this far from the ray origin
//   maxOpacity      upper limit of the result
// The density is integrated analytically along the ray. An exponential integrated over a
// segment equals its largest value times (1 - e^-|k|) / |k|, a form that needs no large
// exponent, so the result is finite and accurate for any input.
float VxFogAmount(vec3 ro, vec3 rd, float dist, float density, float heightFalloff,
                  float baseHeight, float startDistance, float maxOpacity) {
    float start = max(startDistance, 0.0f);
    float len = max(dist - start, 0.0f);
    float falloff = max(heightFalloff, 0.0f);
    float originHeight = ro.y + rd.y * start - baseHeight;
    float k = falloff * rd.y * len;                         // exponent difference over the segment
    float lowest = min(originHeight, originHeight + rd.y * len);
    float atLowest = exp(clamp(-falloff * lowest, -80.0f, 60.0f));
    // (1 - e^-|k|) / |k|, with its limit 1 - |k| / 2 around 0.
    float ak = abs(k);
    float integral = (ak > 1.0e-3f) ? (1.0f - exp(-ak)) / max(ak, 1.0e-3f) : 1.0f - 0.5f * ak;
    float opticalDepth = max(density, 0.0f) * atLowest * len * integral;
    float amount = 1.0f - exp(-min(opticalDepth, 80.0f));
    return clamp(amount, 0.0f, clamp(maxOpacity, 0.0f, 1.0f));
}

// ---- Surfaces and lights -------------------------------------------------------------------

// Material colour with the per-voxel brightness jitter.
//   variationAmount   material parameter: relative brightness range (0 = flat colour)
//   variation         per-voxel value in [0, 1] (G-buffer variation byte / 255)
vec3 VxAlbedo(vec3 color, float variationAmount, float variation) {
    float scale = 1.0f + variationAmount * (variation * 2.0f - 1.0f);
    return max(color * max(scale, 0.0f), vec3(0.0f));
}

// Specular highlight of a light in direction `l` seen from direction `v` (both pointing away
// from the surface): an energy-normalised Blinn-Phong lobe, already multiplied by N.L.
// Roughness 1 gives a wide, dim lobe; roughness -> 0 a tight, bright one (exponent capped).
float VxSpecular(vec3 n, vec3 v, vec3 l, float roughness) {
    float ndl = max(dot(n, l), 0.0f);
    vec3 hv = l + v;
    vec3 h = hv * inversesqrt(max(dot(hv, hv), 1.0e-8f));
    float ndh = clamp(dot(n, h), 0.0f, 1.0f);
    float a = clamp(roughness, 0.03f, 1.0f);
    float a2 = a * a;
    float exponent = clamp(2.0f / (a2 * a2) - 2.0f, 1.0f, VX_SPECULAR_MAX_EXPONENT);
    float lobe = pow(max(ndh, 1.0e-4f), exponent);
    return lobe * (exponent + 2.0f) * 0.125f * ndl;
}

// Distance attenuation of a point light: inverse square, multiplied by a smooth window that
// reaches zero (with zero slope) at `radius`, so lights can be culled by range without a seam.
float VxLightFalloff(float dist, float radius) {
    float r = max(radius, 1.0e-4f);
    float x = clamp(dist / r, 0.0f, 1.0f);
    float x2 = x * x;
    float window = (1.0f - x2 * x2);
    window = window * window;
    return window / (dist * dist + VX_LIGHT_MIN_DISTANCE * VX_LIGHT_MIN_DISTANCE);
}

// ---- Output transform ----------------------------------------------------------------------

// ACES filmic curve (Narkowicz fit). Input: linear HDR, output: linear in [0, 1].
vec3 VxTonemapAces(vec3 color) {
    vec3 x = max(color, vec3(0.0f));
    vec3 num = x * (x * 2.51f + vec3(0.03f));
    vec3 den = x * (x * 2.43f + vec3(0.59f)) + vec3(0.14f);
    return clamp(num / den, vec3(0.0f), vec3(1.0f));
}

// Linear -> sRGB transfer function (the exact piecewise curve).
vec3 VxLinearToSrgb(vec3 color) {
    vec3 c = clamp(color, vec3(0.0f), vec3(1.0f));
    vec3 lo = c * 12.92f;
    vec3 hi = pow(max(c, vec3(0.0031308f)), vec3(1.0f / 2.4f)) * 1.055f - vec3(0.055f);
    return mix(lo, hi, step(vec3(0.0031308f), c));
}

// False colour ramp for scalar debug views: 0 = blue, 0.5 = green, 1 = red.
vec3 VxHeatColor(float value) {
    float x = clamp(value, 0.0f, 1.0f);
    return vec3(clamp(4.0f * x - 2.0f, 0.0f, 1.0f),
                clamp(min(4.0f * x, 4.0f - 4.0f * x), 0.0f, 1.0f),
                clamp(2.0f - 4.0f * x, 0.0f, 1.0f));
}

#endif // VX_SHARED_SHADING_GLSL
