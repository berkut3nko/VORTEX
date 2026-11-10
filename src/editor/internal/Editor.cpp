module;

#include <entt/entt.hpp>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtc/type_ptr.hpp>

#include <imgui.h>
#include <ImGuizmo.h>

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

module vortex.editor;

import vortex.engine;

namespace vortex::editor {

    namespace {

        constexpr ImU32 kPreviewColor = IM_COL32(255, 214, 64, 235);
        constexpr ImU32 kPreviewShadow = IM_COL32(0, 0, 0, 160);
        const ImVec4 kErrorColor(1.0f, 0.42f, 0.36f, 1.0f);

        constexpr const char* kToolNames[kToolCount] = {"Select", "Brush", "Spawn"};
        constexpr const char* kMotionNames[] = {"Static", "Kinematic", "Dynamic"};
        constexpr const char* kBrushShapeNames[] = {"Sphere", "Box"};
        constexpr const char* kBrushModeNames[] = {"Add", "Remove"};
        constexpr const char* kPrimitiveNames[] = {"Box", "Sphere"};
        constexpr const char* kDebugViewNames[] = {"None", "Albedo", "Normal", "Depth", "Lighting", "Steps", "Motion", "Material"};

        /// Combo over the values 0 .. count - 1 of an enum.
        template <typename Enum, size_t Count>
        bool EnumCombo(const char* label, Enum& value, const char* const (&names)[Count]) {
            int index = std::clamp(static_cast<int>(value), 0, static_cast<int>(Count) - 1);
            const bool changed = ImGui::Combo(label, &index, names, static_cast<int>(Count));
            if (changed) value = static_cast<Enum>(index);
            return changed;
        }

        bool MaterialCombo(const char* label, const std::vector<MaterialChoice>& choices, uint8_t& id) {
            const char* preview = id == 0 ? "(none)" : "(unnamed)";
            for (const MaterialChoice& choice : choices) {
                if (choice.id == id) preview = choice.name.c_str();
            }
            bool changed = false;
            if (ImGui::BeginCombo(label, preview)) {
                for (const MaterialChoice& choice : choices) {
                    ImGui::PushID(static_cast<int>(choice.id));
                    const bool selected = choice.id == id;
                    if (ImGui::Selectable(choice.name.c_str(), selected)) {
                        id = choice.id;
                        changed = true;
                    }
                    if (selected) ImGui::SetItemDefaultFocus();
                    ImGui::PopID();
                }
                ImGui::EndCombo();
            }
            return changed;
        }

        /// First-use placement of a panel; a negative x counts from the right edge of the display.
        void PlacePanel(float x, float y, float width, float height) {
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            ImGui::SetNextWindowPos(ImVec2(x < 0.0f ? std::max(display.x + x, 0.0f) : x, y), ImGuiCond_FirstUseEver);
            ImGui::SetNextWindowSize(ImVec2(width, height), ImGuiCond_FirstUseEver);
        }

        const char* MotionName(physics::MotionType motion) {
            return kMotionNames[std::clamp(static_cast<int>(motion), 0, 2)];
        }

        double D(float v) { return static_cast<double>(v); }
    }

    struct Editor::Impl {
        bool open = false;
        bool active = false;            ///< OnUpdate ran while open and has not cleaned up since.
        Tool tool = Tool::Select;
        EditorSettings settings;
        EditorPanels panels;

        scene::Entity selection = scene::kNoEntity;
        scene::Entity gizmoEntity = scene::kNoEntity;   ///< What the gizmo was attached to last frame.
        bool cancelGizmo = false;                       ///< Reset ImGuizmo's drag state in the next OnGui.

        BrushStroke stroke;
        bool looking = false;           ///< Right button held: the mouse turns the camera.
        bool noMouseSet = false;        ///< We set ImGuiConfigFlags_NoMouse and have to clear it.

        bool hoverValid = false;        ///< A surface is under the cursor (Brush / Spawn tools).
        glm::dvec3 hoverPosition{0.0};
        glm::vec3 hoverNormal{0.0f, 1.0f, 0.0f};

        std::string status;
        bool statusError = false;

        std::vector<MaterialChoice> materials;
        uint64_t materialsRevision = 0;
        std::vector<scene::Entity> entities;            ///< Scratch list of the scene panel.

        std::array<char, 1024> importPath{};
        std::filesystem::path importPathShown;          ///< The path the text field was filled from.
        float sunAzimuth = 0.0f;                        ///< Kept while the sun is at the zenith.

        void SetStatus(std::string text, bool error = false) {
            status = std::move(text);
            statusError = error;
            if (error) Log::Warn("Editor: {}", status);
            else Log::Info("Editor: {}", status);
        }

        void RefreshMaterials(const voxel::MaterialLibrary& library) {
            if (materialsRevision == library.Revision()) return;
            materials = ListMaterials(library, voxel::kWorldPalette);
            materialsRevision = library.Revision();
        }

        /// Gives the cursor back and lets ImGui see the mouse again.
        void EndLook(graphics::Window* window) {
            looking = false;
            if (window != nullptr && window->IsCursorCaptured()) window->SetCursorCaptured(false);
            if (noMouseSet && ImGui::GetCurrentContext() != nullptr) ImGui::GetIO().ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
            noMouseSet = false;
        }

        void MenuBar(Editor& self);
        void ToolsPanel(Editor& self, Engine& engine);
        void ScenePanel(Editor& self, Engine& engine);
        void InspectorPanel(Editor& self, Engine& engine);
        void EnvironmentPanel(Engine& engine);
        void RenderPanel(Engine& engine);
        void ProfilerPanel();
        void WorldPanel(Engine& engine);
        void Gizmo(Editor& self, Engine& engine);
        void Overlays(Engine& engine);
    };

    // --- state -------------------------------------------------------------------------------------

    Editor::Editor() : m_Impl(std::make_unique<Impl>()) {}
    Editor::~Editor() = default;

    bool Editor::IsOpen() const { return m_Impl->open; }

    void Editor::SetOpen(bool open) {
        Impl& s = *m_Impl;
        if (s.open == open) return;
        s.open = open;
        if (!open) {
            // The cursor itself is released by the next OnUpdate (it needs the window).
            EndStroke();
            s.hoverValid = false;
            s.cancelGizmo = true;
        }
    }

