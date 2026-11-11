# Game UI and cursor capture (design, as built)

Owner's request: "rework interface, make lock mouse at center on play mode (at least at wayland).
Leave ImGui for debugging and map editor."

Status: implemented. Sections marked **Deviation** say where the build differs from the first
version of this design and why. Windowed behaviour (pointer lock on Wayland / X11, focus, menu
clicks, ImGui overlay with Alt) has not been confirmed on a real desktop yet: headless runs cannot
exercise it (see "Owner checks" at the end).

## Goals

- Play mode: cursor locked and hidden the whole time (`GLFW_CURSOR_DISABLED` + raw motion when
  supported). Mouse look without holding a button. Works on Wayland and X11.
- Escape opens a pause menu (Resume / Quit) and releases the cursor; it no longer quits.
- A game interface without ImGui: crosshair, small HUD (walk / fly, view, tool placeholder, FPS,
  key hints), pause menu. Drawn by a tiny engine-side screen-space layer.
- ImGui only for debugging (F3 overlay, hidden by default) and the map editor (F1). In play with
  no overlay, ImGui neither draws nor captures input.

## Module placement (respects layering)

| Part | Module | GPU | Tested |
|---|---|---|---|
| `HudCanvas` (quad list), `BitmapFont` (embedded glyphs, `Measure`), `Rgba`, `HudRect`, `PlaceRect` / `Anchor` / `Align` | partition `vortex.core:hud` (Core) | no | unit |
| `Menu` (items, focus, `HandleInput(MenuInput) -> MenuEvent`, `Draw`) | `vortex.core:hud` | no | unit |
| `CursorCapture` state machine (facts -> captured + edges, pause flag) | `vortex.core:hud` | no | unit |
| HUD pipeline (`hud.vert` / `hud.frag`, `u_Hud` binding 10, pass `hud`) | `vortex.graphics` (`Renderer`) | yes | contract tests + headless screenshot |
| `Engine::Hud()`, `HudCursor()`, `WindowFocused()`, `SetImGuiInput()` | `vortex.engine` | - | headless |
| Game wiring: pause menu, HUD content, cursor edges, F3 overlay | `src/main.cpp` | - | headless |

Core gets no GPU and no GLFW: the canvas is plain data, the state machines take booleans.

**Deviation (no separate `vortex.graphics:hud` / `HudRenderer`).** The HUD is one more pipeline of
the renderer's `PipelineSet` with its quads in a per-frame storage buffer, like particles. It
reuses `BindPipeline`, `Upload`, the deletion queue, the timestamp pool and pipeline rebuilds on
a swapchain format change, which a separate class would have had to duplicate.

## API the game uses

```cpp
HudCanvas& hud = engine.Hud();                    // cleared and sized by the engine every frame
hud.Rect({x, y, w, h}, Rgba(0, 0, 0, 128));
hud.Text({x, y}, "FLY", Rgba::White(), /*scale, 0 = auto*/ 0, Align::Left, /*shadow*/ Rgba(0, 0, 0, 170));
glm::vec2 size = hud.Measure("Resume");
hud.Crosshair(Rgba::White());
hud.DimBackground(0.45f);                         // darken the 3D image under the HUD (free)

menu.Layout(hud.Size(), hud.TextScale());
MenuEvent e = menu.HandleInput(MenuInput::From(engine.GetInput(), engine.HudCursor(), cursorFree));
menu.Draw(hud);                                   // later in the frame, on top of the rest

CursorCapture::Inputs c;                          // focused, pointerInside (Window::IsHovered), editorOpen,
c.freeCursor = overlay && input.KeyDownRaw(Key::LeftAlt);   // raw: ImGui must not flip it mid-drag
c.leftButtonDown = input.MouseDownRaw(MouseButton::Left); c.dt = dt; // ... escape, headless
CursorCapture::Result r = capture.Update(c);
if (r.justPaused) { menu.Open(); editor.ReleaseInput(engine); }
if (r.justCaptured) window->SetCursorCaptured(true); else if (r.justReleased) window->SetCursorCaptured(false);
engine.SetImGuiInput(editorOpen || (overlay && !r.captured));
if (r.clicks && input.MousePressed(MouseButton::Left)) Shoot();
```

**Deviation.** The first design had `MenuState::Update(hud, input, visible)` doing input and
drawing at once, and `engine.SetPlayCapture(bool)`. Input and drawing are split so the game can
react to the menu (resume, quit) before anything else reads input in the frame, and draw the menu
last, on top of the HUD. The capture edges are applied by the game through the existing
`Window::SetCursorCaptured`, because the editor also drives that call during its right-button look
and only the game knows when the editor owns the cursor.

