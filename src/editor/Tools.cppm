module;

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

export module vortex.editor:tools;

import vortex.core;
import vortex.math;
import vortex.voxel;
import vortex.physics;
import vortex.scene;
import :view;

export namespace vortex::editor {

    // ---------------------------------------------------------------------------------------------
    //  The logic of the editor tools. Nothing here touches ImGui, a window or a GPU: the functions
    //  work on the same World / Scene / PhysicsWorld / MaterialLibrary objects the unit tests
    //  build, and editor::Editor only adds widgets and input on top of them.
    //  Main-thread only, no exceptions; failures are reported through the return value.
    // ---------------------------------------------------------------------------------------------

    // --- what is under the cursor ----------------------------------------------------------------

    /// @brief Closest surface along a ray: terrain voxels or the body of a scene entity.
    struct SurfaceHit {
        bool hit = false;
        float distance = 0.0f;                      ///< Metres along the ray.
        glm::dvec3 position{0.0};                   ///< World metres.
        glm::vec3 normal{0.0f, 1.0f, 0.0f};         ///< Unit, pointing back towards the ray origin's side.
        scene::Entity entity = scene::kNoEntity;    ///< The entity that was hit; kNoEntity = terrain.
    };

    /**
     * @brief Casts a ray against the terrain voxels (voxel::World::Raycast, exact to the voxel)
     * and against the physics bodies (PhysicsWorld::Raycast), and returns whichever is closer.
     * @details
     *  - A physics hit counts as an entity only if `scene.EntityFromBody` resolves it to a living
     *    entity. Terrain collision meshes (user data 0) and foreign bodies are treated as terrain:
     *    they only matter when the voxel ray found nothing (then the physics hit is returned with
     *    entity = kNoEntity).
     *  - Terrain colliders only exist near the camera, the voxel world everywhere in the window;
     *    comparing both distances is what stops a click from selecting an object through a hill.
     *  - On a tie the entity wins (an object resting on the ground is in front of it).
     *  - Entities without a RigidBody (visual-only objects, lights) are never hit; select those
     *    from the scene list.
     *  - A physics world that is not initialised simply contributes no hits.
     * @param maxDistance Metres; <= 0 or a zero direction gives no hit.
     */
    SurfaceHit TraceSurface(const CursorRay& ray, const voxel::World& world, const physics::PhysicsWorld& physics,
                            const scene::Scene& scene, float maxDistance);

    // --- brush -----------------------------------------------------------------------------------

    enum class BrushShape : uint8_t { Sphere, Box };
    enum class BrushMode : uint8_t { Add, Remove };

    struct BrushSettings {
        BrushShape shape = BrushShape::Sphere;
        BrushMode mode = BrushMode::Remove;
        float radius = 1.0f;          ///< Metres: sphere radius / half the edge of the box. Clamped to [voxel size, kMaxBrushRadius].
        uint8_t material = 1;         ///< World palette id written by Add (ignored by Remove). 0 makes Add a no-op.
        float spacing = 0.5f;         ///< Distance between two applications of one stroke, as a fraction of the radius (clamped to [0.05, 4]).
    };

    /// @brief Largest brush radius in metres (a sphere this size touches about 4 million voxels at 0.1 m).
    inline constexpr float kMaxBrushRadius = 10.0f;

    /// @brief Progress of one stroke (mouse button held). Value-initialise to start a new stroke.
    struct BrushStroke {
        bool active = false;                ///< At least one application happened in this stroke.
        glm::dvec3 lastCenter{0.0};         ///< Centre of the most recent application (world metres).
        bool resting = false;               ///< StrokeBrushAlongRay only: `restPosition` is valid.
        glm::dvec3 restPosition{0.0};       ///< Where the ray of the most recent application meets the terrain AFTER that edit.
    };

    struct BrushResult {
        bool applied = false;       ///< false: skipped (too close to the previous application of the stroke).
        uint32_t changed = 0;       ///< Voxels whose id changed.
    };

    /**
     * @brief Where the brush shape goes for a surface hit.
     * @details Remove: the hit point itself - the shape is centred on the surface and bites half
     * of itself out of it. Add: `hit + normal * max(radius - voxelSize, 0)` - the shape sits ON
     * the surface, sunk in by one voxel so that what is added is always attached to what was hit.
     * `radius` is the clamped radius (see BrushSettings::radius).
     */
    glm::dvec3 BrushCenter(const BrushSettings& settings, const glm::dvec3& hitPosition, const glm::vec3& hitNormal, float voxelSize);

    /**
     * @brief Applies the brush once, centred on `center` (world metres).
     * @details Add writes `settings.material`, Remove writes 0 - through World::FillSphere /
     * World::FillBox, i.e. regardless of material hardness or the indestructible flag (this is an
     * editor, not a weapon). Sphere: centre and radius converted to voxel units. Box: the voxel
     * range `[round((center - radius) / voxelSize), round((center + radius) / voxelSize))` per
     * axis, at least one voxel thick. Chunks that are not loaded are skipped by the world, the
     * edit marks chunks modified (saved with the world) and queues them for the terrain colliders.
     * @return Number of voxels that changed.
     */
    uint32_t ApplyBrush(voxel::World& world, const BrushSettings& settings, const glm::dvec3& center);

