module;

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

export module vortex.core:hud;

import :input;

export namespace vortex {

    // =============================================================================================
    //  Game HUD: a tiny screen-space UI without ImGui (docs/design/game-ui.md).
    //  Everything here is plain data and state machines: no GPU, no window. The renderer draws
    //  the quads of a HudCanvas (graphics: hud.vert / hud.frag); the game builds them each frame.
    //  Coordinates are framebuffer pixels, (0,0) at the top-left, y down.
    // =============================================================================================

    /// @brief 8-bit RGBA packed into one word, red in the lowest byte (GLSL unpackUnorm4x8).
    /// Colours are display (sRGB) values: the HUD is drawn after tone mapping.
    struct Rgba {
        uint32_t value = 0xFFFFFFFFu;

        constexpr Rgba() = default;
        constexpr Rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a = 255)
            : value(static_cast<uint32_t>(r) | (static_cast<uint32_t>(g) << 8) | (static_cast<uint32_t>(b) << 16) |
                    (static_cast<uint32_t>(a) << 24)) {}

        constexpr uint8_t R() const { return static_cast<uint8_t>(value & 0xFFu); }
        constexpr uint8_t G() const { return static_cast<uint8_t>((value >> 8) & 0xFFu); }
        constexpr uint8_t B() const { return static_cast<uint8_t>((value >> 16) & 0xFFu); }
        constexpr uint8_t A() const { return static_cast<uint8_t>(value >> 24); }
        constexpr Rgba WithAlpha(uint8_t a) const { return Rgba(R(), G(), B(), a); }

        static constexpr Rgba White() { return Rgba(255, 255, 255); }
        static constexpr Rgba Black() { return Rgba(0, 0, 0); }
        static constexpr Rgba Transparent() { return Rgba(0, 0, 0, 0); }

