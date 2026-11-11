module;

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

module vortex.core;

namespace vortex {

    namespace {

        constexpr int kFirstGlyph = 32;
        constexpr int kGlyphCount = 95;    // 32..126

        struct GlyphArt {
            char c;
            std::array<const char*, BitmapFont::kGlyphHeight> rows;
        };

        constexpr GlyphArt kGlyphArt[] = {
#include "HudFont.inc"
        };
        static_assert(std::size(kGlyphArt) == kGlyphCount, "HudFont.inc must list every printable ASCII character");

        /// Turns the art into bitmaps. Any mistake in HudFont.inc stops the compilation here.
        consteval std::array<uint64_t, kGlyphCount> BuildGlyphs() {
            std::array<uint64_t, kGlyphCount> glyphs{};
            for (int i = 0; i < kGlyphCount; ++i) {
                const GlyphArt& art = kGlyphArt[i];
                if (art.c != static_cast<char>(kFirstGlyph + i)) throw "HudFont.inc: characters out of order";
                uint64_t bits = 0;
                for (int row = 0; row < BitmapFont::kGlyphHeight; ++row) {
                    const char* text = art.rows[static_cast<size_t>(row)];
                    if (text == nullptr) continue;
                    for (int col = 0; col < BitmapFont::kGlyphWidth; ++col) {
                        const char cell = text[col];
                        if (cell == '#') bits |= uint64_t{1} << (row * BitmapFont::kGlyphWidth + col);
                        else if (cell != '.') throw "HudFont.inc: rows are 5 cells of '.' or '#'";
                    }
                    if (text[BitmapFont::kGlyphWidth] != '\0') throw "HudFont.inc: a row is longer than 5 cells";
                }
                glyphs[static_cast<size_t>(i)] = bits;
            }
            return glyphs;
        }

        constexpr std::array<uint64_t, kGlyphCount> kGlyphs = BuildGlyphs();

        /// Pixel width of one line of `count` characters.
        float LineWidth(size_t count, int scale) {
            if (count == 0) return 0.0f;
            return static_cast<float>((static_cast<int>(count) * BitmapFont::kAdvance - 1) * scale);
        }

        HudQuad MakeQuad(float x, float y, float w, float h, Rgba color, uint64_t bits) {
            HudQuad quad;
            quad.x = x;
            quad.y = y;
            quad.w = w;
            quad.h = h;
            quad.color = color.value;
            quad.bitsLo = static_cast<uint32_t>(bits & 0xFFFFFFFFu);
            quad.bitsHi = static_cast<uint32_t>(bits >> 32);
            return quad;
        }
    }

    // ---------------------------------------------------------------------------------------------
    //  Layout
    // ---------------------------------------------------------------------------------------------

    HudRect PlaceRect(Anchor anchor, glm::vec2 size, glm::vec2 screen, glm::vec2 margin) {
        const int column = static_cast<int>(anchor) % 3;   // 0 left, 1 centre, 2 right
        const int row = static_cast<int>(anchor) / 3;      // 0 top, 1 middle, 2 bottom
        auto place = [](int slot, float extent, float available, float inset) {
            switch (slot) {
                case 0: return inset;
                case 1: return (available - extent) * 0.5f;
                default: return available - extent - inset;
            }
        };
        HudRect rect;
        rect.x = std::floor(place(column, size.x, screen.x, margin.x));
        rect.y = std::floor(place(row, size.y, screen.y, margin.y));
        rect.w = size.x;
        rect.h = size.y;
        return rect;
    }

    // ---------------------------------------------------------------------------------------------
    //  Font
    // ---------------------------------------------------------------------------------------------

    uint64_t BitmapFont::Glyph(char c) {
        const int code = static_cast<unsigned char>(c);
        if (code < kFirstGlyph || code >= kFirstGlyph + kGlyphCount) return kGlyphs['?' - kFirstGlyph];
        return kGlyphs[static_cast<size_t>(code - kFirstGlyph)];
    }

    bool BitmapFont::Pixel(uint64_t glyph, int x, int y) {
        if (x < 0 || y < 0 || x >= kGlyphWidth || y >= kGlyphHeight) return false;
        return ((glyph >> (y * kGlyphWidth + x)) & 1u) != 0;
    }