    void Editor::Toggle() { SetOpen(!m_Impl->open); }
    bool Editor::CapturesInput() const { return m_Impl->open; }
    Tool Editor::ActiveTool() const { return m_Impl->tool; }

    void Editor::SetTool(Tool tool) {
        Impl& s = *m_Impl;
        const int index = static_cast<int>(tool);
        if (index < 0 || index >= kToolCount || tool == s.tool) return;
        s.tool = tool;
        s.hoverValid = false;
        s.cancelGizmo = true;       // a gizmo drag does not survive leaving (or re-entering) the Select tool
        EndStroke();
    }

    EditorSettings& Editor::Settings() { return m_Impl->settings; }
    const EditorSettings& Editor::Settings() const { return m_Impl->settings; }
    EditorPanels& Editor::Panels() { return m_Impl->panels; }

    scene::Entity Editor::Selection(const scene::Scene& scene) const {
        return scene.IsValid(m_Impl->selection) ? m_Impl->selection : scene::kNoEntity;
    }

    void Editor::Select(scene::Entity entity) {
        Impl& s = *m_Impl;
        if (entity == s.selection) return;
        s.selection = entity;
        s.cancelGizmo = true;       // a drag never continues on another entity
    }

    void Editor::EndStroke() { m_Impl->stroke = BrushStroke{}; }
    std::string_view Editor::Status() const { return m_Impl->status; }
    bool Editor::StatusIsError() const { return m_Impl->statusError; }

    // --- actions -----------------------------------------------------------------------------------

    bool Editor::DeleteSelection(Engine& engine) {
        Impl& s = *m_Impl;
        scene::Scene& scene = engine.Scene();
        const scene::Entity entity = Selection(scene);
        if (entity == scene::kNoEntity) {
            s.SetStatus("Nothing is selected", true);
            return false;
        }
        std::string label = EntityLabel(scene, entity);
        scene.Destroy(entity);
        Select(scene::kNoEntity);
        s.SetStatus(std::format("Deleted {}", label));
        return true;
    }

    scene::Entity Editor::DuplicateSelection(Engine& engine) {
        Impl& s = *m_Impl;
        scene::Scene& scene = engine.Scene();
        const scene::Entity entity = Selection(scene);
        if (entity == scene::kNoEntity) {
            s.SetStatus("Nothing is selected", true);
            return scene::kNoEntity;
        }
        const EntityInfo info = DescribeEntity(scene, entity);
        const float width = static_cast<float>(info.sizeVoxels.x) * scene.Config().voxelSize;
        const scene::Entity copy = DuplicateEntity(scene, entity, glm::vec3(std::max(width, 0.5f), 0.0f, 0.0f));
        if (copy == scene::kNoEntity) {
            s.SetStatus(std::format("Could not duplicate {} (out of chunk slots or bodies?)", EntityLabel(scene, entity)), true);
            return scene::kNoEntity;
        }
        Select(copy);
        s.SetStatus(std::format("Duplicated {} as {}", EntityLabel(scene, entity), EntityLabel(scene, copy)));
        return copy;
    }

    scene::Entity Editor::SpawnPrimitive(Engine& engine) {
        const graphics::Camera& camera = engine.camera;
        const float distance = std::isfinite(m_Impl->settings.spawnDistance) ? std::max(m_Impl->settings.spawnDistance, 0.0f) : 4.0f;
        const glm::dvec3 anchor = camera.position + glm::dvec3(camera.Forward()) * static_cast<double>(distance);
        return SpawnPrimitiveAt(engine, anchor, glm::vec3(0.0f));
    }

    scene::Entity Editor::SpawnPrimitiveAt(Engine& engine, const glm::dvec3& anchor, const glm::vec3& normal) {
        Impl& s = *m_Impl;
        scene::Scene& scene = engine.Scene();
        const char* shape = s.settings.primitive.shape == PrimitiveShape::Sphere ? "sphere" : "box";

        voxel::VoxelVolume volume = BuildPrimitive(s.settings.primitive);
        if (volume.IsEmpty()) {
            s.SetStatus("The primitive has no material: nothing to spawn", true);
            return scene::kNoEntity;
        }
        const glm::ivec3 size = volume.Size();
        scene::VoxelObjectDesc desc;
        desc.model = std::make_shared<voxel::VoxelModel>(std::move(volume), voxel::kWorldPalette, shape);
        desc.position = PlaceOnSurface(size, scene.Config().voxelSize, anchor, normal);
        desc.motion = s.settings.spawnMotion;
        const scene::Entity entity = scene.CreateVoxelObject(desc);
        if (entity == scene::kNoEntity) {
            s.SetStatus(std::format("Could not create the {} (out of chunk slots or bodies?)", shape), true);
            return scene::kNoEntity;
        }
        Select(entity);
        s.SetStatus(std::format("Spawned {} ({} x {} x {} voxels)", EntityLabel(scene, entity), size.x, size.y, size.z));
        return entity;
    }

    bool Editor::ImportFile(Engine& engine) {
        Impl& s = *m_Impl;
        scene::Scene& scene = engine.Scene();
        ImportResult imported = ImportModels(engine.Materials(), s.settings.import);
        if (!imported.ok) {
            s.SetStatus(std::format("Import failed: {}", imported.error), true);
            return false;
        }

        // The group keeps its layout and is centred in front of the camera.
        glm::ivec3 lo(imported.models.front().offset);
        glm::ivec3 hi(lo);
        for (const ImportedModel& model : imported.models) {
            lo = glm::min(lo, model.offset);
            hi = glm::max(hi, model.offset + model.model->Size());
        }
        const float voxelSize = scene.Config().voxelSize;
        const glm::vec3 centerVoxels = glm::vec3(lo + hi) * 0.5f;
        const graphics::Camera& camera = engine.camera;
        const float distance = std::isfinite(s.settings.spawnDistance) ? std::max(s.settings.spawnDistance, 0.0f) : 4.0f;
        const glm::dvec3 anchor = camera.position + glm::dvec3(camera.Forward()) * static_cast<double>(distance);

        scene::Entity first = scene::kNoEntity;
        size_t created = 0;
        for (ImportedModel& model : imported.models) {
            scene::VoxelObjectDesc desc;
            desc.position = glm::vec3(anchor + glm::dvec3((glm::vec3(model.offset) - centerVoxels) * voxelSize));
            desc.model = std::move(model.model);
            desc.motion = s.settings.spawnMotion;
            const scene::Entity entity = scene.CreateVoxelObject(desc);
            if (entity == scene::kNoEntity) break;
            if (first == scene::kNoEntity) first = entity;
            ++created;
        }
        if (first != scene::kNoEntity) Select(first);

        const std::string file = s.settings.import.file.filename().string();
        if (created != imported.models.size()) {
            s.SetStatus(std::format("Import of '{}' stopped after {} of {} models (out of chunk slots or bodies?)", file, created,
                                    imported.models.size()), true);
            return false;
        }
        s.SetStatus(std::format("Imported '{}': {} model(s), palette {}", file, created, imported.palette));
        return true;
    }