    /**
     * @brief Continuous painting at a known point: applies the brush unless this stroke already
     * applied it closer than `spacing * radius` (never less than one voxel) to `center`.
     * @details The first call of a stroke (`!stroke.active`) always applies. An application
     * updates `stroke` even when it changed no voxel, so holding the button on one spot costs one
     * edit, not one per frame. Reset the stroke (`stroke = {}`) when the button is released.
     * The distance is the plain 3D distance between centres: right for centres the caller chooses,
     * wrong for centres found by a ray cast against the terrain being edited - use
     * StrokeBrushAlongRay for those.
     */
    BrushResult StrokeBrush(voxel::World& world, const BrushSettings& settings, BrushStroke& stroke, const glm::dvec3& center);

    /**
     * @brief Continuous painting under a cursor: casts `ray` against the terrain
     * (World::Raycast up to `maxDistance`) and applies the brush at BrushCenter of the hit, with
     * the stroke spacing measured as the distance the hit point travelled over the terrain.
     * @details Every application changes the surface under the cursor - removing makes the next
     * hit deeper, adding brings it closer - so the distance between consecutive hits (or brush
     * centres) is not the distance the cursor moved: a resting cursor would re-apply every frame
     * and drill (or pile up) along the ray. Therefore, after each application the same ray is
     * cast again and the hit on the EDITED terrain is remembered (BrushStroke::restPosition);
     * the next application happens once the current hit is `spacing * radius` (at least one
     * voxel) away from it. A cursor that rests gets the remembered point again and applies
     * exactly once; a cursor that moves is measured along the real surface in 3D, so the spacing
     * is the same whether it moves sideways or away from the camera at a grazing angle.
     * If the ray finds no terrain after an application (the brush removed all of it), the next
     * terrain hit applies. The first call of a stroke always applies.
     * @return `applied == false` if the ray hits no terrain (the stroke is not changed) or the
     * hit is too close; on an application `stroke.lastCenter` is where the brush went.
     */
    BrushResult StrokeBrushAlongRay(voxel::World& world, const BrushSettings& settings, BrushStroke& stroke, const CursorRay& ray,
                                    float maxDistance);

    // --- primitives and placement ----------------------------------------------------------------

    enum class PrimitiveShape : uint8_t { Box, Sphere };

    /// @brief Largest primitive edge in voxels.
    inline constexpr int kMaxPrimitiveSize = 256;

    struct PrimitiveSettings {
        PrimitiveShape shape = PrimitiveShape::Box;
        glm::ivec3 size{8};           ///< Voxels per axis, each clamped to [1, kMaxPrimitiveSize].
        uint8_t material = 1;         ///< Palette id of every voxel (0 gives an empty volume).
    };

    /**
     * @brief Dense volume of a primitive, exactly `size` voxels large.
     * @details Box: every voxel solid. Sphere: the ellipsoid inscribed in the box - a voxel is
     * solid when its centre `(x + 0.5, y + 0.5, z + 0.5)` satisfies `sum(((c - size/2) /
     * (size/2))^2) <= 1`. The result is symmetric under mirroring each axis, a 1-voxel axis is
     * always filled along that axis, and a size of 1x1x1 is one voxel.
     */
    voxel::VoxelVolume BuildPrimitive(const PrimitiveSettings& settings);

    /**
     * @brief Position (world metres, model minimum corner, no rotation) that puts a model of
     * `sizeVoxels` at an anchor point.
     * @param normal Zero: the model is centred on `anchor` (dropping something in front of the
     * camera). Otherwise the surface normal at `anchor`: the model is moved along it by its
     * half extent in that direction, `0.5 * dot(abs(n), size * voxelSize)`, so it rests on the
     * surface instead of being stuck in it (n is normalised here).
     */
    glm::vec3 PlaceOnSurface(const glm::ivec3& sizeVoxels, float voxelSize, const glm::dvec3& anchor, const glm::vec3& normal);

    // --- materials -------------------------------------------------------------------------------

    /// @brief Entry of a material combo box.
    struct MaterialChoice {
        uint8_t id = 0;
        std::string name;
    };

    /// @brief The named entries (ids 1..255, ascending) of a palette. Entries without a name are
    /// not listed; an unknown palette gives an empty list.
    std::vector<MaterialChoice> ListMaterials(const voxel::MaterialLibrary& library, voxel::PaletteId palette = voxel::kWorldPalette);

    /**
     * @brief Creates a palette for an imported mesh: `materials[i]` becomes entry `i + 1`, the
     * numbering voxel::MeshImportResult::materials uses, named "<name> <i + 1>".
     * @details A palette has 255 usable entries: materials beyond that are dropped with one log
     * line (their voxels render with the default material). An empty list still creates the
     * palette. Existing palettes are not touched.
     * @return The new palette's id.
     */
    voxel::PaletteId BuildImportPalette(voxel::MaterialLibrary& library, std::span<const voxel::Material> materials, std::string_view name);