    glm::vec2 BitmapFont::Measure(std::string_view text, int scale) {
        scale = std::max(scale, 1);
        size_t lines = 1;
        size_t longest = 0;
        size_t current = 0;
        for (const char c : text) {
            if (c == '\n') {
                ++lines;
                current = 0;
                continue;
            }
            longest = std::max(longest, ++current);
        }
        const float height = static_cast<float>((static_cast<int>(lines) * kLineHeight - (kLineHeight - kGlyphHeight)) * scale);
        return {LineWidth(longest, scale), height};
    }

    // ---------------------------------------------------------------------------------------------
    //  Canvas
    // ---------------------------------------------------------------------------------------------

    void HudCanvas::Begin(glm::vec2 size) {
        m_Size = glm::max(size, glm::vec2(0.0f));
        m_Quads.clear();
        m_Overflowed = false;
        m_Dim = 0.0f;
    }

    void HudCanvas::DimBackground(float amount) {
        if (!(amount > 0.0f)) return;   // also rejects NaN
        m_Dim = std::max(m_Dim, std::min(amount, 1.0f));
    }

    int HudCanvas::TextScale() const { return std::max(1, static_cast<int>(m_Size.y) / 360); }

    int HudCanvas::ResolveScale(int scale) const { return scale > 0 ? scale : TextScale(); }

    void HudCanvas::Push(const HudQuad& quad) {
        if (!(quad.w > 0.0f) || !(quad.h > 0.0f)) return;
        if (m_Quads.size() >= kMaxQuads) {
            m_Overflowed = true;
            return;
        }
        m_Quads.push_back(quad);
    }

    void HudCanvas::Rect(const HudRect& rect, Rgba color) {
        if (color.A() == 0) return;
        Push(MakeQuad(rect.x, rect.y, rect.w, rect.h, color, BitmapFont::kSolid));
    }

    void HudCanvas::Frame(const HudRect& rect, Rgba color, float thickness) {
        const float t = std::min({thickness, rect.w * 0.5f, rect.h * 0.5f});
        if (!(t > 0.0f)) return;
        Rect({rect.x, rect.y, rect.w, t}, color);                          // top
        Rect({rect.x, rect.y + rect.h - t, rect.w, t}, color);             // bottom
        Rect({rect.x, rect.y + t, t, rect.h - 2.0f * t}, color);           // left
        Rect({rect.x + rect.w - t, rect.y + t, t, rect.h - 2.0f * t}, color);   // right
    }

    glm::vec2 HudCanvas::Measure(std::string_view text, int scale) const {
        return BitmapFont::Measure(text, ResolveScale(scale));
    }

    glm::vec2 HudCanvas::Text(glm::vec2 pos, std::string_view text, Rgba color, int scale, Align align, Rgba shadow) {
        scale = ResolveScale(scale);
        const float s = static_cast<float>(scale);
        const float glyphW = static_cast<float>(BitmapFont::kGlyphWidth) * s;
        const float glyphH = static_cast<float>(BitmapFont::kGlyphHeight) * s;
        const float advance = static_cast<float>(BitmapFont::kAdvance) * s;
        const float lineHeight = static_cast<float>(BitmapFont::kLineHeight) * s;

        auto draw = [&](glm::vec2 origin, Rgba ink) {
            float y = origin.y;
            size_t start = 0;
            while (start <= text.size()) {
                size_t end = text.find('\n', start);
                if (end == std::string_view::npos) end = text.size();
                const std::string_view line = text.substr(start, end - start);
                float x = origin.x;
                if (align == Align::Center) x -= std::floor(LineWidth(line.size(), scale) * 0.5f);
                else if (align == Align::Right) x -= LineWidth(line.size(), scale);
                for (const char c : line) {
                    if (c != ' ') Push(MakeQuad(x, y, glyphW, glyphH, ink, BitmapFont::Glyph(c)));
                    x += advance;
                }
                y += lineHeight;
                start = end + 1;
            }
        };

        if (color.A() == 0 && shadow.A() == 0) return Measure(text, scale);
        const glm::vec2 origin = glm::floor(pos);
        if (shadow.A() != 0) draw(origin + glm::vec2(s), shadow);
        if (color.A() != 0) draw(origin, color);
        return Measure(text, scale);
    }