    BrushResult Editor::BrushAlong(Engine& engine, const CursorRay& ray) {
        Impl& s = *m_Impl;
        voxel::World& world = engine.World();
        const float reach = std::isfinite(s.settings.reach) ? std::max(s.settings.reach, 1.0f) : 200.0f;
        const float voxelSize = world.VoxelSize();
        // Spacing over the edited surface: a cursor that rests on one spot applies once instead of drilling.
        const BrushResult result = StrokeBrushAlongRay(world, s.settings.brush, s.stroke, ray, reach);
        const glm::dvec3 center = s.stroke.lastCenter;      // only used when the brush was applied
        if (result.changed > 0) {
            // What rested on (or was buried by) the edited ground has to react.
            const float reachOfEdit = std::clamp(s.settings.brush.radius, voxelSize, kMaxBrushRadius) + 1.0f;
            engine.Physics().ActivateInBox(math::AABB(glm::vec3(center) - glm::vec3(reachOfEdit), glm::vec3(center) + glm::vec3(reachOfEdit)));
            // Not logged: a stroke applies many times.
            s.status = std::format("Brush: {} voxels {}", result.changed, s.settings.brush.mode == BrushMode::Add ? "added" : "removed");
            s.statusError = false;
        }
        return result;
    }

    // --- per frame: input ----------------------------------------------------------------------------

    void Editor::OnUpdate(Engine& engine, float dt) {
        Impl& s = *m_Impl;
        Input& in = engine.GetInput();
        graphics::Window* window = engine.Window();

        if (in.KeyPressed(s.settings.toggleKey)) Toggle();

        if (!s.open) {
            if (s.active) {
                s.EndLook(window);
                s.active = false;
            }
            return;
        }
        s.active = true;
        const bool hasUi = ImGui::GetCurrentContext() != nullptr;

        // --- camera look: the right button is latched, so the release always ends it -------------
        if (!s.looking) {
            if (window != nullptr && in.MousePressed(MouseButton::Right)) s.looking = true;
        } else if (window == nullptr || in.MouseReleased(MouseButton::Right) ||
                   (!in.MouseCapturedByUi() && !in.MouseDown(MouseButton::Right))) {
            s.looking = false;
        }
        if (window != nullptr && window->IsCursorCaptured() != s.looking) window->SetCursorCaptured(s.looking);
        if (hasUi) {
            // imgui_impl_glfw keeps reporting the position of a disabled cursor: without this the
            // panels would react to the invisible cursor while the camera turns.
            ImGuiIO& io = ImGui::GetIO();
            if (s.looking && !s.noMouseSet) {
                io.ConfigFlags |= ImGuiConfigFlags_NoMouse;
                s.noMouseSet = true;
            } else if (!s.looking && s.noMouseSet) {
                io.ConfigFlags &= ~ImGuiConfigFlags_NoMouse;
                s.noMouseSet = false;
            }
        }

        // --- fly camera ------------------------------------------------------------------------------
        FlyInput fly;
        if (s.looking) fly.lookDelta = glm::vec2(static_cast<float>(in.MouseDeltaX()), static_cast<float>(in.MouseDeltaY()));
        const auto axis = [&in](Key positive, Key negative) {
            return (in.KeyDown(positive) ? 1.0f : 0.0f) - (in.KeyDown(negative) ? 1.0f : 0.0f);
        };
        fly.move = glm::vec3(axis(Key::D, Key::A), axis(Key::Space, Key::LeftControl), axis(Key::W, Key::S));
        fly.fast = in.KeyDown(Key::LeftShift);
        ApplyFlyCamera(engine.camera, fly, s.settings.fly, dt);

        // --- shortcuts -------------------------------------------------------------------------------
        if (in.KeyPressed(Key::Num1)) SetTool(Tool::Select);
        if (in.KeyPressed(Key::Num2)) SetTool(Tool::Brush);
        if (in.KeyPressed(Key::Num3)) SetTool(Tool::Spawn);
        if (in.KeyPressed(Key::G)) s.settings.gizmoOperation = GizmoOperation::Translate;
        if (in.KeyPressed(Key::R)) s.settings.gizmoOperation = GizmoOperation::Rotate;
        if (in.KeyPressed(Key::Delete)) DeleteSelection(engine);

        // --- the active tool, along the ray through the cursor ----------------------------------------
        s.hoverValid = false;
        if (!hasUi || s.looking || in.MouseCapturedByUi()) {
            EndStroke();
            return;
        }
        const ImGuiIO& io = ImGui::GetIO();
        if (!ImGui::IsMousePosValid(&io.MousePos) || !(io.DisplaySize.x > 0.0f) || !(io.DisplaySize.y > 0.0f)) {
            EndStroke();
            return;
        }
        const CursorRay ray = CursorToRay(engine.camera, glm::vec2(io.MousePos.x, io.MousePos.y), glm::vec2(io.DisplaySize.x, io.DisplaySize.y));
        const float reach = std::isfinite(s.settings.reach) ? std::max(s.settings.reach, 1.0f) : 200.0f;
        scene::Scene& scene = engine.Scene();

        switch (s.tool) {
            case Tool::Select: {
                if (!in.MousePressed(MouseButton::Left)) break;
                // The click belongs to the gizmo while it is hovered (last frame) or dragged.
                if (Selection(scene) != scene::kNoEntity && ImGuizmo::IsOver()) break;
                const SurfaceHit hit = TraceSurface(ray, engine.World(), engine.Physics(), scene, reach);
                Select(hit.entity);
                if (hit.entity != scene::kNoEntity) s.SetStatus(std::format("Selected {}", EntityLabel(scene, hit.entity)));
                else s.SetStatus("Selection cleared");
                break;
            }
            case Tool::Brush: {
                if (in.MouseDown(MouseButton::Left)) BrushAlong(engine, ray);
                else EndStroke();
                // After the edit, so the preview sits on the new surface.
                const voxel::WorldRayHit hit = engine.World().Raycast(ray.origin, ray.direction, reach);
                if (hit.hit) {
                    s.hoverValid = true;
                    s.hoverPosition = hit.position;
                    s.hoverNormal = hit.normal;
                }
                break;
            }
            case Tool::Spawn: {
                const SurfaceHit hit = TraceSurface(ray, engine.World(), engine.Physics(), scene, reach);
                if (hit.hit) {
                    s.hoverValid = true;
                    s.hoverPosition = hit.position;
                    s.hoverNormal = hit.normal;
                }
                if (in.MousePressed(MouseButton::Left)) {
                    if (hit.hit) SpawnPrimitiveAt(engine, hit.position, hit.normal);
                    else s.SetStatus("Nothing under the cursor to spawn on", true);
                }
                break;
            }
        }
    }

