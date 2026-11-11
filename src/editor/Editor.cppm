module;

#include <glm/glm.hpp>

#include <cstdint>
#include <memory>
#include <string_view>

export module vortex.editor;

export import :view;
export import :tools;

import vortex.engine;

export namespace vortex::editor {

    /// @brief The mouse tools; exactly one is active.
    enum class Tool : uint8_t {
        Select,     ///< Click picks an entity; gizmo, inspector, delete, duplicate.
        Brush,      ///< Hold the button to add / remove terrain.
        Spawn,      ///< Click drops a primitive on the surface under the cursor; import by path.
    };
    inline constexpr int kToolCount = 3;

    enum class GizmoOperation : uint8_t { Translate, Rotate };
    enum class GizmoSpace : uint8_t { World, Local };

    /// @brief Everything the tool panels edit. Plain data: a game may preset or persist it.
    struct EditorSettings {
        Key toggleKey = Key::F1;                 ///< Opens / closes the editor (handled in Editor::OnUpdate).
        FlySettings fly;                         ///< Editor camera.
        float reach = 200.0f;                    ///< Length of the cursor ray in metres.

        GizmoOperation gizmoOperation = GizmoOperation::Translate;
        GizmoSpace gizmoSpace = GizmoSpace::World;

        BrushSettings brush;

        PrimitiveSettings primitive;
        physics::MotionType spawnMotion = physics::MotionType::Dynamic;   ///< Motion type of spawned / imported objects.
        float spawnDistance = 4.0f;              ///< Metres in front of the camera for "spawn in front".
        ImportSettings import;                   ///< Path and options of the import form.
    };

    /// @brief Which panels are shown while the editor is open (the main menu bar toggles them).
    /// The tool panels start open; the others are opened from the "Panels" menu so that the view
    /// is not covered the first time the editor comes up.
    struct EditorPanels {
        bool tools = true;          ///< Tool choice and the active tool's options.
        bool scene = true;          ///< Entity list.
        bool inspector = true;      ///< The selected entity.
        bool environment = false;   ///< Sun, sky, fog.
        bool render = false;        ///< RenderSettings, debug view, GPU pass timings.
        bool profiler = false;      ///< vortex::Profiler timers and counters.
        bool world = false;         ///< World / store / physics statistics, Save.

        /// @brief Opens or closes every panel at once (a headless smoke run opens them all).
        void SetAll(bool open) { tools = scene = inspector = environment = render = profiler = world = open; }
    };

