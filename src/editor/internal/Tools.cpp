module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module vortex.editor;

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.physics;
import vortex.scene;

namespace vortex::editor {

    namespace {

        float ClampedRadius(const BrushSettings& settings, float voxelSize) {
            const float lo = std::min(voxelSize, kMaxBrushRadius);
            return std::isfinite(settings.radius) ? std::clamp(settings.radius, lo, kMaxBrushRadius) : lo;
        }

        glm::vec3 NormalizedOrZero(const glm::vec3& v) {
            const float length = glm::length(v);
            return std::isfinite(length) && length > 1.0e-6f ? v / length : glm::vec3(0.0f);
        }

        glm::ivec3 ClampedPrimitiveSize(const glm::ivec3& size) {
            return glm::clamp(size, glm::ivec3(1), glm::ivec3(kMaxPrimitiveSize));
        }

        std::string LowerExtension(const std::filesystem::path& file) {
            std::string extension = file.extension().string();
            for (char& c : extension) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return extension;
        }
    }

    // --- what is under the cursor ----------------------------------------------------------------

    SurfaceHit TraceSurface(const CursorRay& ray, const voxel::World& world, const physics::PhysicsWorld& physics,
                            const scene::Scene& scene, float maxDistance) {
        SurfaceHit out;
        const float length = glm::length(ray.direction);
        if (!(maxDistance > 0.0f) || !std::isfinite(length) || length <= 1.0e-6f) return out;
        const glm::vec3 direction = ray.direction / length;

        const voxel::WorldRayHit terrain = world.Raycast(ray.origin, direction, maxDistance);
        physics::RayHit body;
        if (physics.IsInitialized()) body = physics.Raycast(glm::vec3(ray.origin), direction, maxDistance);
        const scene::Entity entity = body.hit ? scene.EntityFromBody(body.body) : scene::kNoEntity;

        const auto fromBody = [&](scene::Entity owner) {
            out.hit = true;
            out.distance = body.distance;
            out.position = ray.origin + glm::dvec3(direction) * static_cast<double>(body.distance);
            out.normal = body.normal;
            out.entity = owner;
        };

        if (entity != scene::kNoEntity && (!terrain.hit || body.distance <= terrain.distance)) {
            fromBody(entity);
        } else if (terrain.hit) {
            out.hit = true;
            out.distance = terrain.distance;
            out.position = terrain.position;
            out.normal = terrain.normal;
        } else if (body.hit) {
            fromBody(scene::kNoEntity);     // a terrain collider or a foreign body where no voxel was found
        }
        return out;
    }

    // --- brush -----------------------------------------------------------------------------------

    glm::dvec3 BrushCenter(const BrushSettings& settings, const glm::dvec3& hitPosition, const glm::vec3& hitNormal, float voxelSize) {
        if (settings.mode == BrushMode::Remove) return hitPosition;
        const float lift = std::max(ClampedRadius(settings, voxelSize) - voxelSize, 0.0f);
        return hitPosition + glm::dvec3(NormalizedOrZero(hitNormal)) * static_cast<double>(lift);
    }

    uint32_t ApplyBrush(voxel::World& world, const BrushSettings& settings, const glm::dvec3& center) {
        const uint8_t id = settings.mode == BrushMode::Add ? settings.material : uint8_t{0};
        if (settings.mode == BrushMode::Add && id == 0) return 0;

        const float voxelSize = world.VoxelSize();
        const double radius = static_cast<double>(ClampedRadius(settings, voxelSize));
        const double inv = 1.0 / static_cast<double>(voxelSize);

        if (settings.shape == BrushShape::Sphere) {
            return world.FillSphere(glm::vec3(center * inv), static_cast<float>(radius * inv), id);
        }
        const glm::ivec3 lo = glm::ivec3(glm::round((center - glm::dvec3(radius)) * inv));
        const glm::ivec3 hi = glm::max(glm::ivec3(glm::round((center + glm::dvec3(radius)) * inv)), lo + glm::ivec3(1));
        return world.FillBox(lo, hi, id);
    }