    // --- per frame: widgets --------------------------------------------------------------------------

    void Editor::OnGui(Engine& engine) {
        Impl& s = *m_Impl;
        if (!s.open || ImGui::GetCurrentContext() == nullptr) return;

        // Once per frame, after ImGui::NewFrame and before Manipulate.
        ImGuizmo::BeginFrame();
        const ImGuiIO& io = ImGui::GetIO();
        ImGuizmo::SetOrthographic(false);
        ImGuizmo::SetRect(0.0f, 0.0f, io.DisplaySize.x, io.DisplaySize.y);

        s.RefreshMaterials(engine.Materials());
        s.MenuBar(*this);

        EditorPanels& panels = s.panels;
        const float bottom = io.DisplaySize.y;
        if (panels.tools) {
            PlacePanel(-330.0f, 30.0f, 320.0f, 400.0f);
            if (ImGui::Begin("Tools##vortex.editor", &panels.tools)) s.ToolsPanel(*this, engine);
            ImGui::End();
        }
        if (panels.inspector) {
            PlacePanel(-330.0f, 440.0f, 320.0f, 270.0f);
            if (ImGui::Begin("Inspector##vortex.editor", &panels.inspector)) s.InspectorPanel(*this, engine);
            ImGui::End();
        }
        if (panels.scene) {
            PlacePanel(10.0f, 350.0f, 260.0f, 170.0f);
            if (ImGui::Begin("Scene##vortex.editor", &panels.scene)) s.ScenePanel(*this, engine);
            ImGui::End();
        }
        if (panels.world) {
            PlacePanel(10.0f, 530.0f, 260.0f, 180.0f);
            if (ImGui::Begin("World##vortex.editor", &panels.world)) s.WorldPanel(engine);
            ImGui::End();
        }
        if (panels.environment) {
            PlacePanel(-650.0f, 30.0f, 310.0f, 400.0f);
            if (ImGui::Begin("Environment##vortex.editor", &panels.environment)) s.EnvironmentPanel(engine);
            ImGui::End();
        }
        if (panels.render) {
            PlacePanel(-970.0f, 30.0f, 310.0f, 520.0f);
            if (ImGui::Begin("Render##vortex.editor", &panels.render)) s.RenderPanel(engine);
            ImGui::End();
        }
        if (panels.profiler) {
            PlacePanel(280.0f, std::max(bottom - 270.0f, 30.0f), std::max(io.DisplaySize.x - 620.0f, 320.0f), 260.0f);
            if (ImGui::Begin("Profiler##vortex.editor", &panels.profiler)) s.ProfilerPanel();
            ImGui::End();
        }

        s.Gizmo(*this, engine);
        s.Overlays(engine);
    }

    void Editor::Impl::MenuBar(Editor& self) {
        if (!ImGui::BeginMainMenuBar()) return;
        if (ImGui::BeginMenu("Editor")) {
            if (ImGui::MenuItem("Close", "F1")) self.SetOpen(false);
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Tool")) {
            for (int i = 0; i < kToolCount; ++i) {
                const char shortcut[2] = {static_cast<char>('1' + i), '\0'};
                if (ImGui::MenuItem(kToolNames[i], shortcut, tool == static_cast<Tool>(i))) self.SetTool(static_cast<Tool>(i));
            }
            ImGui::EndMenu();
        }
        if (ImGui::BeginMenu("Panels")) {
            ImGui::MenuItem("Tools", nullptr, &panels.tools);
            ImGui::MenuItem("Scene", nullptr, &panels.scene);
            ImGui::MenuItem("Inspector", nullptr, &panels.inspector);
            ImGui::MenuItem("Environment", nullptr, &panels.environment);
            ImGui::MenuItem("Render", nullptr, &panels.render);
            ImGui::MenuItem("Profiler", nullptr, &panels.profiler);
            ImGui::MenuItem("World", nullptr, &panels.world);
            ImGui::EndMenu();
        }
        ImGui::Separator();
        ImGui::TextDisabled("%s tool  |  hold RMB + WASD to fly", kToolNames[std::clamp(static_cast<int>(tool), 0, kToolCount - 1)]);
        if (!status.empty()) {
            ImGui::Separator();
            if (statusError) ImGui::TextColored(kErrorColor, "%s", status.c_str());
            else ImGui::TextUnformatted(status.c_str());
        }
        ImGui::EndMainMenuBar();
    }

