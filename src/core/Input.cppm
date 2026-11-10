module;

#include <array>
#include <cstddef>
#include <cstdint>

export module vortex.core:input;

export namespace vortex {

    /// @brief Keyboard keys. Values match GLFW key codes so the platform layer can pass them through.
    enum class Key : int {
        Space = 32, Apostrophe = 39, Comma = 44, Minus = 45, Period = 46, Slash = 47,
        Num0 = 48, Num1, Num2, Num3, Num4, Num5, Num6, Num7, Num8, Num9,
        Semicolon = 59, Equal = 61,
        A = 65, B, C, D, E, F, G, H, I, J, K, L, M, N, O, P, Q, R, S, T, U, V, W, X, Y, Z,
        LeftBracket = 91, Backslash = 92, RightBracket = 93, GraveAccent = 96,
        Escape = 256, Enter, Tab, Backspace, Insert, Delete, Right, Left, Down, Up,
        PageUp, PageDown, Home, End,
        CapsLock = 280, ScrollLock, NumLock, PrintScreen, Pause,
        F1 = 290, F2, F3, F4, F5, F6, F7, F8, F9, F10, F11, F12,
        LeftShift = 340, LeftControl, LeftAlt, LeftSuper, RightShift, RightControl, RightAlt, RightSuper
    };

    enum class MouseButton : int { Left = 0, Right = 1, Middle = 2 };

    /**
     * @brief Per-frame input state.
     * @details The platform layer feeds events through the On* functions; game code only reads.
     * "Pressed" / "Released" are edges that are true for exactly one frame.
     * While the UI wants the mouse or keyboard, game-facing queries report "not pressed" so a
     * click on an editor panel never also digs a hole in the world.
     */
    class Input {
    public:
        static constexpr int kKeyCount = 512;
        static constexpr int kButtonCount = 8;

        // --- queries -------------------------------------------------------------------
        bool KeyDown(Key k) const { return !m_KeyboardCaptured && Get(m_Keys, static_cast<int>(k)); }
        bool KeyPressed(Key k) const {
            return !m_KeyboardCaptured && Get(m_Keys, static_cast<int>(k)) && !Get(m_PrevKeys, static_cast<int>(k));
        }
        bool KeyReleased(Key k) const { return !Get(m_Keys, static_cast<int>(k)) && Get(m_PrevKeys, static_cast<int>(k)); }

        bool MouseDown(MouseButton b) const { return !m_MouseCaptured && GetButton(m_Buttons, static_cast<int>(b)); }
        bool MousePressed(MouseButton b) const {
            return !m_MouseCaptured && GetButton(m_Buttons, static_cast<int>(b)) && !GetButton(m_PrevButtons, static_cast<int>(b));
        }
        bool MouseReleased(MouseButton b) const {
            return !GetButton(m_Buttons, static_cast<int>(b)) && GetButton(m_PrevButtons, static_cast<int>(b));
        }

        double MouseX() const { return m_MouseX; }
        double MouseY() const { return m_MouseY; }
        /// @brief Cursor movement since the previous frame, in pixels.
        double MouseDeltaX() const { return m_DeltaX; }
        double MouseDeltaY() const { return m_DeltaY; }
        /// @brief Scroll wheel movement accumulated during this frame.
        double ScrollY() const { return m_MouseCaptured ? 0.0 : m_ScrollY; }

        bool MouseCapturedByUi() const { return m_MouseCaptured; }
        bool KeyboardCapturedByUi() const { return m_KeyboardCaptured; }

        // --- platform side ---------------------------------------------------------------
        /// @brief Call once per frame before polling events.
        void NewFrame() {
            m_PrevKeys = m_Keys;
            m_PrevButtons = m_Buttons;
            m_DeltaX = 0.0;
            m_DeltaY = 0.0;
            m_ScrollY = 0.0;
        }
        void OnKey(int key, bool down) {
            if (key >= 0 && key < kKeyCount) m_Keys[static_cast<size_t>(key)] = down;
        }
        void OnMouseButton(int button, bool down) {
            if (button >= 0 && button < kButtonCount) m_Buttons[static_cast<size_t>(button)] = down;
        }
        void OnCursor(double x, double y) {
            if (m_HasCursor) {
                m_DeltaX += x - m_MouseX;
                m_DeltaY += y - m_MouseY;
            }
            m_MouseX = x;
            m_MouseY = y;
            m_HasCursor = true;
        }
        /// @brief Forget the last cursor position (after toggling cursor capture) to avoid a jump.
        void ResetCursorDelta() { m_HasCursor = false; m_DeltaX = 0.0; m_DeltaY = 0.0; }
        void OnScroll(double y) { m_ScrollY += y; }
        void SetUiCapture(bool mouse, bool keyboard) { m_MouseCaptured = mouse; m_KeyboardCaptured = keyboard; }
        /// @brief Releases everything (window lost focus).
        void Clear() { m_Keys.fill(false); m_Buttons.fill(false); }

    private:
        static bool Get(const std::array<bool, kKeyCount>& a, int i) { return i >= 0 && i < kKeyCount && a[static_cast<size_t>(i)]; }
        static bool GetButton(const std::array<bool, kButtonCount>& a, int i) { return i >= 0 && i < kButtonCount && a[static_cast<size_t>(i)]; }

        std::array<bool, kKeyCount> m_Keys{};
        std::array<bool, kKeyCount> m_PrevKeys{};
        std::array<bool, kButtonCount> m_Buttons{};
        std::array<bool, kButtonCount> m_PrevButtons{};
        double m_MouseX = 0.0, m_MouseY = 0.0;
        double m_DeltaX = 0.0, m_DeltaY = 0.0;
        double m_ScrollY = 0.0;
        bool m_HasCursor = false;
        bool m_MouseCaptured = false;
        bool m_KeyboardCaptured = false;
    };
}