    /**
     * @brief The in-game editor: tools, gizmo and panels on top of a running vortex::Engine.
     *
     * @details **Layering.** The editor sits above the engine (`vortex.editor` imports
     * `vortex.engine`); the engine knows nothing about it. A game owns an Editor and forwards two
     * of its Application callbacks. Everything that is logic rather than widgets is a free
     * function of the `:view` and `:tools` partitions and needs neither ImGui nor a GPU; this
     * class adds input handling, ImGui panels and the ImGuizmo gizmo.
     *
     * **Frame protocol.** Engine::Run opens the ImGui frame before, and closes it after, both
     * callbacks, so ImGui state may be read in OnUpdate and widgets are submitted in OnGui:
     * @code
     *   window.PollEvents()                      // -> Input
     *   ui.BeginFrame()                          // ImGui::NewFrame
     *   ui.UpdateInputCapture(input)             // Input hides what ImGui wants
     *   streaming; fixed steps (app.OnFixedUpdate, physics, scene.SyncFromPhysics)
     *   app.OnUpdate:   editor.OnUpdate(engine, dt);            // FIRST
     *                   if (!editor.CapturesInput()) { game input, game camera }
     *   particles.Update()
     *   app.OnGui:      game windows; editor.OnGui(engine);     // any order
     *   ui.EndFrame()                            // ImGui::Render
     *   scene.Gather*, renderer.RenderFrame()
     * @endcode
     *  - OnUpdate comes first in the game's OnUpdate so that the toggle key, the editor camera
     *    and CapturesInput() are settled before the game looks at input in the same frame.
     *  - OnUpdate does input and tool actions (pick, brush, spawn): the world and the scene are
     *    edited at the point of the frame where game logic edits them, before the particle
     *    update and the scene gather, so an edit is visible in the frame it was made.
     *  - OnGui calls ImGuizmo::BeginFrame() first (it must run after ImGui::NewFrame and before
     *    Manipulate, once per frame), then submits the panels and the gizmo. A gizmo change is
     *    written with Scene::SetTransform before the gather of the same frame.
     *  - While the editor is closed OnGui makes no ImGui call at all, and OnUpdate only watches
     *    the toggle key.
     *  - Headless (no window): the ImGui context exists and nothing is drawn; both functions run
     *    unchanged (there is no cursor, so no tool acts on its own). Every open panel is
     *    submitted every frame, which makes a headless run with the editor open and
     *    `Panels().SetAll(true)` a smoke test of the UI.
     *
     * **Input.** There is one mouse and three parties. Per frame, in this order:
     *  1. *ImGui* - when the cursor is over a panel or a widget is active, `Input` reports no
     *     mouse buttons / scroll (and no keys while a text field has the focus). That is done by
     *     Engine::Run through UiLayer::UpdateInputCapture; the editor reads mouse and keys only
     *     through `Input`, so a click on a panel never reaches a tool.
     *  2. *Camera* - holding the right button looks around: the cursor is captured
     *     (Window::SetCursorCaptured), ImGui is told to ignore the mouse
     *     (ImGuiConfigFlags_NoMouse, because the GLFW backend keeps reporting the hidden cursor)
     *     and tools are suspended until the button is released. W A S D, Space / Left Ctrl and
     *     Shift fly the camera whenever the keyboard is free.
     *  3. *Gizmo* - with the Select tool and a valid selection, the left button belongs to
     *     ImGuizmo while it is hovered or dragged (ImGuizmo::IsOver(), which reports the previous
     *     frame's hover and any drag in progress); no pick happens.
     *  4. *Tool* - otherwise the left button drives the active tool along the cursor ray.
     *  The *game* comes last: while CapturesInput() is true it must not react to mouse or
     *  keyboard, must not move `engine.camera` and must not touch the cursor capture. One
     *  exception: a game pause menu may skip OnUpdate while it is open; it calls ReleaseInput()
     *  when it opens, which ends the look (cursor and ImGui mouse given back) and a stroke. Shortcuts
     *  (keyboard free): toggle key, 1 / 2 / 3 choose the tool, G / R gizmo translate / rotate,
     *  Delete removes the selection.
     *
     * **Cursor and matrices.** The cursor position and viewport size come from ImGui
     * (io.MousePos, io.DisplaySize) - the same coordinates ImGuizmo is given with
     * SetRect(0, 0, DisplaySize) - and the ray is CursorToRay(engine.camera, ...). ImGuizmo gets
     * `GizmoView(camera)` and `GizmoProjection(camera, aspect)` (a conventional right-handed,
     * y-up, 0..1 depth projection in world space - never the engine's reverse-Z Vulkan matrix)
     * and `GizmoModelMatrix(transform, GizmoPivot(...))` as the object matrix; the result goes
     * back through TransformFromGizmo and Scene::SetTransform(entity, position, rotation,
     * teleport = false), and a dragged body is held still (zero velocity). ProjectToCursor is the
     * inverse of CursorToRay for these matrices, so the gizmo sits where the object is rendered.
     *
     * **Selection.** The selection is a scene::Entity id, never a pointer. Ids carry a version,
     * so when the entity is destroyed - by the editor, an explosion, a lifetime - the id goes
     * stale and Selection() returns kNoEntity from then on; nothing has to be notified. An entity
     * that is damaged but survives keeps its id and stays selected. Whenever the selection
     * changes or dies, a gizmo drag in progress is cancelled (ImGuizmo::Enable(false / true); the
     * editor draws a single gizmo and uses no ImGuizmo id scopes, so that reliably resets it) and
     * can never continue on another entity.
     *
     * Main-thread only. One Editor per Engine; it keeps no reference to the engine between calls.
     */
    class Editor {
    public:
        Editor();
        ~Editor();
        Editor(const Editor&) = delete;
        Editor& operator=(const Editor&) = delete;

        // --- state -------------------------------------------------------------------------------

