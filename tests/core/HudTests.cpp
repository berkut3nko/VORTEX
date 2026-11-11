// Game HUD logic without a GPU (docs/design/game-ui.md): bitmap font, layout, canvas, menu input
// and the play-mode cursor capture state machine.

#include <gtest/gtest.h>

#include <glm/glm.hpp>

#include <cstdint>
#include <set>
#include <string>

import vortex.core;

using namespace vortex;

namespace {

    /// Number of set cells of a glyph bitmap.
    int CountCells(uint64_t glyph) {
        int count = 0;
        for (int y = 0; y < BitmapFont::kGlyphHeight; ++y) {
            for (int x = 0; x < BitmapFont::kGlyphWidth; ++x) count += BitmapFont::Pixel(glyph, x, y) ? 1 : 0;
        }
        return count;
    }

    uint64_t QuadBits(const HudQuad& q) { return uint64_t{q.bitsLo} | (uint64_t{q.bitsHi} << 32); }

    /// A pause menu as the game builds it.
    Menu MakeMenu(glm::vec2 screen = {1280.0f, 720.0f}) {
        Menu menu;
        menu.SetTitle("PAUSED");
        menu.AddItem("Resume", 1);
        menu.AddItem("Settings", 2);
        menu.AddItem("Quit", 3);
        menu.Layout(screen, 2);
        return menu;
    }

    glm::vec2 Centre(const HudRect& r) { return {r.x + r.w * 0.5f, r.y + r.h * 0.5f}; }

    CursorCapture::Inputs Playing() {
        CursorCapture::Inputs in;
        in.focused = true;
        return in;
    }
}

// -------------------------------------------------------------------------------------------------
//  Font
// -------------------------------------------------------------------------------------------------

TEST(BitmapFont, EveryPrintableGlyphIsDistinctAndFitsTheGrid) {
    std::set<uint64_t> seen;
    for (int c = 33; c <= 126; ++c) {
        const uint64_t glyph = BitmapFont::Glyph(static_cast<char>(c));
        EXPECT_NE(glyph, 0u) << "'" << static_cast<char>(c) << "' is empty";
        EXPECT_EQ(glyph & ~BitmapFont::kSolid, 0u) << "'" << static_cast<char>(c) << "' has bits outside the 5 x 9 grid";
        EXPECT_TRUE(seen.insert(glyph).second) << "'" << static_cast<char>(c) << "' looks like another glyph";
    }
    EXPECT_EQ(BitmapFont::Glyph(' '), 0u);
}

TEST(BitmapFont, GlyphShapes) {
    // 'I' = top bar, stem in the middle column, bottom bar; nothing in the descender rows.
    const uint64_t i = BitmapFont::Glyph('I');
    EXPECT_TRUE(BitmapFont::Pixel(i, 1, 0) && BitmapFont::Pixel(i, 2, 0) && BitmapFont::Pixel(i, 3, 0));
    EXPECT_FALSE(BitmapFont::Pixel(i, 0, 0));
    for (int y = 1; y < 6; ++y) EXPECT_TRUE(BitmapFont::Pixel(i, 2, y)) << y;
    EXPECT_FALSE(BitmapFont::Pixel(i, 2, 7));
    EXPECT_EQ(CountCells(i), 3 + 5 + 3);
    // Descenders use rows 7..8; capitals never do.
    EXPECT_TRUE(BitmapFont::Pixel(BitmapFont::Glyph('g'), 1, 8));
    for (char c = 'A'; c <= 'Z'; ++c) {
        for (int x = 0; x < BitmapFont::kGlyphWidth; ++x) {
            EXPECT_FALSE(BitmapFont::Pixel(BitmapFont::Glyph(c), x, 7)) << c;
            EXPECT_FALSE(BitmapFont::Pixel(BitmapFont::Glyph(c), x, 8)) << c;
        }
    }
    // Outside the grid nothing is set.
    EXPECT_FALSE(BitmapFont::Pixel(BitmapFont::kSolid, -1, 0));
    EXPECT_FALSE(BitmapFont::Pixel(BitmapFont::kSolid, 5, 0));
    EXPECT_FALSE(BitmapFont::Pixel(BitmapFont::kSolid, 0, 9));
    EXPECT_EQ(CountCells(BitmapFont::kSolid), 45);
}