    void HudCanvas::Crosshair(Rgba color, float arm, float thickness) {
        const float s = static_cast<float>(TextScale());
        if (!(arm > 0.0f)) arm = 5.0f * s;
        if (!(thickness > 0.0f)) thickness = s;
        const glm::vec2 c = glm::floor(m_Size * 0.5f);
        const float half = std::floor(thickness * 0.5f);
        const HudRect horizontal{c.x - arm, c.y - half, 2.0f * arm, thickness};
        const HudRect vertical{c.x - half, c.y - arm, thickness, 2.0f * arm};
        const Rgba outline = Rgba(0, 0, 0, static_cast<uint8_t>(color.A() / 2));
        for (const HudRect& r : {horizontal, vertical}) Rect({r.x - 1.0f, r.y - 1.0f, r.w + 2.0f, r.h + 2.0f}, outline);
        Rect(horizontal, color);
        Rect(vertical, color);
    }

    // ---------------------------------------------------------------------------------------------
    //  Menu
    // ---------------------------------------------------------------------------------------------

    MenuInput MenuInput::From(const Input& input, glm::vec2 mouse, bool hasMouse) {
        MenuInput m;
        m.up = input.KeyPressed(Key::Up) || input.KeyPressed(Key::W);
        m.down = input.KeyPressed(Key::Down) || input.KeyPressed(Key::S);
        m.activate = input.KeyPressed(Key::Enter) || input.KeyPressed(Key::Space);
        m.back = input.KeyPressed(Key::Escape);
        m.hasMouse = hasMouse;
        m.mouseMoved = hasMouse && (input.MouseDeltaX() != 0.0 || input.MouseDeltaY() != 0.0);
        m.mouse = mouse;
        m.click = hasMouse && input.MousePressed(MouseButton::Left);
        return m;
    }

    void Menu::AddItem(std::string label, int id) { m_Items.push_back(Item{std::move(label), id, HudRect{}}); }

    void Menu::ClearItems() {
        m_Items.clear();
        m_Focus = 0;
    }

    void Menu::SetFocus(int index) {
        const int count = static_cast<int>(m_Items.size());
        m_Focus = count == 0 ? 0 : std::clamp(index, 0, count - 1);
    }

    void Menu::Open() {
        SetFocus(0);
        m_PointerKnown = false;
    }

    HudRect Menu::ItemRect(size_t index) const { return index < m_Items.size() ? m_Items[index].rect : HudRect{}; }

    void Menu::Layout(glm::vec2 screen, int scale) {
        m_Screen = screen;
        m_Scale = std::max(scale, 1);
        const float s = static_cast<float>(m_Scale);
        const float pad = 8.0f * s;
        const float gap = 2.0f * s;
        const float itemHeight = static_cast<float>(BitmapFont::kGlyphHeight + 6) * s;
        const glm::vec2 title = m_Title.empty() ? glm::vec2(0.0f) : BitmapFont::Measure(m_Title, m_Scale * 2);

        float textWidth = title.x;
        for (const Item& item : m_Items) textWidth = std::max(textWidth, BitmapFont::Measure(item.label, m_Scale).x);
        const float itemWidth = std::max(textWidth + 2.0f * pad, 100.0f * s);

        const float itemsHeight = m_Items.empty() ? 0.0f
                                                  : static_cast<float>(m_Items.size()) * itemHeight +
                                                        static_cast<float>(m_Items.size() - 1) * gap;
        const float titleBlock = m_Title.empty() ? 0.0f : title.y + 2.0f * pad;
        const glm::vec2 panelSize(itemWidth + 2.0f * pad, titleBlock + itemsHeight + (m_Title.empty() ? 2.0f * pad : pad));
        m_Panel = PlaceRect(Anchor::Center, panelSize, screen);

        float y = m_Panel.y + (m_Title.empty() ? pad : titleBlock);
        for (Item& item : m_Items) {
            item.rect = HudRect{m_Panel.x + pad, std::floor(y), itemWidth, itemHeight};
            y += itemHeight + gap;
        }
        SetFocus(m_Focus);
    }

