# Game-UI history: text replaced or moved by the WP3 rewrite

Moved verbatim from `docs/design/game-ui.md` lines 1-9 and 245-300 at engine `430fde0` on 2026-11-18 (WP3 document
rewrite). The text below is unchanged; corrections go only under "Errata" at the end.

The original text of every section of the current document that the rewrite replaced or changed, in source order. The
current document holds the rewritten version. Lines 245-300 (the owner checks) now live in the game's
`docs/owner-checks.md`.

---

# Game UI and cursor capture (design, as built)

Owner's request: "rework interface, make lock mouse at center on play mode (at least at wayland).
Leave ImGui for debugging and map editor."

Status: implemented. Sections marked **Deviation** say where the build differs from the first
version of this design and why. Windowed behaviour (pointer lock on Wayland / X11, focus, menu
clicks, ImGui overlay with Alt) has not been confirmed on a real desktop yet: headless runs cannot
exercise it (see "Owner checks" at the end).

<!-- source lines 245-300 -->

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

### Network session (networking.md stages 5-6; windowed, two machines)

The game as a client of `vortex_server` (`src/Session.cpp`) has its own small interface on the same HUD: a bottom-left
panel `ONLINE host:port  slot N  WALK/AIR` with `ping`, `players` and the tools (Blast [LMB], Crate [E]), the key hints `Esc menu   V view`,
`Cast not run: ...` for 2 s when the server reports a cast it did not run, `Disconnected from the server` when the
connection ends, and a pause menu with Resume / "Leave the session" (the session goes on while it is open). The steps
(commands, firewall, token) are in the game's `CLAUDE.md`, "Owner's two-machine test": the server is started with
`--new-token` and the players use the token it prints (the session files' tokens are in the repository). Check on both
machines:
9. `Shredborne --connect <server>:27015 --token <token>` opens the window on the plaza; the log says `Session: joined
   ... as slot N`. The cursor locks as in check 1; mouse look, WASD, Space, Shift work.
10. The other player is a figure with a gold torso, moving smoothly where they walk (a moment late); it disappears
    when they leave (`Session: player in slot N left`).
11. LMB: a small puff at the crosshair at once, the hole (debris, flash) a moment later on both screens; the other
    player's holes appear too. Walking into a fresh hole may pull the view a little (a correction).
12. Esc: the menu shows "Leave the session"; the other player keeps moving behind it; Resume does not cast; "Leave the
    session" closes the game and the server logs the leave. Closing the window does the same.
13. Stop the server (Ctrl-C) while a client plays: the HUD says "Disconnected from the server" at once (a server that
    is killed or unplugged: after up to 30 s, ENet's timeout). A wrong token is refused before the window opens
    (`refused this client: wrong token`); a server that is not there gives `no answer from ... within 15 s`.
14. (Stage 6.) The plaza's crates are there on both screens, in the same places. E drops a crate 3 m ahead a moment
    after the key, on both screens; walking into a resting crate stops the player without a jerk. A blast that hits a
    crate breaks it into pieces (lumps on each screen, not necessarily the same) and both players see the crate gone.
    Cutting the watch tower's base (several casts, as `--tower-blast` does offline) drops it as one body that falls,
    tips and comes to rest in the same place on both screens; standing where it lands, it passes through the player
    and may enclose them (walk out). The log's `Session: objects:` line at exit counts copies, rests and fragments.
    (Review of stage 6.) Aiming at a crate in the open (10 m away, the ground well behind it) breaks the crate, not the
    ground behind; the puff appears on the crate. Standing on a resting crate (jump onto it) while the other player
    blasts the ground a metre or two beside it: the player stays on the crate (rides it if it slides), no drop into it.