    namespace {
        /// Distance between two applications of one stroke, in metres.
        double StrokeSpacing(const BrushSettings& settings, float voxelSize) {
            const float spacing = std::isfinite(settings.spacing) ? std::clamp(settings.spacing, 0.05f, 4.0f) : 0.5f;
            return static_cast<double>(std::max(spacing * ClampedRadius(settings, voxelSize), voxelSize));
        }
    }

    BrushResult StrokeBrush(voxel::World& world, const BrushSettings& settings, BrushStroke& stroke, const glm::dvec3& center) {
        BrushResult result;
        if (stroke.active && glm::length(center - stroke.lastCenter) < StrokeSpacing(settings, world.VoxelSize())) return result;

        result.applied = true;
        result.changed = ApplyBrush(world, settings, center);
        stroke.active = true;
        stroke.lastCenter = center;
        stroke.resting = false;
        return result;
    }

    BrushResult StrokeBrushAlongRay(voxel::World& world, const BrushSettings& settings, BrushStroke& stroke, const CursorRay& ray,
                                    float maxDistance) {
        BrushResult result;
        const glm::vec3 direction = NormalizedOrZero(ray.direction);
        if (!(maxDistance > 0.0f) || direction == glm::vec3(0.0f)) return result;
        const voxel::WorldRayHit hit = world.Raycast(ray.origin, direction, maxDistance);
        if (!hit.hit) return result;

        const float voxelSize = world.VoxelSize();
        if (stroke.active && stroke.resting && glm::length(hit.position - stroke.restPosition) < StrokeSpacing(settings, voxelSize)) {
            return result;
        }

        const glm::dvec3 center = BrushCenter(settings, hit.position, hit.normal, voxelSize);
        result.applied = true;
        result.changed = ApplyBrush(world, settings, center);
        stroke.active = true;
        stroke.lastCenter = center;
        // Where a cursor that does not move will hit from now on.
        const voxel::WorldRayHit rest = world.Raycast(ray.origin, direction, maxDistance);
        stroke.resting = rest.hit;
        stroke.restPosition = rest.position;
        return result;
    }

    // --- primitives and placement ----------------------------------------------------------------

    voxel::VoxelVolume BuildPrimitive(const PrimitiveSettings& settings) {
        const glm::ivec3 size = ClampedPrimitiveSize(settings.size);
        voxel::VoxelVolume volume(size);
        if (settings.material == 0) return volume;

        if (settings.shape == PrimitiveShape::Box) {
            volume.FillBox(glm::ivec3(0), size, settings.material);
            return volume;
        }
        const glm::vec3 half = glm::vec3(size) * 0.5f;
        for (int z = 0; z < size.z; ++z)
            for (int y = 0; y < size.y; ++y)
                for (int x = 0; x < size.x; ++x) {
                    const glm::vec3 d = (glm::vec3(x, y, z) + glm::vec3(0.5f) - half) / half;
                    if (glm::dot(d, d) <= 1.0f) volume.Set(x, y, z, settings.material);
                }
        return volume;
    }

    glm::vec3 PlaceOnSurface(const glm::ivec3& sizeVoxels, float voxelSize, const glm::dvec3& anchor, const glm::vec3& normal) {
        const glm::vec3 extent = glm::vec3(sizeVoxels) * voxelSize;
        const glm::vec3 n = NormalizedOrZero(normal);
        const float lift = 0.5f * glm::dot(glm::abs(n), extent);
        const glm::dvec3 center = anchor + glm::dvec3(n) * static_cast<double>(lift);
        return glm::vec3(center - glm::dvec3(extent) * 0.5);
    }

    // --- materials -------------------------------------------------------------------------------

    std::vector<MaterialChoice> ListMaterials(const voxel::MaterialLibrary& library, voxel::PaletteId palette) {
        std::vector<MaterialChoice> out;
        if (palette >= library.PaletteCount()) return out;
        for (uint32_t i = 1; i < voxel::kPaletteSize; ++i) {
            const std::string_view name = library.Name(palette, static_cast<uint8_t>(i));
            if (!name.empty()) out.push_back({static_cast<uint8_t>(i), std::string(name)});
        }
        return out;
    }