Coordinates are framebuffer pixels, y down; `Engine::HudCursor()` converts the window-coordinate
cursor (`Input::MouseX/Y`) with framebuffer / window size, which differs on HiDPI and scaled
Wayland outputs. Integer text scale `TextScale() = max(1, height / 360)` (2 at 720p, 3 at 1080p)
keeps the bitmap crisp.

## Cursor-capture state machine

`CursorCapture::Update(Inputs)`, once per frame, inputs `headless, focused, pointerInside,
editorOpen, freeCursor, escape, leftButtonDown, dt`; it owns the pause flag.

```
if (escape && !paused) paused = true                          // justPaused
wanted   = !headless && focused && !editorOpen && !paused && !freeCursor
captured = wanted && (captured last frame || pointerInside)   // starts only with the pointer over the window
justCaptured / justReleased = edges of captured
clicks   = captured && left button seen up >= kClickGrace (0.15 s) after justCaptured
```

**Deviation (review).** The capture starts only with the pointer over the window
(`Window::IsHovered`, GLFW_HOVERED) and is kept while wanted. On Wayland GLFW locks the pointer
only when it enters the surface, so a focused window with the pointer outside (Alt-Tab) used to
count as captured: no "Click to play", and the first click after moving in was a shot. Now the
entry is the capture edge. Game clicks (`clicks`) additionally wait until the button that brought
the game back has been released and 0.15 s have passed: focus-in and the button press can arrive
in different polls (X11 click-to-focus, Wayland keyboard / pointer objects), so blocking only the
`justCaptured` frame was not enough. Mouse look still starts one frame after the capture.

| From | Event | To | Cursor |
|---|---|---|---|
| start (windowed) | window focused | Playing | captured |
| start, unfocused | - | Playing (waiting, HUD says "Click to play") | free until the window gets the focus |
| Playing | Escape | Paused | released, menu shown |
| Paused | Escape (menu Back) or Resume | Playing | captured |
| Paused | Quit | exit | - |
| Paused | click outside the menu items | stays Paused | free |
| Playing | focus lost | Playing (waiting) | released |
| Playing (waiting) | focus gained (a click into the window gives it) and pointer over the window | Playing | captured; clicks blocked until the button is up and 0.15 s passed |
| Playing (waiting, focused) | pointer enters the window | Playing | captured (Wayland: the lock starts here) |
| any | F1 editor open | Editor | released; the editor owns it (its right-button look keeps working) |
| Editor | F1 close | Playing | captured on the same frame |
| Editor | Escape | Paused | released: the game calls `Editor::ReleaseInput`, which ends its look (cursor, ImGui `NoMouse`) |
| Playing | F3 overlay shown | Playing + overlay | captured; hold Left Alt to free the cursor and use ImGui (Alt read raw, `Input::KeyDownRaw`: ImGui has the keyboard during a slider drag and a filtered read would re-lock mid-drag) |
| Paused | focus lost | Paused | stays paused |
| any | headless | - | never captured; no GLFW calls |

- The Escape that closes the menu is consumed: the game passes `escape = false` on every frame
  that started paused, so it cannot reopen the menu on the same frame.
- While paused the game does not call `Editor::OnUpdate` (F1 and the tools are inert) and skips
  all game input, including on the frame the menu closes: the click on "Resume" does not also
  blast a hole. **The simulation is not paused** - sessions are networked; "pause" is a menu.
- Mouse look needs `captured && !justCaptured`; the left-button action needs `clicks` (see
  above): the click that gives the window the focus back is not a shot. Look reads `Input::MouseDelta*` only;
  `Window::SetCursorCaptured` resets the delta on every mode change, so there is no jump.
- **Deviation:** Escape pauses with the editor open too (the design only paused from play): the
  pause menu is now the only in-game way to quit, so it must be reachable from the editor.

## Wayland specifics (GLFW 3.4, read from `_deps/glfw-src/src/wl_window.c`)

- `GLFW_CURSOR_DISABLED` uses `zwp_pointer_constraints_v1` lock + `zwp_relative_pointer_v1`
  (`lockPointer`, ~line 2918). Relative motion is accumulated into a virtual cursor position and
  delivered through the normal cursor-pos callback (`relativePointerHandleRelativeMotion`),
  using the unaccelerated deltas when `GLFW_RAW_MOUSE_MOTION` is on (always supported on
  Wayland). So the delta-based `Input::OnCursor` works unchanged; absolute positions are never
  used for look. `Window::SetCursorCaptured` enables raw motion before the lock and disables it
  after the unlock.
- The lock is only applied while the pointer is over the surface (`wl.hovered`); otherwise the
  mode is stored and applied in `pointerHandleEnter`. `CursorCapture` therefore starts a capture
  only while `Window::IsHovered()`, so the HUD says "Click to play" until the pointer is in and
  the entry is the capture edge (clicks grace).