    MenuEvent Menu::HandleInput(const MenuInput& input) {
        MenuEvent event;
        const int count = static_cast<int>(m_Items.size());
        if (input.back) {
            event.kind = MenuEvent::Kind::Back;
            return event;
        }
        if (count == 0) return event;

        // A hidden cursor makes the position stale; a moving one makes it real again.
        m_PointerKnown = input.hasMouse && (m_PointerKnown || input.mouseMoved);
        int hovered = -1;
        if (m_PointerKnown) {
            for (int i = 0; i < count; ++i) {
                if (m_Items[static_cast<size_t>(i)].rect.Contains(input.mouse)) hovered = i;
            }
            // Only a moving pointer takes the focus: a resting one must not undo Up / Down.
            if (hovered >= 0 && input.mouseMoved) m_Focus = hovered;
        }
        if (input.click) {
            if (hovered >= 0) {
                event.kind = MenuEvent::Kind::Activated;
                event.id = m_Items[static_cast<size_t>(hovered)].id;
            }
            return event;   // a click outside the items does nothing
        }
        if (input.up) m_Focus = (m_Focus + count - 1) % count;
        if (input.down) m_Focus = (m_Focus + 1) % count;
        if (input.activate) {
            event.kind = MenuEvent::Kind::Activated;
            event.id = m_Items[static_cast<size_t>(m_Focus)].id;
        }
        return event;
    }

    void Menu::Draw(HudCanvas& canvas) const {
        const float s = static_cast<float>(m_Scale);
        const float pad = 8.0f * s;
        canvas.DimBackground(0.45f);
        canvas.Rect(m_Panel, Rgba(14, 16, 22, 225));
        canvas.Frame(m_Panel, Rgba(255, 255, 255, 48), s);
        if (!m_Title.empty()) {
            canvas.Text({m_Panel.x + m_Panel.w * 0.5f, m_Panel.y + pad}, m_Title, Rgba(255, 206, 110), m_Scale * 2, Align::Center,
                        Rgba(0, 0, 0, 160));
        }
        const float textOffset = 3.0f * s;
        for (size_t i = 0; i < m_Items.size(); ++i) {
            const Item& item = m_Items[i];
            const bool focused = static_cast<int>(i) == m_Focus;
            canvas.Rect(item.rect, focused ? Rgba(255, 176, 64, 220) : Rgba(255, 255, 255, 22));
            canvas.Text({item.rect.x + item.rect.w * 0.5f, item.rect.y + textOffset}, item.label,
                        focused ? Rgba(20, 16, 10) : Rgba(235, 235, 235), m_Scale, Align::Center);
        }
        if (!m_Footer.empty()) {
            canvas.Text({m_Panel.x + m_Panel.w * 0.5f, m_Panel.y + m_Panel.h + pad}, m_Footer, Rgba(200, 200, 200, 220), m_Scale,
                        Align::Center, Rgba(0, 0, 0, 160));
        }
    }

    // ---------------------------------------------------------------------------------------------
    //  Cursor capture
    // ---------------------------------------------------------------------------------------------

    CursorCapture::Result CursorCapture::Update(const Inputs& inputs) {
        Result result;
        if (inputs.escape && !m_Paused) {
            m_Paused = true;
            result.justPaused = true;
        }
        const bool wanted = !inputs.headless && inputs.focused && !inputs.editorOpen && !m_Paused && !inputs.freeCursor;
        const bool captured = wanted && (m_Captured || inputs.pointerInside);
        result.captured = captured;
        result.justCaptured = captured && !m_Captured;
        result.justReleased = !captured && m_Captured;
        m_Captured = captured;

        if (result.justCaptured) {
            m_ClicksBlocked = true;
            m_SinceCapture = 0.0f;
        } else if (captured && m_ClicksBlocked && inputs.dt > 0.0f) {   // also rejects NaN
            m_SinceCapture += inputs.dt;
        }
        if (captured && m_ClicksBlocked && !inputs.leftButtonDown && m_SinceCapture >= kClickGrace) m_ClicksBlocked = false;
        result.clicks = captured && !m_ClicksBlocked;
        return result;
    }
}