TEST(BitmapFont, NonAsciiFallsBackToQuestionMark) {
    const uint64_t question = BitmapFont::Glyph('?');
    EXPECT_EQ(BitmapFont::Glyph('\t'), question);
    EXPECT_EQ(BitmapFont::Glyph(static_cast<char>(127)), question);
    EXPECT_EQ(BitmapFont::Glyph(static_cast<char>(0xC3)), question);   // first byte of UTF-8 'A-umlaut'
    EXPECT_EQ(BitmapFont::Glyph('\0'), question);
}

TEST(BitmapFont, MeasureCountsCellsLinesAndScale) {
    EXPECT_EQ(BitmapFont::Measure("", 1), glm::vec2(0.0f, 9.0f));
    EXPECT_EQ(BitmapFont::Measure("A", 1), glm::vec2(5.0f, 9.0f));
    EXPECT_EQ(BitmapFont::Measure("AB", 1), glm::vec2(11.0f, 9.0f));            // 6 + 5
    EXPECT_EQ(BitmapFont::Measure("Resume", 2), glm::vec2((6 * 6 - 1) * 2.0f, 18.0f));
    // The longest line and the number of lines; the last line has no spacing below it.
    EXPECT_EQ(BitmapFont::Measure("ab\nabcd\n", 1), glm::vec2(23.0f, 11.0f * 3 - 2));
    EXPECT_EQ(BitmapFont::Measure("x", 0), BitmapFont::Measure("x", 1));    // scale is at least 1
}

// -------------------------------------------------------------------------------------------------
//  Layout
// -------------------------------------------------------------------------------------------------

TEST(HudLayout, AnchorsPlaceExactPixelRects) {
    const glm::vec2 screen(1280.0f, 720.0f);
    const glm::vec2 size(100.0f, 20.0f);
    const glm::vec2 margin(8.0f, 4.0f);
    EXPECT_EQ(PlaceRect(Anchor::TopLeft, size, screen, margin), (HudRect{8, 4, 100, 20}));
    EXPECT_EQ(PlaceRect(Anchor::Top, size, screen, margin), (HudRect{590, 4, 100, 20}));
    EXPECT_EQ(PlaceRect(Anchor::TopRight, size, screen, margin), (HudRect{1172, 4, 100, 20}));
    EXPECT_EQ(PlaceRect(Anchor::Left, size, screen, margin), (HudRect{8, 350, 100, 20}));
    EXPECT_EQ(PlaceRect(Anchor::Center, size, screen, margin), (HudRect{590, 350, 100, 20}));
    EXPECT_EQ(PlaceRect(Anchor::Right, size, screen, margin), (HudRect{1172, 350, 100, 20}));
    EXPECT_EQ(PlaceRect(Anchor::BottomLeft, size, screen, margin), (HudRect{8, 696, 100, 20}));
    EXPECT_EQ(PlaceRect(Anchor::Bottom, size, screen, margin), (HudRect{590, 696, 100, 20}));
    EXPECT_EQ(PlaceRect(Anchor::BottomRight, size, screen, margin), (HudRect{1172, 696, 100, 20}));
}