    void Editor::Impl::ToolsPanel(Editor& self, Engine& engine) {
        for (int i = 0; i < kToolCount; ++i) {
            if (i > 0) ImGui::SameLine();
            if (ImGui::RadioButton(kToolNames[i], tool == static_cast<Tool>(i))) self.SetTool(static_cast<Tool>(i));
        }
        ImGui::Separator();

        scene::Scene& scene = engine.Scene();
        switch (tool) {
            case Tool::Select: {
                ImGui::TextWrapped("Click an object to select it, drag the gizmo to move or turn it.");
                if (ImGui::RadioButton("Translate (G)", settings.gizmoOperation == GizmoOperation::Translate)) settings.gizmoOperation = GizmoOperation::Translate;
                ImGui::SameLine();
                if (ImGui::RadioButton("Rotate (R)", settings.gizmoOperation == GizmoOperation::Rotate)) settings.gizmoOperation = GizmoOperation::Rotate;
                if (ImGui::RadioButton("World axes", settings.gizmoSpace == GizmoSpace::World)) settings.gizmoSpace = GizmoSpace::World;
                ImGui::SameLine();
                if (ImGui::RadioButton("Local axes", settings.gizmoSpace == GizmoSpace::Local)) settings.gizmoSpace = GizmoSpace::Local;

                const scene::Entity selected = self.Selection(scene);
                if (selected != scene::kNoEntity) ImGui::Text("Selected: %s", EntityLabel(scene, selected).c_str());
                else ImGui::TextDisabled("Nothing selected");
                ImGui::BeginDisabled(selected == scene::kNoEntity);
                const bool remove = ImGui::Button("Delete (Del)##select");
                ImGui::SameLine();
                const bool duplicate = ImGui::Button("Duplicate##select");
                ImGui::EndDisabled();
                if (remove) self.DeleteSelection(engine);
                else if (duplicate) self.DuplicateSelection(engine);
                break;
            }
            case Tool::Brush: {
                BrushSettings& brush = settings.brush;
                const float voxelSize = engine.World().VoxelSize();
                ImGui::TextWrapped("Hold the left button on the terrain.");
                EnumCombo("Mode##brush", brush.mode, kBrushModeNames);
                EnumCombo("Shape##brush", brush.shape, kBrushShapeNames);
                ImGui::SliderFloat("Radius##brush", &brush.radius, std::min(voxelSize, kMaxBrushRadius), kMaxBrushRadius, "%.2f m", ImGuiSliderFlags_Logarithmic);
                ImGui::BeginDisabled(brush.mode != BrushMode::Add);
                MaterialCombo("Material##brush", materials, brush.material);
                ImGui::EndDisabled();
                ImGui::SliderFloat("Spacing##brush", &brush.spacing, 0.05f, 4.0f, "%.2f x radius");
                break;
            }
            case Tool::Spawn: {
                PrimitiveSettings& primitive = settings.primitive;
                ImGui::TextWrapped("Click a surface to drop the primitive on it.");
                EnumCombo("Shape##primitive", primitive.shape, kPrimitiveNames);
                if (ImGui::DragInt3("Size##primitive", &primitive.size.x, 0.25f, 1, kMaxPrimitiveSize, "%d vox")) {
                    primitive.size = glm::clamp(primitive.size, glm::ivec3(1), glm::ivec3(kMaxPrimitiveSize));
                }
                MaterialCombo("Material##primitive", materials, primitive.material);
                EnumCombo("Motion##spawn", settings.spawnMotion, kMotionNames);
                ImGui::SliderFloat("Distance##spawn", &settings.spawnDistance, 0.5f, 50.0f, "%.1f m");
                if (ImGui::Button("Spawn in front##spawn")) self.SpawnPrimitive(engine);

                ImGui::SeparatorText("Import .vox / .gltf / .glb");
                ImportSettings& import = settings.import;
                if (import.file != importPathShown) {
                    // The settings were changed from outside: show the new path.
                    const std::string text = import.file.string();
                    const size_t count = std::min(text.size(), importPath.size() - 1);
                    std::memcpy(importPath.data(), text.data(), count);
                    importPath[count] = '\0';
                    importPathShown = import.file;
                }
                if (ImGui::InputText("File##import", importPath.data(), importPath.size())) {
                    import.file = std::filesystem::path(importPath.data());
                    importPathShown = import.file;
                }
                ImGui::DragFloat("Voxels per unit##import", &import.voxelsPerUnit, 0.1f, 0.1f, 200.0f, "%.1f");
                ImGui::Checkbox("Fill interior##import", &import.fillInterior);
                ImGui::SliderInt("Max voxels / axis##import", &import.maxVoxelsPerAxis, 8, 512);
                if (ImGui::Button("Import##import")) self.ImportFile(engine);
                break;
            }
        }

        ImGui::SeparatorText("Camera");
        ImGui::SliderFloat("Fly speed", &settings.fly.speed, 0.5f, 60.0f, "%.1f m/s", ImGuiSliderFlags_Logarithmic);
        ImGui::SliderFloat("Fast (Shift)", &settings.fly.fastMultiplier, 1.0f, 10.0f, "%.1f x");

        if (!status.empty()) {
            ImGui::Separator();
            if (statusError) {
                ImGui::PushStyleColor(ImGuiCol_Text, kErrorColor);
                ImGui::TextWrapped("%s", status.c_str());
                ImGui::PopStyleColor();
            } else {
                ImGui::TextWrapped("%s", status.c_str());
            }
        }
    }

    void Editor::Impl::ScenePanel(Editor& self, Engine& engine) {
        scene::Scene& scene = engine.Scene();
        // A snapshot first: selecting must not happen while a registry view is being walked.
        entities.clear();
        for (const scene::Entity entity : scene.Registry().view<scene::Transform>()) entities.push_back(entity);
        std::sort(entities.begin(), entities.end(), [](scene::Entity a, scene::Entity b) { return entt::to_entity(a) < entt::to_entity(b); });

        const scene::Entity selected = self.Selection(scene);
        ImGui::Text("%zu entities, %zu voxel objects", entities.size(), scene.VoxelObjectCount());
        ImGui::BeginDisabled(selected == scene::kNoEntity);
        if (ImGui::SmallButton("Deselect")) self.Select(scene::kNoEntity);
        ImGui::EndDisabled();
        ImGui::Separator();

        ImGuiListClipper clipper;
        clipper.Begin(static_cast<int>(entities.size()));
        while (clipper.Step()) {
            for (int i = clipper.DisplayStart; i < clipper.DisplayEnd; ++i) {
                const scene::Entity entity = entities[static_cast<size_t>(i)];
                ImGui::PushID(static_cast<int>(entt::to_integral(entity)));
                if (ImGui::Selectable(EntityLabel(scene, entity).c_str(), entity == selected)) self.Select(entity);
                ImGui::PopID();
            }
        }
        clipper.End();
    }