    voxel::PaletteId BuildImportPalette(voxel::MaterialLibrary& library, std::span<const voxel::Material> materials, std::string_view name) {
        const voxel::PaletteId palette = library.CreatePalette(name);
        const size_t usable = voxel::kPaletteSize - 1;
        const size_t count = std::min(materials.size(), usable);
        for (size_t i = 0; i < count; ++i) {
            library.Set(palette, static_cast<uint8_t>(i + 1), materials[i], std::format("{} {}", name, i + 1));
        }
        if (materials.size() > usable) {
            Log::Warn("Import palette '{}': {} materials, only the first {} fit a palette", name, materials.size(), usable);
        }
        return palette;
    }

    // --- import ----------------------------------------------------------------------------------

    glm::ivec3 VoxModelOffset(const glm::ivec3& translation, const glm::ivec3& size) {
        // x, y: floor. z is the mirrored MagicaVoxel y axis: ceiling.
        return translation - glm::ivec3(math::FloorDiv(size.x, 2), math::FloorDiv(size.y, 2), size.z - math::FloorDiv(size.z, 2));
    }

    ImportResult ImportModels(voxel::MaterialLibrary& library, const ImportSettings& settings) {
        ImportResult result;
        if (settings.file.empty()) {
            result.error = "no file given";
            return result;
        }
        const std::string extension = LowerExtension(settings.file);
        const std::string shown = settings.file.string();
        std::string stem = settings.file.stem().string();
        if (stem.empty()) stem = "model";

        if (extension == ".vox") {
            std::string error;
            std::optional<voxel::VoxFile> file = voxel::LoadVox(settings.file, &error);
            if (!file) {
                result.error = error.empty() ? std::format("could not read '{}'", shown) : std::format("'{}': {}", shown, error);
                return result;
            }
            size_t solid = 0;
            for (const voxel::VoxModel& model : file->models) solid += model.volume.IsEmpty() ? 0u : 1u;
            if (solid == 0) {
                result.error = std::format("'{}' contains no solid voxels", shown);
                return result;
            }

            result.palette = voxel::RegisterVoxPalette(library, *file, stem);
            result.models.reserve(solid);
            for (voxel::VoxModel& model : file->models) {
                if (model.volume.IsEmpty()) continue;
                ImportedModel imported;
                imported.offset = VoxModelOffset(model.translation, model.volume.Size());
                std::string name = model.name.empty() ? stem : model.name;
                imported.model = std::make_shared<voxel::VoxelModel>(std::move(model.volume), result.palette, std::move(name));
                result.models.push_back(std::move(imported));
            }
            result.ok = true;
            return result;
        }

        if (extension == ".gltf" || extension == ".glb") {
            voxel::MeshImportSettings mesh;
            mesh.file = settings.file;
            mesh.voxelsPerUnit = std::isfinite(settings.voxelsPerUnit) && settings.voxelsPerUnit > 0.0f ? settings.voxelsPerUnit : 10.0f;
            mesh.fillInterior = settings.fillInterior;
            mesh.maxVoxelsPerAxis = std::max(settings.maxVoxelsPerAxis, 1);
            voxel::MeshImportResult imported = voxel::ImportMesh(mesh);
            if (!imported.ok) {
                result.error = imported.error.empty() ? std::format("could not import '{}'", shown) : std::format("'{}': {}", shown, imported.error);
                return result;
            }
            if (imported.volume.IsEmpty()) {
                result.error = std::format("'{}' produced no solid voxels", shown);
                return result;
            }
            result.palette = BuildImportPalette(library, imported.materials, stem);
            ImportedModel model;
            model.model = std::make_shared<voxel::VoxelModel>(std::move(imported.volume), result.palette, stem);
            result.models.push_back(std::move(model));
            result.ok = true;
            return result;
        }

        result.error = extension.empty() ? std::format("'{}' has no extension (expected .vox, .gltf or .glb)", shown)
                                         : std::format("unsupported file type '{}' (expected .vox, .gltf or .glb)", extension);
        return result;
    }

    // --- entities --------------------------------------------------------------------------------