    // --- import ----------------------------------------------------------------------------------

    struct ImportSettings {
        std::filesystem::path file;         ///< `.vox` (MagicaVoxel) or `.gltf` / `.glb`; the extension decides, case-insensitively.
        float voxelsPerUnit = 10.0f;        ///< glTF only: mesh unit (metre) -> voxels. Use 1 / voxel size for true scale.
        bool fillInterior = true;           ///< glTF only: make closed meshes solid.
        int maxVoxelsPerAxis = 256;         ///< glTF only: larger results are rejected.
    };

    /// @brief One model of an imported file.
    struct ImportedModel {
        std::shared_ptr<voxel::VoxelModel> model;   ///< Not baked yet; Scene::CreateVoxelObject bakes it.
        glm::ivec3 offset{0};                       ///< Voxels: minimum corner relative to the file's origin (.vox: VoxModelOffset; 0 for glTF).
    };

    struct ImportResult {
        bool ok = false;
        std::string error;                  ///< Human readable reason when !ok (shown in the editor).
        std::vector<ImportedModel> models;  ///< At least one when ok.
        voxel::PaletteId palette = 0;       ///< The palette created for the file (valid when ok).
    };

    /**
     * @brief Minimum corner (engine voxels) of a `.vox` model from its scene translation
     * (voxel::VoxModel::translation) and its size (engine axes).
     * @details MagicaVoxel places the voxel `floor(size / 2)` of a model at the translation, so
     * in its own axes the model starts at `t - floor(size / 2)`. Engine x and y are MagicaVoxel x
     * and z and keep that. Engine z is MagicaVoxel -y: the mirrored range starts at
     * `-(t.y - floor(size / 2) + size) = translation.z - ceil(size / 2)`. Using the floor there
     * would shift every model of odd depth by one voxel against its neighbours.
     */
    glm::ivec3 VoxModelOffset(const glm::ivec3& translation, const glm::ivec3& size);

    /**
     * @brief Loads a model file into voxel models with a palette of their own.
     * @details `.vox`: voxel::LoadVox, the palette through voxel::RegisterVoxPalette, one model
     * per non-empty model of the file (named after the model or the file). glTF: voxel::ImportMesh
     * and BuildImportPalette(result.materials); one model. Nothing is added to the library when
     * the import fails (missing file, parse error, unsupported extension, no solid voxels): the
     * palette is only created after the file was read successfully.
     */
    ImportResult ImportModels(voxel::MaterialLibrary& library, const ImportSettings& settings);

    // --- entities --------------------------------------------------------------------------------

    /// @brief What the inspector shows about an entity. Plain values: safe to keep across frames.
    struct EntityInfo {
        bool valid = false;                 ///< false: kNoEntity or a stale id; every other field is default.
        std::string name;                   ///< scene::Name, or empty.
        bool hasTransform = false;
        glm::vec3 position{0.0f};           ///< Transform::current.
        glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
        bool hasVoxels = false;             ///< Has a VoxelBody with a model.
        uint32_t voxelCount = 0;            ///< Solid voxels of the model.
        glm::ivec3 sizeVoxels{0};
        bool hasBody = false;               ///< Has a RigidBody.
        physics::MotionType motion = physics::MotionType::Static;
        bool isLight = false;               ///< Has a PointLight.
    };

    EntityInfo DescribeEntity(const scene::Scene& scene, scene::Entity entity);

    /// @brief Label for lists and logs: the entity's name, or "object" / "light" / "entity" by
    /// what it is, followed by " #<index>" (the entity index without the version), e.g.
    /// "crate #12". Not unique across stale ids: use the entity value itself for ImGui ids.
    std::string EntityLabel(const scene::Scene& scene, scene::Entity entity);

    /**
     * @brief Pivot of the gizmo in the entity's local units (see GizmoModelMatrix): half the
     * model size for voxel objects, zero for everything else and for invalid ids.
     */
    glm::vec3 GizmoPivot(const scene::Scene& scene, scene::Entity entity);

    /**
     * @brief Creates a copy of an entity, moved by `offset` (world metres).
     * @details Voxel objects: the copy SHARES the model (models are copy-on-write under damage,
     * see scene::Scene::Damage), with the same rotation, visibility, name and - if the source has
     * a body - motion type; it starts at rest. Lights (Transform + PointLight, no VoxelBody): a
     * new persistent light with the same parameters; a Lifetime is not copied. Game components
     * are not copied.
     * @return The new entity, or kNoEntity if `entity` is invalid, is neither a voxel object nor
     * a light, or could not be created (out of chunk slots or bodies; the scene logs why).
     */
    scene::Entity DuplicateEntity(scene::Scene& scene, scene::Entity entity, const glm::vec3& offset);
}