    void Editor::Impl::InspectorPanel(Editor& self, Engine& engine) {
        scene::Scene& scene = engine.Scene();
        const scene::Entity entity = self.Selection(scene);
        if (entity == scene::kNoEntity) {
            ImGui::TextDisabled("Nothing selected");
            return;
        }
        const EntityInfo info = DescribeEntity(scene, entity);
        ImGui::Text("%s", EntityLabel(scene, entity).c_str());
        ImGui::Text("Name: %s", info.name.empty() ? "(none)" : info.name.c_str());
        if (info.hasBody) ImGui::Text("Motion: %s", MotionName(info.motion));
        else ImGui::TextUnformatted("Motion: no body");
        if (info.hasVoxels) {
            ImGui::Text("Voxels: %u  (%d x %d x %d)", info.voxelCount, info.sizeVoxels.x, info.sizeVoxels.y, info.sizeVoxels.z);
        } else {
            ImGui::TextUnformatted("Voxels: none");
        }

        if (info.hasTransform) {
            glm::vec3 position = info.position;
            if (ImGui::DragFloat3("Position##inspector", &position.x, 0.05f, 0.0f, 0.0f, "%.2f")) {
                scene.SetTransform(entity, position, info.rotation, true);
                if (const scene::RigidBody* body = scene.TryGet<scene::RigidBody>(entity)) {
                    engine.Physics().SetVelocity(body->body, glm::vec3(0.0f), glm::vec3(0.0f));
                }
            }
            const glm::vec3 euler = glm::degrees(glm::eulerAngles(info.rotation));
            ImGui::Text("Rotation: %.1f  %.1f  %.1f deg", D(euler.x), D(euler.y), D(euler.z));
        }

        if (scene::PointLight* light = scene.TryGet<scene::PointLight>(entity)) {
            ImGui::SeparatorText("Light");
            ImGui::ColorEdit3("Colour##light", &light->color.x, ImGuiColorEditFlags_Float);
            ImGui::DragFloat("Intensity##light", &light->intensity, 0.05f, 0.0f, 1000.0f, "%.2f");
            ImGui::DragFloat("Range##light", &light->radius, 0.05f, 0.1f, 200.0f, "%.2f m");
            ImGui::DragFloat("Source size##light", &light->sourceRadius, 0.005f, 0.0f, 5.0f, "%.3f m");
        }

        // Last: nothing above may be used once the entity is gone.
        ImGui::Separator();
        const bool remove = ImGui::Button("Delete##inspector");
        ImGui::SameLine();
        const bool duplicate = ImGui::Button("Duplicate##inspector");
        if (remove) self.DeleteSelection(engine);
        else if (duplicate) self.DuplicateSelection(engine);
    }

    void Editor::Impl::EnvironmentPanel(Engine& engine) {
        if (ImGui::CollapsingHeader("Sun", ImGuiTreeNodeFlags_DefaultOpen)) {
            graphics::SunLight& sun = engine.sun;
            SunAngles angles = SunAnglesFromDirection(sun.direction);
            // Straight up or down has no azimuth: keep the slider where the user left it.
            if (std::abs(std::cos(angles.elevation)) < 1.0e-3f) angles.azimuth = sunAzimuth;
            else sunAzimuth = angles.azimuth;
            bool changed = ImGui::SliderAngle("Azimuth##sun", &angles.azimuth, -180.0f, 180.0f, "%.0f deg");
            changed |= ImGui::SliderAngle("Elevation##sun", &angles.elevation, -90.0f, 90.0f, "%.0f deg");
            if (changed) {
                sunAzimuth = angles.azimuth;
                sun.direction = SunDirectionFromAngles(angles);
            }
            ImGui::ColorEdit3("Colour##sun", &sun.color.x, ImGuiColorEditFlags_Float);
            ImGui::SliderFloat("Intensity##sun", &sun.intensity, 0.0f, 20.0f, "%.2f");
            ImGui::SliderFloat("Angular radius##sun", &sun.angularRadius, 0.0f, 0.2f, "%.3f rad");
        }
        if (ImGui::CollapsingHeader("Sky", ImGuiTreeNodeFlags_DefaultOpen)) {
            graphics::SkySettings& sky = engine.sky;
            ImGui::ColorEdit3("Zenith##sky", &sky.zenith.x, ImGuiColorEditFlags_Float);
            ImGui::ColorEdit3("Horizon##sky", &sky.horizon.x, ImGuiColorEditFlags_Float);
            ImGui::ColorEdit3("Ground##sky", &sky.ground.x, ImGuiColorEditFlags_Float);
        }
        if (ImGui::CollapsingHeader("Fog", ImGuiTreeNodeFlags_DefaultOpen)) {
            graphics::FogSettings& fog = engine.fog;
            ImGui::Checkbox("Enabled##fog", &engine.Renderer().Settings().fog);
            ImGui::ColorEdit3("Colour##fog", &fog.color.x, ImGuiColorEditFlags_Float);
            ImGui::DragFloat("Density##fog", &fog.density, 0.0002f, 0.0f, 1.0f, "%.4f /m");
            ImGui::DragFloat("Height falloff##fog", &fog.heightFalloff, 0.001f, 0.0f, 2.0f, "%.3f /m");
            ImGui::DragFloat("Base height##fog", &fog.baseHeight, 0.1f, -1000.0f, 1000.0f, "%.1f m");
            ImGui::DragFloat("Start distance##fog", &fog.startDistance, 0.1f, 0.0f, 1000.0f, "%.1f m");
            ImGui::SliderFloat("Max opacity##fog", &fog.maxOpacity, 0.0f, 1.0f, "%.2f");
        }
    }