    EntityInfo DescribeEntity(const scene::Scene& scene, scene::Entity entity) {
        EntityInfo info;
        if (!scene.IsValid(entity)) return info;
        info.valid = true;
        if (const scene::Name* name = scene.TryGet<scene::Name>(entity)) info.name = name->value;
        if (const scene::Transform* transform = scene.TryGet<scene::Transform>(entity)) {
            info.hasTransform = true;
            info.position = transform->current.position;
            info.rotation = transform->current.rotation;
        }
        if (const scene::VoxelBody* body = scene.TryGet<scene::VoxelBody>(entity); body != nullptr && body->model != nullptr) {
            info.hasVoxels = true;
            info.voxelCount = body->model->SolidCount();
            info.sizeVoxels = body->model->Size();
        }
        if (const scene::RigidBody* body = scene.TryGet<scene::RigidBody>(entity)) {
            info.hasBody = true;
            info.motion = body->motion;
        }
        info.isLight = scene.TryGet<scene::PointLight>(entity) != nullptr;
        return info;
    }

    std::string EntityLabel(const scene::Scene& scene, scene::Entity entity) {
        const uint32_t index = static_cast<uint32_t>(entt::to_entity(entity));
        std::string_view kind = "entity";
        if (const scene::Name* name = scene.TryGet<scene::Name>(entity); name != nullptr && !name->value.empty()) kind = name->value;
        else if (scene.TryGet<scene::VoxelBody>(entity) != nullptr) kind = "object";
        else if (scene.TryGet<scene::PointLight>(entity) != nullptr) kind = "light";
        return std::format("{} #{}", kind, index);
    }

    glm::vec3 GizmoPivot(const scene::Scene& scene, scene::Entity entity) {
        const scene::VoxelBody* body = scene.TryGet<scene::VoxelBody>(entity);
        if (body == nullptr || body->model == nullptr) return glm::vec3(0.0f);
        return glm::vec3(body->model->Size()) * 0.5f;
    }

    scene::Entity DuplicateEntity(scene::Scene& scene, scene::Entity entity, const glm::vec3& offset) {
        const scene::Transform* transform = scene.TryGet<scene::Transform>(entity);
        if (transform == nullptr) return scene::kNoEntity;
        // Copies, not pointers: creating the duplicate may move the component storage.
        const math::Transform pose = transform->current;

        if (const scene::VoxelBody* voxels = scene.TryGet<scene::VoxelBody>(entity); voxels != nullptr && voxels->model != nullptr) {
            scene::VoxelObjectDesc desc;
            desc.model = voxels->model;
            desc.position = pose.position + offset;
            desc.rotation = pose.rotation;
            desc.visible = voxels->visible;
            const scene::RigidBody* body = scene.TryGet<scene::RigidBody>(entity);
            desc.withBody = body != nullptr;
            if (body != nullptr) desc.motion = body->motion;
            if (const scene::Name* name = scene.TryGet<scene::Name>(entity)) desc.name = name->value;
            return scene.CreateVoxelObject(desc);
        }

        if (const scene::PointLight* source = scene.TryGet<scene::PointLight>(entity)) {
            const scene::PointLight light = *source;
            const scene::Name* sourceName = scene.TryGet<scene::Name>(entity);
            const std::string name = sourceName != nullptr ? sourceName->value : std::string();

            scene::LightDesc desc;
            desc.position = glm::dvec3(pose.position + offset);
            desc.color = light.color;
            desc.intensity = light.intensity;
            desc.radius = light.radius;
            desc.sourceRadius = light.sourceRadius;
            const scene::Entity copy = scene.CreateLight(desc);
            // CreateLight has no rotation / offset / name: carry them over.
            if (scene::Transform* created = scene.TryGet<scene::Transform>(copy)) {
                created->current.rotation = pose.rotation;
                created->previous = created->current;
            }
            if (scene::PointLight* created = scene.TryGet<scene::PointLight>(copy)) created->offset = light.offset;
            if (!name.empty() && scene.IsValid(copy)) scene.Registry().emplace_or_replace<scene::Name>(copy, name);
            return copy;
        }
        return scene::kNoEntity;
    }
}