        bool IsOpen() const;
        /// @brief Opens or closes the editor. Takes effect immediately for IsOpen / CapturesInput;
        /// the cursor is released by the next OnUpdate. Closing ends a brush stroke, a camera look
        /// and a gizmo drag; the selection and all settings are kept.
        void SetOpen(bool open);
        void Toggle();

        /// @brief True while the editor owns mouse, keyboard, camera and cursor capture (= while it
        /// is open). The game checks this after calling OnUpdate and skips its own input handling
        /// and camera control for the frame; simulation and automation keep running.
        bool CapturesInput() const;

        Tool ActiveTool() const;
        /// @brief Switches the tool; ends a brush stroke in progress.
        void SetTool(Tool tool);

        EditorSettings& Settings();
        const EditorSettings& Settings() const;
        EditorPanels& Panels();

        // --- per frame (see the frame protocol above) --------------------------------------------

        /// @brief Toggle key, editor camera, cursor capture, shortcuts and the active tool's mouse
        /// action. Call first thing in Application::OnUpdate, every frame, open or not.
        void OnUpdate(Engine& engine, float dt);

        /// @brief The game takes mouse and keyboard away (its pause menu opens and OnUpdate will
        /// not run until it closes): ends the right-button look - releases the cursor and clears
        /// the editor's ImGuiConfigFlags_NoMouse - ends a brush stroke and drops the tool's hover
        /// preview. The editor stays open; the next OnUpdate starts a look only on a new press.
        void ReleaseInput(Engine& engine);

        /// @brief ImGuizmo frame, menu bar, panels, gizmo and tool overlays. Call from
        /// Application::OnGui, every frame; does nothing while closed.
        void OnGui(Engine& engine);

        // --- selection ---------------------------------------------------------------------------

        /// @brief The selected entity if it is still alive in `scene`, otherwise kNoEntity.
        scene::Entity Selection(const scene::Scene& scene) const;
        /// @brief Selects an entity (kNoEntity clears). The id is not checked here; a stale one
        /// simply reads back as kNoEntity. Cancels a gizmo drag when the selection changes.
        void Select(scene::Entity entity);

        // --- actions (what the buttons, shortcuts and clicks do) ----------------------------------
        //  Each reports through Status(); none needs the editor to be open or an ImGui frame.

        /// @brief Destroys the selected entity. @return false if nothing valid was selected.
        bool DeleteSelection(Engine& engine);

        /// @brief DuplicateEntity of the selection, placed beside it (one model width along +X, at
        /// least 0.5 m), and selects the copy. @return The copy, or kNoEntity (see Status()).
        scene::Entity DuplicateSelection(Engine& engine);

        /// @brief Creates the primitive of Settings().primitive (world palette, Settings().spawnMotion)
        /// centred `spawnDistance` in front of the camera and selects it.
        /// @return kNoEntity if the volume is empty or the scene could not create it (see Status()).
        scene::Entity SpawnPrimitive(Engine& engine);

        /// @brief The same, resting on a surface point (see PlaceOnSurface). This is what a click
        /// with the Spawn tool does with the TraceSurface hit.
        scene::Entity SpawnPrimitiveAt(Engine& engine, const glm::dvec3& anchor, const glm::vec3& normal);

        /// @brief ImportModels(Settings().import) and one scene object per model, the group centred
        /// `spawnDistance` in front of the camera (models keep their relative offsets); selects the
        /// first. @return false on any failure, with the reason in Status(); models created before
        /// a failure (out of chunk slots) stay.
        bool ImportFile(Engine& engine);

        /// @brief One brush application at the surface hit of `ray` against the terrain
        /// (World::Raycast up to Settings().reach), honouring the stroke spacing
        /// (StrokeBrushAlongRay: a resting cursor applies once); wakes bodies around the edit. This is what holding the button with the Brush tool does every frame.
        /// @return The result; `applied == false` also when the ray hits no terrain.
        BrushResult BrushAlong(Engine& engine, const CursorRay& ray);
        /// @brief Ends the current brush stroke (button released): the next BrushAlong applies.
        void EndStroke();

        /// @brief Result of the most recent action, for the status line ("" initially). Valid
        /// until the next action.
        std::string_view Status() const;
        /// @brief True if Status() describes a failure.
        bool StatusIsError() const;

    private:
        struct Impl;
        std::unique_ptr<Impl> m_Impl;
    };
}