    void Editor::Impl::RenderPanel(Engine& engine) {
        graphics::RenderSettings& rs = engine.Renderer().Settings();
        if (ImGui::CollapsingHeader("Settings", ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::SliderFloat("Render scale", &rs.renderScale, 0.25f, 1.0f, "%.2f");
            ImGui::Checkbox("VSync", &rs.vsync);
            ImGui::Checkbox("TAA", &rs.taa);
            ImGui::Checkbox("Sun shadows", &rs.sunShadows);
            ImGui::Checkbox("Ambient occlusion", &rs.ambientOcclusion);
            ImGui::Checkbox("Emissive lighting", &rs.emissiveLighting);
            ImGui::Checkbox("Point light shadows", &rs.lightShadows);
            ImGui::Checkbox("Fog", &rs.fog);
            ImGui::Checkbox("Spatial filter", &rs.spatialFilter);
            ImGui::Checkbox("Checkerboard lighting", &rs.checkerboardLighting);
            ImGui::SliderFloat("Ambient distance", &rs.ambientDistance, 0.5f, 30.0f, "%.1f m");
            ImGui::SliderFloat("Ambient intensity", &rs.ambientIntensity, 0.0f, 3.0f, "%.2f");
            ImGui::SliderFloat("Emissive scale", &rs.emissiveScale, 0.0f, 5.0f, "%.2f");
            ImGui::SliderFloat("Exposure", &rs.exposure, 0.05f, 8.0f, "%.2f", ImGuiSliderFlags_Logarithmic);
            ImGui::SliderFloat("Lighting history", &rs.lightingHistory, 1.0f, 64.0f, "%.0f frames");
            EnumCombo("Debug view", rs.debugView, kDebugViewNames);
            int uploads = static_cast<int>(std::min<uint32_t>(rs.maxChunkUploadsPerFrame, 4096u));
            if (ImGui::SliderInt("Chunk uploads / frame", &uploads, 1, 1024)) rs.maxChunkUploadsPerFrame = static_cast<uint32_t>(std::max(uploads, 1));
        }
        if (ImGui::CollapsingHeader("Statistics", ImGuiTreeNodeFlags_DefaultOpen)) {
            const graphics::RenderStats& stats = engine.Renderer().Stats();
            ImGui::Text("Render %u x %u, output %u x %u", stats.renderSize.x, stats.renderSize.y, stats.outputSize.x, stats.outputSize.y);
            ImGui::Text("Instances: %u drawn of %u", stats.instancesDrawn, stats.instancesTotal);
            ImGui::Text("Chunk uploads: %u (%u pending)", stats.chunkUploads, stats.chunkUploadsPending);
            ImGui::Text("Lights: %u   particles: %u", stats.lights, stats.particles);
            ImGui::Text("GPU memory: %.1f MiB", static_cast<double>(stats.gpuMemoryBytes) / (1024.0 * 1024.0));
            ImGui::Text("Frame %llu", static_cast<unsigned long long>(stats.frameIndex));
            float total = 0.0f;
            for (const graphics::PassTiming& pass : stats.passes) {
                ImGui::Text("GPU %-12s %6.2f ms", pass.name.c_str(), D(pass.milliseconds));
                total += pass.milliseconds;
            }
            ImGui::Text("GPU %-12s %6.2f ms", "total", D(total));
        }
    }

    void Editor::Impl::ProfilerPanel() {
        ImGui::Text("%.1f fps (%.2f ms)", D(ImGui::GetIO().Framerate), D(1000.0f / std::max(ImGui::GetIO().Framerate, 1.0e-3f)));
        const std::vector<Profiler::Timer> timers = Profiler::SnapshotTimers();
        if (ImGui::CollapsingHeader("Timers", ImGuiTreeNodeFlags_DefaultOpen)) {
            if (timers.empty()) ImGui::TextDisabled("No samples yet");
            for (size_t i = 0; i < timers.size(); ++i) {
                const Profiler::Timer& timer = timers[i];
                ImGui::PushID(static_cast<int>(i));
                ImGui::Text("%-24s %6.2f ms   avg %6.2f   max %6.2f", timer.name.c_str(), D(timer.lastMs), D(timer.avgMs), D(timer.maxMs));
                // A full ring starts at the cursor (the oldest sample); a partial one at 0.
                const int count = std::clamp(timer.samples, 0, Profiler::kHistory);
                const int offset = count == Profiler::kHistory ? std::clamp(timer.cursor, 0, Profiler::kHistory - 1) : 0;
                if (count > 0) {
                    ImGui::PlotLines("##history", timer.history.data(), count, offset, nullptr, 0.0f, std::max(timer.maxMs, 0.1f),
                                     ImVec2(ImGui::GetContentRegionAvail().x, 28.0f));
                }
                ImGui::PopID();
            }
        }
        if (ImGui::CollapsingHeader("Counters", ImGuiTreeNodeFlags_DefaultOpen)) {
            const std::vector<Profiler::Counter> counters = Profiler::SnapshotCounters();
            if (counters.empty()) ImGui::TextDisabled("No counters");
            for (const Profiler::Counter& counter : counters) ImGui::Text("%-24s %.6g", counter.name.c_str(), counter.value);
        }
    }

    void Editor::Impl::WorldPanel(Engine& engine) {
        voxel::World& world = engine.World();
        const voxel::WorldStats stats = world.Stats();
        const glm::ivec3 window = world.WindowSize();
        ImGui::Text("Chunks: %u dense, %u uniform, %u empty", stats.denseChunks, stats.uniformChunks, stats.emptyChunks);
        ImGui::Text("Loading: %u pending, %u jobs", stats.pendingChunks, stats.jobsInFlight);
        ImGui::Text("Chunk store: %u / %u", engine.Chunks().UsedCount(), engine.Chunks().Capacity());
        ImGui::Text("Window: %d x %d x %d chunks, voxel %.2f m", window.x, window.y, window.z, D(world.VoxelSize()));
        ImGui::Text("Bodies: %u (%u awake, %u frozen without terrain)", engine.Physics().BodyCount(), engine.Physics().ActiveBodyCount(),
                    engine.Physics().FrozenBodyCount());
        ImGui::Text("Rubble: %zu of %u", engine.Scene().DebrisCount(), engine.Scene().Config().maxDebris);
        ImGui::Text("Entities: %zu   particles: %u", engine.Scene().EntityCount(), engine.Particles().AliveCount());
        const glm::dvec3 eye = engine.camera.position;
        ImGui::Text("Camera: %.1f  %.1f  %.1f", eye.x, eye.y, eye.z);

        const bool persistent = !world.Config().saveDirectory.empty();
        ImGui::BeginDisabled(!persistent);
        const bool save = ImGui::Button("Save world");
        ImGui::EndDisabled();
        if (!persistent) {
            ImGui::SameLine();
            ImGui::TextDisabled("(no save directory)");
        }
        if (save) {
            world.Save();
            SetStatus(std::format("World saved to '{}'", world.Config().saveDirectory.string()));
        }
    }

    void Editor::Impl::Gizmo(Editor& self, Engine& engine) {
        scene::Scene& scene = engine.Scene();
        const scene::Entity entity = self.Selection(scene);
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const scene::Transform* transform = entity != scene::kNoEntity ? scene.TryGet<scene::Transform>(entity) : nullptr;
        const bool shown = tool == Tool::Select && transform != nullptr && !looking && display.x > 0.0f && display.y > 0.0f;
        // ImGuizmo only ends a drag inside Manipulate (when the button is up). A drag that is still
        // marked as running when Manipulate is called again - on another entity, or after frames
        // without a gizmo (other tool, camera look) - would move the object to wherever the cursor
        // is by then, so it is dropped here whenever the gizmo is not shown.
        if (cancelGizmo || entity != gizmoEntity || (!shown && ImGuizmo::IsUsingAny())) {
            // The editor uses no ImGuizmo id scopes, so this resets whatever drag is in progress.
            ImGuizmo::Enable(false);
            ImGuizmo::Enable(true);
            cancelGizmo = false;
        }
        gizmoEntity = entity;
        if (!shown) return;

        const math::Transform pose = transform->current;
        const glm::vec3 pivot = GizmoPivot(scene, entity);
        const glm::mat4 view = GizmoView(engine.camera);
        const glm::mat4 projection = GizmoProjection(engine.camera, ViewAspect(glm::vec2(display.x, display.y)));
        glm::mat4 model = GizmoModelMatrix(pose, pivot);

        const ImGuizmo::OPERATION operation = settings.gizmoOperation == GizmoOperation::Rotate ? ImGuizmo::ROTATE : ImGuizmo::TRANSLATE;
        const ImGuizmo::MODE mode = settings.gizmoSpace == GizmoSpace::Local ? ImGuizmo::LOCAL : ImGuizmo::WORLD;
        const bool changed = ImGuizmo::Manipulate(glm::value_ptr(view), glm::value_ptr(projection), operation, mode, glm::value_ptr(model));
        if (changed) {
            const math::Transform moved = TransformFromGizmo(model, pivot, pose.scale);
            scene.SetTransform(entity, moved.position, moved.rotation, false);
        }
        if (changed || ImGuizmo::IsUsing()) {
            // A body that is being dragged is held still: gravity must not pull it off the gizmo.
            if (const scene::RigidBody* body = scene.TryGet<scene::RigidBody>(entity)) {
                engine.Physics().SetVelocity(body->body, glm::vec3(0.0f), glm::vec3(0.0f));
            }
        }
    }

    void Editor::Impl::Overlays(Engine& engine) {
        if (!hoverValid || looking || (tool != Tool::Brush && tool != Tool::Spawn)) return;
        const ImVec2 size = ImGui::GetIO().DisplaySize;
        if (!(size.x > 0.0f) || !(size.y > 0.0f)) return;
        const glm::vec2 display(size.x, size.y);
        const graphics::Camera& camera = engine.camera;
        ImDrawList* draw = ImGui::GetBackgroundDrawList();

        const std::optional<glm::vec2> hit = WorldToCursor(camera, hoverPosition, display);
        if (!hit) return;
        const ImVec2 at(hit->x, hit->y);

        if (tool == Tool::Brush) {
            // The outline of the brush as seen from the camera, around where it will be applied.
            const float voxelSize = engine.World().VoxelSize();
            const float radius = std::clamp(settings.brush.radius, std::min(voxelSize, kMaxBrushRadius), kMaxBrushRadius);
            const glm::dvec3 center = BrushCenter(settings.brush, hoverPosition, hoverNormal, voxelSize);
            const std::optional<glm::vec2> middle = WorldToCursor(camera, center, display);
            const std::optional<glm::vec2> edge = WorldToCursor(camera, center + glm::dvec3(camera.Up() * radius), display);
            if (middle && edge) {
                const float pixels = std::clamp(glm::distance(*middle, *edge), 2.0f, 4.0f * std::max(size.x, size.y));
                const ImVec2 c(middle->x, middle->y);
                if (settings.brush.shape == BrushShape::Box) {
                    draw->AddRect(ImVec2(c.x - pixels, c.y - pixels), ImVec2(c.x + pixels, c.y + pixels), kPreviewShadow, 0.0f, 0, 3.0f);
                    draw->AddRect(ImVec2(c.x - pixels, c.y - pixels), ImVec2(c.x + pixels, c.y + pixels), kPreviewColor, 0.0f, 0, 1.5f);
                } else {
                    draw->AddCircle(c, pixels, kPreviewShadow, 48, 3.0f);
                    draw->AddCircle(c, pixels, kPreviewColor, 48, 1.5f);
                }
            }
        }

        // The hit point and the surface normal.
        draw->AddCircleFilled(at, 4.0f, kPreviewShadow, 12);
        draw->AddCircleFilled(at, 2.5f, kPreviewColor, 12);
        if (const std::optional<glm::vec2> tip = WorldToCursor(camera, hoverPosition + glm::dvec3(hoverNormal * 0.5f), display)) {
            draw->AddLine(at, ImVec2(tip->x, tip->y), kPreviewColor, 1.5f);
        }
    }
}