TEST(HudLayout, OddSizesAreFlooredToWholePixels) {
    // (101 - 10) / 2 = 45.5 -> 45; (51 - 11) / 2 = 20.
    EXPECT_EQ(PlaceRect(Anchor::Center, {10.0f, 11.0f}, {101.0f, 51.0f}), (HudRect{45, 20, 10, 11}));
    EXPECT_EQ(PlaceRect(Anchor::BottomRight, {10.5f, 3.0f}, {101.0f, 51.0f}), (HudRect{90, 48, 10.5f, 3}));
    const HudRect r{10, 20, 5, 5};
    EXPECT_TRUE(r.Contains({10.0f, 20.0f}));
    EXPECT_TRUE(r.Contains({14.9f, 24.9f}));
    EXPECT_FALSE(r.Contains({15.0f, 22.0f}));
    EXPECT_FALSE(r.Contains({12.0f, 19.9f}));
}

// -------------------------------------------------------------------------------------------------
//  Canvas
// -------------------------------------------------------------------------------------------------

TEST(HudCanvas, RectIsOneSolidQuad) {
    HudCanvas canvas;
    canvas.Begin({640.0f, 360.0f});
    canvas.Rect({1, 2, 3, 4}, Rgba(10, 20, 30, 40));
    ASSERT_EQ(canvas.Quads().size(), 1u);
    const HudQuad& q = canvas.Quads()[0];
    EXPECT_EQ(q.x, 1.0f);
    EXPECT_EQ(q.y, 2.0f);
    EXPECT_EQ(q.w, 3.0f);
    EXPECT_EQ(q.h, 4.0f);
    EXPECT_EQ(q.color, 10u | (20u << 8) | (30u << 16) | (40u << 24));
    EXPECT_EQ(QuadBits(q), BitmapFont::kSolid);
    // Invisible or empty rectangles cost nothing.
    canvas.Rect({0, 0, 10, 10}, Rgba::Transparent());
    canvas.Rect({0, 0, 0, 10}, Rgba::White());
    canvas.Rect({0, 0, 10, -1}, Rgba::White());
    EXPECT_EQ(canvas.Quads().size(), 1u);
    // Begin clears.
    canvas.Begin({640.0f, 360.0f});
    EXPECT_TRUE(canvas.Quads().empty());
}

TEST(HudCanvas, TextIsOneQuadPerVisibleCharacter) {
    HudCanvas canvas;
    canvas.Begin({1280.0f, 720.0f});
    EXPECT_EQ(canvas.TextScale(), 2);
    const glm::vec2 size = canvas.Text({10.4f, 20.7f}, "Hi there\nok", Rgba::White());
    EXPECT_EQ(size, BitmapFont::Measure("Hi there\nok", 2));
    ASSERT_EQ(canvas.Quads().size(), 9u);   // spaces and newlines draw nothing
    const HudQuad& h = canvas.Quads()[0];
    EXPECT_EQ(h.x, 10.0f);                   // floored to a pixel
    EXPECT_EQ(h.y, 20.0f);
    EXPECT_EQ(h.w, 10.0f);
    EXPECT_EQ(h.h, 18.0f);
    EXPECT_EQ(QuadBits(h), BitmapFont::Glyph('H'));
    EXPECT_EQ(canvas.Quads()[1].x, 22.0f);   // advance 6 cells x 2
    EXPECT_EQ(canvas.Quads()[2].x, 10.0f + 12.0f * 3);   // 't' after the space
    const HudQuad& o = canvas.Quads()[7];
    EXPECT_EQ(o.x, 10.0f);                   // second line starts at the left again ...
    EXPECT_EQ(o.y, 20.0f + 22.0f);           // ... one line height (11 cells) lower
    EXPECT_EQ(QuadBits(o), BitmapFont::Glyph('o'));
}