- If the compositor lacks `relative_pointer_manager`, `lockPointer` reports a GLFW error (logged by
  the window's error callback) and returns; the cursor is hidden but look does not move.
  **Deviation:** no fallback to `GLFW_CURSOR_HIDDEN` + absolute deltas was built: KDE, GNOME,
  sway and Hyprland all provide both protocols. The first capture logs the platform and whether
  raw motion is on (`Window: cursor captured for mouse look on Wayland (pointer lock), raw motion on`).
- `glfwSetCursorPos` is unsupported on Wayland: nothing calls it (ImGui only does with keyboard
  navigation, which is off). On release the compositor puts the pointer back where it was locked.
- Focus: capture is only requested while `GLFW_FOCUSED` is true, and re-requested when it
  becomes true again (the state machine re-evaluates every frame).
- X11: `GLFW_CURSOR_DISABLED` grabs and warps to the centre; raw motion via XInput2. Same code.
- ImGui's GLFW backend sets `GLFW_CURSOR_NORMAL` every frame unless the cursor is disabled or
  `ImGuiConfigFlags_NoMouseCursorChange` is set: play mode sets it (`SetImGuiInput(false)`).

## Input routing

- Order inside the game's `OnUpdate`: pause menu (if the frame started paused) -> editor (if not)
  -> F3 / F5 / F12 -> `CursorCapture::Update` + edges -> `SetImGuiInput` -> game input -> HUD.
- `UiLayer::SetInputEnabled(false)` (via `Engine::SetImGuiInput`) sets `ImGuiConfigFlags_NoMouse |
  NoMouseCursorChange` and makes `UpdateInputCapture` report no capture, so `io.WantCapture*` of a
  hidden window can never block keys. **Deviation (review):** the first version toggled the
  flags on each change and cleared both bits on enable, which could remove the editor's own
  `NoMouse` in the middle of its look. Now the layer remembers the bits it added, re-applies them
  at every `BeginFrame` while disabled and clears only those on enable; the editor sets its
  `NoMouse` on every frame of the look. ImGui input is on while the editor is open, and on the
  F3 overlay while the cursor is free (Alt held or paused).
- The pause menu: `Menu::Open()` on `justPaused` (focus on Resume, pointer unknown). The pointer
  counts only after it moves (`MenuInput::mouseMoved`, from the cursor delta, which is zero for
  the first position after a capture change), so the stale position of a just-released cursor
  can neither hover nor click "Quit"; and only a moving pointer takes the focus, so one resting
  on an item does not undo Up / Down every frame.
- ImGui frames still run every frame; with the overlay hidden and the editor closed no window is
  submitted and `UiLayer::Draw` returns early (`TotalVtxCount == 0`).
- The former "Shredborne" ImGui window is the F3 debug overlay ("Debug (F3)"), hidden by default.

## Rendering and cost

**Deviation (quads with cell bits instead of vertices + font texture).** A `HudQuad` is
`{x, y, w, h, colour, bitsLo, bitsHi, reserved}` (32 bytes, `VxHudQuad` in `common.glsl`). The
quad is covered by a 5 x 9 grid of cells; a cell is drawn when its bit is set. A solid rectangle
has all 45 bits set, a glyph has its bitmap. So there is no font atlas, no texture upload, no
sampler and no image layout to track: `hud.vert` expands quad `gl_InstanceIndex` into 6
vertices (`vkCmdDraw(6, quadCount, 0, 0)`, no vertex buffer), `hud.frag` tests one bit. One
pipeline, one draw call, premultiplied alpha, no depth, no `discard` (an unset cell writes 0).
Vertices are pixels mapped to NDC with `u_Frame.outputSize.zw`.

Order inside the backbuffer rendering scope: present (tone mapping) -> hud -> imgui, each with
its own timestamp interval (`GPU present / hud / imgui` in the headless log). Quads are uploaded
into a per-frame host-visible buffer (grows like the others; cap 8192 quads per frame = 256 KiB,
overflow dropped and logged once).

**Deviation (backdrop dim).** The menu's darkened backdrop was a full-screen blended quad: it
measured 0.15 ms on the UHD 620 at 1280x720. It is now `FrameUniforms::settings.w` (0..255,
`HudCanvas::DimBackground`), multiplied in by `present.frag`, which writes every pixel anyway:
no measurable cost.

Measured on the UHD 620, headless, 1280x720 output, median of 120 frames:

| Case | quads | GPU hud |
|---|---|---|
| play HUD (crosshair, mode panel, hints, fps) | 149 | 0.01 ms |
| pause menu (dim in present) | 71 | 0.01 ms |
| pause menu with a full-screen dim quad (first version) | 223 | 0.17 ms |
| `--no-hud` | 0 | 0.00 ms |

`GPU present` stayed at 0.42-0.43 ms with the dim. CPU cost of building the HUD is an estimate
(a few hundred `push_back`s and a 2-8 KiB copy): well under 0.02 ms; not measured separately.

## Font

Hand-drawn 5 x 9 bitmap font in `src/core/internal/HudFont.inc` (ASCII 32..126, rows of `.` /
`#`, parsed by a `consteval` function: a malformed glyph is a compile error). Capitals and digits
use rows 0..6, descenders rows 7..8. Advance 6 cells, line height 11 cells. Non-ASCII bytes
draw '?'. **Deviation:** the design proposed font8x8_basic; it was not on disk and a font drawn
for the engine has no licence question.

`Measure(text, scale) = {(longest line * 6 - 1) * scale, (lines * 11 - 2) * scale}`.

## Tests (GPU-free, ctest)

- `BitmapFont`: every glyph non-empty, distinct and inside the grid; shapes; non-ASCII -> '?';
  `Measure` with newlines and scale.
- `HudLayout`: all nine anchors with margins give exact rects; odd sizes are floored; `Contains`.
- `HudCanvas`: rect -> one solid quad; invisible / empty rects add nothing; text -> one quad per
  visible character, pixel-floored, advance and line height; alignment; shadow first; crosshair
  centred; overflow cap; `DimBackground`.
- `HudMenu`: layout centred and stacked; Up / Down wrap; Enter activates the focused item;
  Escape = Back (wins over anything else); hover focuses; click on an item activates it; click
  outside does nothing; a hidden cursor never hovers; a resting pointer does not override the
  keyboard; a stale pointer after `Open()` neither hovers nor clicks; `MenuInput::From` edges,
  `mouseMoved` and UI capture; `Draw` uses the backdrop dim, not a full-screen quad.
- `Input.RawQueriesIgnoreUiCapture`: `KeyDownRaw` / `MouseDownRaw` ignore ImGui capture.
- `CursorCapture`: every row of the table above, edges fire once, focus loss while paused keeps
  the pause, headless never captures, the capture starts only with the pointer inside, and the
  click that starts it (held, or pressed a frame later) is not a shot.
- Graphics contract: `u_Hud` binding, `hud` pipeline resources, `hud.frag` writes one float colour,
  `VxHudQuad` layout == `HudQuad`, `VX_HUD_CELLS_X/Y` == font grid.
- Headless: the game run logs `HUD: N quads drawn on the last frame` (149 in the default run) and
  its screenshot shows the HUD; `--pause-menu` starts with the menu open (screenshot of the
  menu); `--no-hud` draws nothing (`HUD: 0 quads`, screenshot differs).

## Out of scope

- Textured / skinned widgets, TTF fonts, Unicode, localisation, gamepad navigation, settings menu
  (key rebinding, sensitivity slider: later, sensitivity stays a constant), scaling DPI beyond the
  integer scale, in-game chat (M1 networking), confined (not locked) cursor modes.

## Owner checks (windowed; an assistant cannot run them)

On Wayland, and on X11 through XWayland: GLFW follows `XDG_SESSION_TYPE` when the matching display
variable is set (`platform.c`), so `XDG_SESSION_TYPE=x11 ./build/Shredborne` runs the X11 path in a
Wayland session (the log line then says `X11 (pointer grab)`):
1. Start the game: once the window has the focus the cursor disappears and moving the mouse turns
   the camera without any button; it never hits a screen edge. The log has
   `Window: cursor captured for mouse look on Wayland (pointer lock), raw motion on`.
2. Escape: the menu appears, the cursor is visible and free. Hover / click Resume and Quit;
   Up / Down / Enter, also with the pointer resting on an item (the keys win); Escape again
   resumes. Resuming does not blast terrain. Open the menu with the pointer where "Quit" will be
   drawn and press Enter without moving the mouse: it resumes.
3. Alt-Tab away and back: the cursor is free while away and locked again on return (no camera jump,
   no shot from the click that focuses the window). Wayland: Alt-Tab back with the pointer outside
   the window: "Click to play" shows until the pointer enters; the first click right after entering
   does not blast.
4. F1: editor opens with a free cursor; right-button look in the editor still works; F1 closes and
   the lock is back. Escape inside the editor opens the pause menu.
5. F3: overlay shows; holding Left Alt frees the cursor to use the sliders; dragging a slider
   with Alt held does not re-lock or flicker; releasing Alt re-locks.
8. Editor: hold the right button to look, press Escape: the menu shows a free cursor and the
   editor panels react to the mouse while paused; after Resume the editor look needs a new
   right-button press.
6. With the overlay hidden and the editor closed, no ImGui window appears and keys are never
   swallowed (WASD, Space, E, V, F all work right after closing the editor).
7. HiDPI / fractional scaling: menu hover matches the pointer.