        friend constexpr bool operator==(const Rgba&, const Rgba&) = default;
    };

    /// @brief Axis-aligned rectangle in pixels: top-left corner and size.
    struct HudRect {
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;

        /// @brief Half-open: the left / top edges are inside, the right / bottom edges are not.
        constexpr bool Contains(glm::vec2 p) const { return p.x >= x && p.y >= y && p.x < x + w && p.y < y + h; }
        constexpr glm::vec2 Position() const { return {x, y}; }
        constexpr glm::vec2 Size() const { return {w, h}; }
        friend constexpr bool operator==(const HudRect&, const HudRect&) = default;
    };

    enum class Anchor : uint8_t { TopLeft, Top, TopRight, Left, Center, Right, BottomLeft, Bottom, BottomRight };
    enum class Align : uint8_t { Left, Center, Right };

    /**
     * @brief Places a box of `size` at `anchor` of a `screen`-sized area, moved inwards by
     * `margin` pixels (ignored along an axis on which the anchor is centred). The corner is
     * floored to whole pixels, so text drawn there stays crisp.
     */
    HudRect PlaceRect(Anchor anchor, glm::vec2 size, glm::vec2 screen, glm::vec2 margin = glm::vec2(0.0f));

    /**
     * @brief One HUD primitive as the GPU reads it (`VxHudQuad` in shaders/include/common.glsl,
     * 32 bytes, std430; a contract test compares the layouts).
     * @details The quad is covered by a 5 x 9 grid of cells; a cell is drawn if its bit is set
     * (bit = row * 5 + column, row 0 at the top). A solid rectangle has every bit set, a glyph
     * has the bits of its bitmap. One pipeline, one draw call, no texture.
     */
    struct HudQuad {
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;   ///< rect, pixels
        uint32_t color = 0;                              ///< Rgba::value
        uint32_t bitsLo = 0;                             ///< cells 0..31
        uint32_t bitsHi = 0;                             ///< cells 32..44
        uint32_t reserved = 0;
    };
    static_assert(sizeof(HudQuad) == 32, "HudQuad mirrors VxHudQuad (vec4 + uvec4)");

    /**
     * @brief The built-in 5 x 9 bitmap font: printable ASCII (32..126), monospace.
     * @details Capitals and digits use rows 0..6, descenders rows 7..8. One glyph is 5 x 9 cells
     * of `scale` pixels; characters advance by kAdvance cells, lines by kLineHeight cells.
     * The glyphs were drawn for this engine and are released with it (src/core/internal/HudFont.inc).
     */
    struct BitmapFont {
        static constexpr int kGlyphWidth = 5;
        static constexpr int kGlyphHeight = 9;
        static constexpr int kAdvance = 6;              ///< glyph + 1 cell of spacing
        static constexpr int kLineHeight = 11;          ///< glyph + 2 cells of spacing
        static constexpr uint64_t kSolid = (uint64_t{1} << (kGlyphWidth * kGlyphHeight)) - 1;

        /// @brief Bitmap of a character; characters outside 32..126 give the bitmap of '?'.
        static uint64_t Glyph(char c);
        /// @brief Whether the cell (x, y) of a bitmap is set; false outside the 5 x 9 grid.
        static bool Pixel(uint64_t glyph, int x, int y);
        /// @brief Size in pixels of `text` at `scale` (>= 1): the longest line by the number of
        /// lines ('\n' starts a new line). The last column of spacing is not included.
        static glm::vec2 Measure(std::string_view text, int scale);
    };

    /**
     * @brief The list of HUD quads of one frame, plus the drawing helpers the game uses.
     * @details The engine calls Begin() with the output size at the start of every frame; the
     * game adds rectangles and text; the renderer draws Quads() in order (later on top) after
     * tone mapping and before ImGui. At most kMaxQuads quads are kept per frame; anything
     * beyond is dropped and Overflowed() reports it.
     */
    class HudCanvas {
    public:
        static constexpr size_t kMaxQuads = 8192;

        /// @brief Clears the canvas and sets the drawing area (output size in pixels).
        void Begin(glm::vec2 size);
        glm::vec2 Size() const { return m_Size; }
        /// @brief The integer text scale for this output: max(1, height / 360), so 2 at 720p,
        /// 3 at 1080p. Pass 0 as `scale` to the functions below to use it.
        int TextScale() const;

        void Rect(const HudRect& rect, Rgba color);
        /// @brief Outline of `rect`, `thickness` pixels wide, drawn inside it.
        void Frame(const HudRect& rect, Rgba color, float thickness = 1.0f);
        /**
         * @brief Draws text. `pos.x` is the left edge, the centre or the right edge of each line
         * depending on `align`; `pos.y` is the top. A non-transparent `shadow` is drawn one
         * scale step down and right first. Returns the size of the text block (Measure).
         */
        glm::vec2 Text(glm::vec2 pos, std::string_view text, Rgba color, int scale = 0, Align align = Align::Left,
                       Rgba shadow = Rgba::Transparent());
        glm::vec2 Measure(std::string_view text, int scale = 0) const;
        /// @brief A plus sign at the centre of the canvas: arm length and thickness in pixels
        /// (0 = from the text scale), with a one pixel dark outline for contrast.
        void Crosshair(Rgba color, float arm = 0.0f, float thickness = 0.0f);

        /// @brief Darkens everything under the HUD by `amount` (0..1) this frame; the largest
        /// request wins. Done by the present pass at no cost, unlike a full-screen quad.
        void DimBackground(float amount);
        float BackgroundDim() const { return m_Dim; }

        std::span<const HudQuad> Quads() const { return m_Quads; }
        bool Overflowed() const { return m_Overflowed; }

    private:
        void Push(const HudQuad& quad);
        int ResolveScale(int scale) const;

        glm::vec2 m_Size{0.0f};
        float m_Dim = 0.0f;
        std::vector<HudQuad> m_Quads;
        bool m_Overflowed = false;
    };

    // ---------------------------------------------------------------------------------------------
    //  Menu
    // ---------------------------------------------------------------------------------------------

    /// @brief What a menu reads in one frame. Positions are framebuffer pixels.
    struct MenuInput {
        bool up = false;          ///< Up arrow or W pressed
        bool down = false;        ///< Down arrow or S pressed
        bool activate = false;    ///< Enter or Space pressed
        bool back = false;        ///< Escape pressed
        bool hasMouse = false;    ///< The cursor is free and over the window
        bool mouseMoved = false;  ///< The cursor moved this frame (Input::MouseDelta* non-zero)
        glm::vec2 mouse{0.0f};
        bool click = false;       ///< Left button pressed this frame

        /// @brief Reads the keys and the left button from `input` (UI-captured input reads as
        /// not pressed). `mouse` is the cursor in framebuffer pixels; pass hasMouse = false while
        /// the cursor is captured, so a hidden cursor never hovers an item. mouseMoved comes from
        /// the cursor delta, which is zero for the first position after a capture change.
        static MenuInput From(const Input& input, glm::vec2 mouse, bool hasMouse);
    };

    struct MenuEvent {
        enum class Kind : uint8_t { None, Activated, Back };
        Kind kind = Kind::None;
        int id = -1;              ///< Id of the activated item (Activated only)
    };

    /**
     * @brief A vertical list of items centred on the screen: keyboard (Up / Down wrap, Enter /
     * Space activate, Escape = Back) and mouse (moving onto an item focuses it, a click on an
     * item activates it, a click elsewhere does nothing). HandleInput and Draw are separate so
     * that a game can react to the input before it draws the rest of its HUD and draw the menu
     * on top.
     * @details The pointer position counts only after the pointer has moved since Open() (or
     * since the last frame without a mouse): right after the cursor is released its position is
     * stale (on Wayland the virtual position of the lock). And only a moving pointer takes the
     * focus, so one resting on an item does not undo Up / Down every frame.
     */
    class Menu {
    public:
        void SetTitle(std::string title) { m_Title = std::move(title); }
        void SetFooter(std::string footer) { m_Footer = std::move(footer); }
        void AddItem(std::string label, int id);
        void ClearItems();
        size_t ItemCount() const { return m_Items.size(); }

        int Focus() const { return m_Focus; }
        void SetFocus(int index);
        /// @brief Call when the menu is shown: focus on the first item, and the pointer counts
        /// only once it moves.
        void Open();

        /// @brief Computes the panel and item rectangles for a screen size and text scale.
        void Layout(glm::vec2 screen, int scale);
        const HudRect& PanelRect() const { return m_Panel; }
        /// @brief Rectangle of item `index` after Layout (empty rectangle if out of range).
        HudRect ItemRect(size_t index) const;

        /// @brief Applies one frame of input (call Layout first). Returns what happened.
        MenuEvent HandleInput(const MenuInput& input);
        /// @brief Dims the screen (HudCanvas::DimBackground) and draws the panel, the title, the
        /// items and the footer.
        void Draw(HudCanvas& canvas) const;

    private:
        struct Item {
            std::string label;
            int id = 0;
            HudRect rect;
        };
        std::string m_Title;
        std::string m_Footer;
        std::vector<Item> m_Items;
        HudRect m_Panel;
        glm::vec2 m_Screen{0.0f};
        int m_Scale = 1;
        int m_Focus = 0;
        bool m_PointerKnown = false;   ///< The pointer moved since Open() / since it was last hidden
    };

    // ---------------------------------------------------------------------------------------------
    //  Cursor capture
    // ---------------------------------------------------------------------------------------------

    /**
     * @brief Play-mode cursor state machine: whether the mouse should be locked (hidden,
     * relative motion) this frame, whether game clicks count, and the pause state Escape
     * toggles. No GLFW: the game feeds the facts and applies the edges to the window
     * (docs/design/game-ui.md).
     *
     * wanted   = !headless && focused && !editorOpen && !paused && !freeCursor
     * captured = wanted && (captured last frame || pointerInside)
     * clicks   = captured && the left button was up at least kClickGrace seconds after the capture
     *
     * The capture starts only with the pointer over the window (Wayland locks the pointer only
     * then), and is kept while it is wanted. The click that brought the game back (focus, pointer
     * entry, Alt released over the overlay) is never a shot, even if its button event arrives a
     * frame after the focus event: clicks wait until the button has been up and kClickGrace has
     * passed since the capture. Escape pauses (the editor may be open: the pause menu is the way
     * out of both). Resume() is called by the game when the pause menu says so (Escape again or
     * the Resume item), so the game must not pass an Escape the menu already consumed. Losing
     * the focus never changes the pause state; headless runs are never captured.
     */
    class CursorCapture {
    public:
        /// Seconds after a capture during which a left-button press is not a game click.
        static constexpr float kClickGrace = 0.15f;

        struct Inputs {
            bool headless = false;
            bool focused = false;       ///< The window has the keyboard focus
            bool pointerInside = true;  ///< The pointer is over the window (GLFW_HOVERED); true where unknown
            bool editorOpen = false;    ///< The editor owns mouse and cursor
            bool freeCursor = false;    ///< e.g. a held key to click the debug overlay
            bool escape = false;        ///< Escape pressed this frame and not consumed by a menu
            bool leftButtonDown = false;///< Physical state of the left button (Input::MouseDownRaw)
            float dt = 0.0f;            ///< Seconds since the previous Update
        };
        struct Result {
            bool captured = false;
            bool justCaptured = false;  ///< captured now, not last frame
            bool justReleased = false;  ///< captured last frame, not now
            bool justPaused = false;    ///< Escape paused the game this frame
            bool clicks = false;        ///< game mouse buttons may act this frame (see the class)
        };

        Result Update(const Inputs& inputs);

        bool Captured() const { return m_Captured; }
        bool Paused() const { return m_Paused; }
        void Pause() { m_Paused = true; }
        void Resume() { m_Paused = false; }

    private:
        bool m_Captured = false;
        bool m_Paused = false;
        bool m_ClicksBlocked = true;
        float m_SinceCapture = 0.0f;
    };
}