TEST(HudCanvas, TextAlignmentAndShadow) {
    HudCanvas canvas;
    canvas.Begin({640.0f, 360.0f});
    EXPECT_EQ(canvas.TextScale(), 1);
    canvas.Text({100.0f, 0.0f}, "abc", Rgba::White(), 1, Align::Right);
    EXPECT_EQ(canvas.Quads()[0].x, 100.0f - 17.0f);        // width of "abc" = 17
    canvas.Begin({640.0f, 360.0f});
    canvas.Text({100.0f, 0.0f}, "abc", Rgba::White(), 1, Align::Center);
    EXPECT_EQ(canvas.Quads()[0].x, 100.0f - 8.0f);         // floor(17 / 2)
    canvas.Begin({640.0f, 360.0f});
    canvas.Text({50.0f, 60.0f}, "a", Rgba::White(), 3, Align::Left, Rgba(0, 0, 0, 128));
    ASSERT_EQ(canvas.Quads().size(), 2u);                 // shadow first, then the text on top
    EXPECT_EQ(canvas.Quads()[0].x, 53.0f);
    EXPECT_EQ(canvas.Quads()[0].y, 63.0f);
    EXPECT_EQ(canvas.Quads()[0].color, Rgba(0, 0, 0, 128).value);
    EXPECT_EQ(canvas.Quads()[1].x, 50.0f);
}

TEST(HudCanvas, CrosshairIsCentred) {
    HudCanvas canvas;
    canvas.Begin({1280.0f, 720.0f});
    canvas.Crosshair(Rgba::White());
    ASSERT_EQ(canvas.Quads().size(), 4u);   // two outlines, two bars
    const HudQuad& h = canvas.Quads()[2];
    const HudQuad& v = canvas.Quads()[3];
    EXPECT_EQ(h.x + h.w * 0.5f, 640.0f);
    EXPECT_EQ(v.y + v.h * 0.5f, 360.0f);
    EXPECT_EQ(h.w, 20.0f);                  // arm 5 cells x scale 2, both sides
    EXPECT_EQ(h.h, 2.0f);
    EXPECT_EQ(v.x, 639.0f);
}

TEST(HudCanvas, OverflowDropsAndReports) {
    HudCanvas canvas;
    canvas.Begin({100.0f, 100.0f});
    for (size_t i = 0; i < HudCanvas::kMaxQuads; ++i) canvas.Rect({0, 0, 1, 1}, Rgba::White());
    EXPECT_FALSE(canvas.Overflowed());
    canvas.Text({0, 0}, "more", Rgba::White());
    EXPECT_TRUE(canvas.Overflowed());
    EXPECT_EQ(canvas.Quads().size(), HudCanvas::kMaxQuads);
    canvas.Begin({100.0f, 100.0f});
    EXPECT_FALSE(canvas.Overflowed());
}

// -------------------------------------------------------------------------------------------------
//  Menu
// -------------------------------------------------------------------------------------------------

TEST(HudMenu, LayoutIsCentredAndItemsStack) {
    const Menu menu = MakeMenu();
    const HudRect panel = menu.PanelRect();
    EXPECT_NEAR(panel.x + panel.w * 0.5f, 640.0f, 1.0f);
    EXPECT_NEAR(panel.y + panel.h * 0.5f, 360.0f, 1.0f);
    for (size_t i = 0; i < menu.ItemCount(); ++i) {
        const HudRect item = menu.ItemRect(i);
        EXPECT_GE(item.x, panel.x);
        EXPECT_LE(item.x + item.w, panel.x + panel.w);
        EXPECT_GE(item.y, panel.y);
        EXPECT_LE(item.y + item.h, panel.y + panel.h);
        if (i > 0) EXPECT_GT(item.y, menu.ItemRect(i - 1).y + menu.ItemRect(i - 1).h - 0.5f) << "items overlap";
    }
    EXPECT_EQ(menu.ItemRect(99), HudRect{});
}

TEST(HudMenu, KeyboardWrapsAndActivatesTheFocusedItem) {
    Menu menu = MakeMenu();
    EXPECT_EQ(menu.Focus(), 0);
    MenuInput up;
    up.up = true;
    EXPECT_EQ(menu.HandleInput(up).kind, MenuEvent::Kind::None);
    EXPECT_EQ(menu.Focus(), 2);              // wrapped to the last item
    MenuInput down;
    down.down = true;
    menu.HandleInput(down);
    EXPECT_EQ(menu.Focus(), 0);              // and back
    menu.HandleInput(down);
    MenuInput enter;
    enter.activate = true;
    const MenuEvent event = menu.HandleInput(enter);
    EXPECT_EQ(event.kind, MenuEvent::Kind::Activated);
    EXPECT_EQ(event.id, 2);
}

TEST(HudMenu, EscapeIsBack) {
    Menu menu = MakeMenu();
    MenuInput escape;
    escape.back = true;
    escape.activate = true;                  // Back wins over anything else in the same frame
    EXPECT_EQ(menu.HandleInput(escape).kind, MenuEvent::Kind::Back);
}

TEST(HudMenu, MouseHoverFocusesAndClickActivates) {
    Menu menu = MakeMenu();
    MenuInput hover;
    hover.hasMouse = true;
    hover.mouseMoved = true;
    hover.mouse = Centre(menu.ItemRect(2));
    EXPECT_EQ(menu.HandleInput(hover).kind, MenuEvent::Kind::None);
    EXPECT_EQ(menu.Focus(), 2);

    MenuInput click = hover;
    click.mouse = Centre(menu.ItemRect(0));
    click.click = true;
    const MenuEvent event = menu.HandleInput(click);
    EXPECT_EQ(event.kind, MenuEvent::Kind::Activated);
    EXPECT_EQ(event.id, 1);
    EXPECT_EQ(menu.Focus(), 0);
}

TEST(HudMenu, ClickOutsideTheItemsDoesNothing) {
    Menu menu = MakeMenu();
    menu.SetFocus(1);
    MenuInput click;
    click.hasMouse = true;
    click.mouseMoved = true;
    click.click = true;
    click.mouse = {5.0f, 5.0f};
    click.activate = true;                   // a click consumes the frame: no keyboard activation
    EXPECT_EQ(menu.HandleInput(click).kind, MenuEvent::Kind::None);
    EXPECT_EQ(menu.Focus(), 1);
    // A captured (hidden) cursor never hovers: the position is ignored.
    MenuInput hidden;
    hidden.mouse = Centre(menu.ItemRect(2));
    menu.HandleInput(hidden);
    EXPECT_EQ(menu.Focus(), 1);
}

TEST(HudMenu, ARestingPointerDoesNotOverrideTheKeyboard) {
    // The pointer is moved onto "Settings" and left there; the keyboard must still move the
    // focus, and Enter must activate what the keyboard chose.
    Menu menu = MakeMenu();
    MenuInput move;
    move.hasMouse = true;
    move.mouseMoved = true;
    move.mouse = Centre(menu.ItemRect(1));
    menu.HandleInput(move);
    EXPECT_EQ(menu.Focus(), 1);

    MenuInput resting = move;
    resting.mouseMoved = false;
    MenuInput up = resting;
    up.up = true;
    menu.HandleInput(up);
    EXPECT_EQ(menu.Focus(), 0);
    menu.HandleInput(resting);              // the next frame: the resting pointer does not take it back
    EXPECT_EQ(menu.Focus(), 0);
    MenuInput enter = resting;
    enter.activate = true;
    const MenuEvent event = menu.HandleInput(enter);
    EXPECT_EQ(event.kind, MenuEvent::Kind::Activated);
    EXPECT_EQ(event.id, 1);                 // "Resume", not the hovered "Settings"

    // A click still activates the item under the pointer, moved or not.
    MenuInput click = resting;
    click.click = true;
    EXPECT_EQ(menu.HandleInput(click).id, 2);
}

TEST(HudMenu, AStalePointerAfterOpeningNeitherHoversNorClicks) {
    // When the menu opens the cursor has just been released: until it moves, its position is
    // stale (on Wayland the virtual position of the lock) and may lie on "Quit".
    Menu menu = MakeMenu();
    menu.SetFocus(2);
    menu.Open();
    EXPECT_EQ(menu.Focus(), 0);
    MenuInput stale;
    stale.hasMouse = true;
    stale.mouse = Centre(menu.ItemRect(2));
    menu.HandleInput(stale);
    EXPECT_EQ(menu.Focus(), 0);
    MenuInput click = stale;
    click.click = true;
    EXPECT_EQ(menu.HandleInput(click).kind, MenuEvent::Kind::None);
    MenuInput enter = stale;
    enter.activate = true;
    EXPECT_EQ(menu.HandleInput(enter).id, 1);

    // Once it moves, the position is real.
    MenuInput moved = stale;
    moved.mouseMoved = true;
    menu.HandleInput(moved);
    EXPECT_EQ(menu.Focus(), 2);
    // Captured again (no mouse): the position goes stale again.
    menu.HandleInput(MenuInput{});
    MenuInput clickAgain = stale;
    clickAgain.click = true;
    EXPECT_EQ(menu.HandleInput(clickAgain).kind, MenuEvent::Kind::None);
}

TEST(Input, RawQueriesIgnoreUiCapture) {
    // Holding a modifier to free the cursor must keep reading as held while ImGui has the
    // keyboard (a slider drag), or the cursor would lock again in the middle of the drag.
    Input input;
    input.NewFrame();
    input.OnKey(static_cast<int>(Key::LeftAlt), true);
    input.OnMouseButton(static_cast<int>(MouseButton::Left), true);
    input.SetUiCapture(true, true);
    EXPECT_FALSE(input.KeyDown(Key::LeftAlt));
    EXPECT_FALSE(input.MouseDown(MouseButton::Left));
    EXPECT_TRUE(input.KeyDownRaw(Key::LeftAlt));
    EXPECT_TRUE(input.MouseDownRaw(MouseButton::Left));
    input.Clear();
    EXPECT_FALSE(input.KeyDownRaw(Key::LeftAlt));
    EXPECT_FALSE(input.MouseDownRaw(MouseButton::Left));
}

TEST(HudMenu, InputFromTheInputObject) {
    Input input;
    input.NewFrame();
    input.OnKey(static_cast<int>(Key::Down), true);
    input.OnMouseButton(static_cast<int>(MouseButton::Left), true);
    MenuInput m = MenuInput::From(input, {3.0f, 4.0f}, true);
    EXPECT_TRUE(m.down);
    EXPECT_FALSE(m.up);
    EXPECT_TRUE(m.click);
    EXPECT_FALSE(m.mouseMoved);              // no cursor event yet
    EXPECT_EQ(m.mouse, glm::vec2(3.0f, 4.0f));
    // Without a free cursor there is no click.
    EXPECT_FALSE(MenuInput::From(input, {}, false).click);
    // Held keys are not pressed again on the next frame.
    input.NewFrame();
    m = MenuInput::From(input, {}, true);
    EXPECT_FALSE(m.down);
    EXPECT_FALSE(m.click);
    // Motion: the first position after a reset is not a move, the next one is.
    input.NewFrame();
    input.OnCursor(10.0, 10.0);
    EXPECT_FALSE(MenuInput::From(input, {}, true).mouseMoved);
    input.NewFrame();
    input.OnCursor(11.0, 10.0);
    EXPECT_TRUE(MenuInput::From(input, {}, true).mouseMoved);
    EXPECT_FALSE(MenuInput::From(input, {}, false).mouseMoved);
    // UI-captured input reads as not pressed.
    input.NewFrame();
    input.OnKey(static_cast<int>(Key::Escape), true);
    input.SetUiCapture(true, true);
    EXPECT_FALSE(MenuInput::From(input, {}, true).back);
}

TEST(HudMenu, DrawDimsTheScreenAndDrawsTheItems) {
    const Menu menu = MakeMenu();
    HudCanvas canvas;
    canvas.Begin({1280.0f, 720.0f});
    menu.Draw(canvas);
    ASSERT_FALSE(canvas.Quads().empty());
    // The backdrop is darkened by the present pass, not by a full-screen blended quad.
    EXPECT_GT(canvas.BackgroundDim(), 0.0f);
    EXPECT_LT(canvas.BackgroundDim(), 1.0f);
    for (const HudQuad& q : canvas.Quads()) EXPECT_LT(q.w * q.h, 1280.0f * 720.0f * 0.5f) << "a quad covers half the screen";
    EXPECT_EQ(canvas.Quads()[0].x, menu.PanelRect().x);   // the panel comes first, the text on top
    EXPECT_LT(canvas.Quads().size(), 200u); // cheap: well under the cap
    canvas.Begin({1280.0f, 720.0f});
    EXPECT_EQ(canvas.BackgroundDim(), 0.0f);  // per frame
}

TEST(HudCanvas, DimBackgroundKeepsTheStrongestRequest) {
    HudCanvas canvas;
    canvas.Begin({10.0f, 10.0f});
    canvas.DimBackground(0.3f);
    canvas.DimBackground(0.1f);
    EXPECT_FLOAT_EQ(canvas.BackgroundDim(), 0.3f);
    canvas.DimBackground(5.0f);
    EXPECT_FLOAT_EQ(canvas.BackgroundDim(), 1.0f);
    canvas.Begin({10.0f, 10.0f});
    canvas.DimBackground(-1.0f);
    EXPECT_FLOAT_EQ(canvas.BackgroundDim(), 0.0f);
}

// -------------------------------------------------------------------------------------------------
//  Cursor capture
// -------------------------------------------------------------------------------------------------

TEST(CursorCapture, CapturedOnlyWhilePlayingFocused) {
    CursorCapture capture;
    CursorCapture::Result r = capture.Update(Playing());
    EXPECT_TRUE(r.captured);
    EXPECT_TRUE(r.justCaptured);
    r = capture.Update(Playing());
    EXPECT_TRUE(r.captured);
    EXPECT_FALSE(r.justCaptured);            // edges fire once

    CursorCapture::Inputs in = Playing();
    in.focused = false;                      // focus lost: released
    r = capture.Update(in);
    EXPECT_FALSE(r.captured);
    EXPECT_TRUE(r.justReleased);
    r = capture.Update(in);
    EXPECT_FALSE(r.justReleased);
    r = capture.Update(Playing());           // focus back: captured again
    EXPECT_TRUE(r.justCaptured);
}

TEST(CursorCapture, StartUnfocusedWaits) {
    CursorCapture capture;
    CursorCapture::Inputs in;
    const CursorCapture::Result r = capture.Update(in);
    EXPECT_FALSE(r.captured);
    EXPECT_FALSE(r.justReleased);
    EXPECT_TRUE(capture.Update(Playing()).justCaptured);
}

TEST(CursorCapture, EscapePausesAndResumeRecaptures) {
    CursorCapture capture;
    capture.Update(Playing());
    CursorCapture::Inputs escape = Playing();
    escape.escape = true;
    CursorCapture::Result r = capture.Update(escape);
    EXPECT_TRUE(r.justPaused);
    EXPECT_TRUE(capture.Paused());
    EXPECT_FALSE(r.captured);
    EXPECT_TRUE(r.justReleased);
    // Escape while paused belongs to the menu; an unconsumed one changes nothing here.
    r = capture.Update(escape);
    EXPECT_FALSE(r.justPaused);
    EXPECT_TRUE(capture.Paused());
    // Focus lost while paused does not unpause.
    CursorCapture::Inputs unfocused;
    capture.Update(unfocused);
    EXPECT_TRUE(capture.Paused());
    capture.Resume();
    r = capture.Update(Playing());
    EXPECT_TRUE(r.captured);
    EXPECT_TRUE(r.justCaptured);
}

TEST(CursorCapture, EditorAndFreeCursorRelease) {
    CursorCapture capture;
    capture.Update(Playing());
    CursorCapture::Inputs editor = Playing();
    editor.editorOpen = true;
    EXPECT_TRUE(capture.Update(editor).justReleased);
    EXPECT_FALSE(capture.Update(editor).captured);
    EXPECT_TRUE(capture.Update(Playing()).justCaptured);   // editor closed

    CursorCapture::Inputs alt = Playing();
    alt.freeCursor = true;                                  // debug overlay: hold Alt to click it
    EXPECT_TRUE(capture.Update(alt).justReleased);
    EXPECT_TRUE(capture.Update(Playing()).justCaptured);

    // Escape with the editor open opens the pause menu too.
    editor.escape = true;
    EXPECT_TRUE(capture.Update(editor).justPaused);
}

TEST(CursorCapture, HeadlessIsNeverCaptured) {
    CursorCapture capture;
    CursorCapture::Inputs in = Playing();
    in.headless = true;
    for (int i = 0; i < 3; ++i) {
        const CursorCapture::Result r = capture.Update(in);
        EXPECT_FALSE(r.captured);
        EXPECT_FALSE(r.justCaptured);
        EXPECT_FALSE(r.justReleased);
    }
}

TEST(CursorCapture, CaptureStartsOnlyWithThePointerOverTheWindow) {
    // Wayland locks the pointer only once it enters the window: until then the game is not
    // captured (the HUD asks for a click) and the moment of entry is the capture edge.
    CursorCapture capture;
    CursorCapture::Inputs outside = Playing();
    outside.pointerInside = false;
    CursorCapture::Result r = capture.Update(outside);
    EXPECT_FALSE(r.captured);
    EXPECT_FALSE(r.clicks);
    r = capture.Update(Playing());
    EXPECT_TRUE(r.justCaptured);
    // Once captured the pointer is locked or grabbed: a stale "outside" does not release it.
    r = capture.Update(outside);
    EXPECT_TRUE(r.captured);
    EXPECT_FALSE(r.justReleased);
}

TEST(CursorCapture, TheClickThatStartsTheCaptureIsNotAShot) {
    CursorCapture capture;
    CursorCapture::Inputs in = Playing();
    in.dt = 0.016f;
    in.leftButtonDown = true;                // the click that gave the focus is still held
    CursorCapture::Result r = capture.Update(in);
    EXPECT_TRUE(r.justCaptured);
    EXPECT_FALSE(r.clicks);
    for (int i = 0; i < 30; ++i) EXPECT_FALSE(capture.Update(in).clicks) << "held since the capture";
    in.leftButtonDown = false;
    EXPECT_TRUE(capture.Update(in).clicks);  // released after the grace time: shots are live

    // A press that arrives a frame after the capture (focus and button events in different
    // polls) is still inside the grace time.
    CursorCapture::Inputs unfocused = in;
    unfocused.focused = false;
    capture.Update(unfocused);
    in.leftButtonDown = false;
    r = capture.Update(in);
    EXPECT_TRUE(r.justCaptured);
    EXPECT_FALSE(r.clicks);
    in.leftButtonDown = true;
    EXPECT_FALSE(capture.Update(in).clicks);
    in.leftButtonDown = false;
    for (int i = 0; i < 30; ++i) capture.Update(in);
    EXPECT_TRUE(capture.Update(in).clicks);

    // Released: never clicks.
    CursorCapture::Inputs alt = in;
    alt.freeCursor = true;
    EXPECT_FALSE(capture.Update(alt).clicks);
    // A long frame does not skip the grace time on the capture frame itself.
    in.dt = 1.0f;
    r = capture.Update(in);
    EXPECT_TRUE(r.justCaptured);
    EXPECT_FALSE(r.clicks);
    EXPECT_TRUE(capture.Update(in).clicks);
}
