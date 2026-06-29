#pragma once
#include <lux/engine/window/LuxWindowDefination.hpp>
#include <bitset>
#include <variant>
#include <vector>
#include <cstdint>

namespace lux::window
{
    // -------------------------------------------------------------------------
    // CharInput — a Unicode codepoint from glfwSetCharCallback.
    // Use InputSnapshot::text_events for any text-entry or IME feature.
    // Do NOT use KeyAction for text input — it cannot handle IME / composition.
    // -------------------------------------------------------------------------
    struct CharInput { uint32_t codepoint; };

    // -------------------------------------------------------------------------
    // InputEvent — a single discrete event captured on the main thread
    // and drained once per snapshot.
    // -------------------------------------------------------------------------
    using InputEvent = std::variant<
        KeyAction,          ///< Key press / release / repeat
        MouseButtonAction,  ///< Mouse button press / release
        MouseScrollAction   ///< Scroll-wheel delta
    >;

    // -------------------------------------------------------------------------
    // InputSnapshot — point-in-time input state capture
    // -------------------------------------------------------------------------

    /**
     * @brief Immutable per-frame input state, produced by the main thread and
     *        consumed by the game thread via TripleBuffer<InputSnapshot>.
     *
     * After `glfwPollEvents()` the main thread calls
     * `LuxWindow::captureInputSnapshot()`, which:
     *   - Copies callback-maintained held-key state and drains the
     *     per-frame just-pressed / just-released edge accumulators.
     *   - Copies callback-maintained mouse button state likewise.
     *   - Records cursor position and computes the displacement delta.
     *   - Accumulates scroll delta from GLFW scroll callbacks.
     *   - Drains the accumulated event buffer (filled by GLFW callbacks).
     *   - Snapshots window / framebuffer geometry and timing.
     *
     * The resulting value is then written into a `TripleBuffer<InputSnapshot>`
     * so the game thread never calls any GLFW function directly.
     *
     * #### Edge detection
     * `keys_just_pressed` and `keys_just_released` (and their mouse-button
     * counterparts) are accumulated directly by the GLFW key/mouse-button
     * callbacks, so even a press-then-release that occurs within a single
     * frame is never lost.  They are NOT derived by XOR-ing held bitmasks.
     *
     * #### Game-thread usage example
     * @code
     *   // Held key — camera movement
     *   if (input.isKeyHeld(KeyEnum::KEY_W))
     *       cam_pos += front * speed * input.sample_dt;
     *
     *   // Just-pressed — one-shot action, no manual edge-detection needed
     *   if (input.isKeyJustPressed(KeyEnum::KEY_SPACE))
     *       player.jump();
     *
     *   // Mouse pick on left-button just-pressed
     *   if (input.isMouseButtonJustPressed(MouseButton::MOUSE_BUTTON_LEFT))
     *       triggerPick(input.cursor_x, input.cursor_y);
     *
     *   // Scroll — direct member, no need to iterate events
     *   cam_fov -= input.scroll_dy * zoom_speed;
     *
     *   // Rare: iterate discrete events for text input / exact ordering
     *   for (const auto& ev : input.events)
     *   {
     *       if (auto* ka = std::get_if<KeyAction>(&ev))
     *           handleTextInput(*ka);
     *   }
     * @endcode
     */
    struct InputSnapshot
    {
        // ----------------------------------------------------------------
        // Keyboard — 3 × 64 bytes = 192 bytes  (vs 2048 bytes before)
        // Indexed by GLFW key code (= KeyEnum cast to size_t).
        // Valid GLFW key codes run from 32 (SPACE) to 348 (MENU).
        // ----------------------------------------------------------------

        /// Key is currently held down (maintained by key callback).
        std::bitset<512> keys_held;

        /// Key transitioned from up → down this frame (accumulated by callback).
        std::bitset<512> keys_just_pressed;

        /// Key transitioned from down → up this frame (accumulated by callback).
        std::bitset<512> keys_just_released;

        // ----------------------------------------------------------------
        // Mouse buttons — one bit per button, packed into three bytes.
        // Bit index matches MouseButton cast to int (0 = LEFT … 7).
        // ----------------------------------------------------------------

        /// Mouse button is currently held down.
        uint8_t mouse_held{0};

        /// Mouse button transitioned from up → down this frame.
        uint8_t mouse_just_pressed{0};

        /// Mouse button transitioned from down → up this frame.
        uint8_t mouse_just_released{0};

        // ----------------------------------------------------------------
        // Cursor
        // ----------------------------------------------------------------

        /// Absolute cursor position in client-area screen coordinates (pixels).
        double cursor_x{0.0};
        double cursor_y{0.0};

        /// Cursor displacement since the previous captureInputSnapshot() call.
        /// Negative delta means left / up movement.
        double cursor_dx{0.0};
        double cursor_dy{0.0};

        // ----------------------------------------------------------------
        // Scroll — accumulated delta from GLFW scroll callbacks this frame.
        // Symmetric with cursor_dx / cursor_dy; no need to iterate events.
        // ----------------------------------------------------------------

        /// Horizontal scroll delta (positive = right).
        double scroll_dx{0.0};

        /// Vertical scroll delta (positive = up / toward user).
        double scroll_dy{0.0};

        // ----------------------------------------------------------------
        // Window / framebuffer geometry
        // Cached here so the game thread never queries the window directly.
        // ----------------------------------------------------------------

        /// Window client-area size in OS screen coordinates.
        uint32_t window_width{0};
        uint32_t window_height{0};

        /// Framebuffer size in actual pixels (may differ on HiDPI displays).
        uint32_t framebuffer_width{0};
        uint32_t framebuffer_height{0};

        // ----------------------------------------------------------------
        // Discrete events accumulated by GLFW callbacks.
        // Prefer the bitset helpers above for per-frame logic.
        // Iterate events only for key combos or exact ordering.
        // ----------------------------------------------------------------

        /// All key-press, mouse-button, and scroll events from this poll cycle,
        /// in callback order.
        std::vector<InputEvent> events;

        /// Unicode text input this frame (from glfwSetCharCallback).
        /// Use this — not events — for any text-entry or IME feature.
        std::vector<CharInput> text_events;

        // ----------------------------------------------------------------
        // UI-layer capture flags
        // Set by the application AFTER calling ImGui's (or another UI
        // framework's) input processing, and BEFORE posting the snapshot
        // to the game thread.  Default false — safe to leave unset when
        // no UI framework is present.
        // ----------------------------------------------------------------

        /// True when a UI layer is consuming keyboard input this frame.
        /// The game thread should skip game-keyboard actions when set.
        bool keyboard_captured_by_ui{false};

        /// True when a UI layer is consuming mouse input this frame.
        /// The game thread should skip game-mouse actions when set.
        bool mouse_captured_by_ui{false};

        // ----------------------------------------------------------------
        // Timing — measured on the main/platform thread.
        // sample_dt reflects the input-sampling interval (= main-thread
        // frame interval).  In a blocked 1:1 game-thread design this equals
        // the simulation step; in a decoupled design the game thread should
        // use its own high-resolution clock instead.
        // ----------------------------------------------------------------

        /// glfwGetTime() at the moment captureInputSnapshot() was called.
        double sample_timestamp{0.0};

        /// Seconds elapsed since the previous captureInputSnapshot() call
        /// (main-thread frame interval, NOT necessarily the game tick dt).
        float sample_dt{0.0f};

        // ----------------------------------------------------------------
        // Helpers — keyboard
        // std::bitset<512> default-initializes to all zeros (= all released),
        // so no explicit constructor is needed.
        // ----------------------------------------------------------------

        /// Returns true if the key is currently held down.
        [[nodiscard]] bool isKeyHeld(KeyEnum k) const noexcept
        {
            const auto i = static_cast<size_t>(static_cast<int>(k));
            return i < keys_held.size() && keys_held[i];
        }

        /// Returns true if the key went down this frame (up → down transition).
        [[nodiscard]] bool isKeyJustPressed(KeyEnum k) const noexcept
        {
            const auto i = static_cast<size_t>(static_cast<int>(k));
            return i < keys_just_pressed.size() && keys_just_pressed[i];
        }

        /// Returns true if the key went up this frame (down → up transition).
        [[nodiscard]] bool isKeyJustReleased(KeyEnum k) const noexcept
        {
            const auto i = static_cast<size_t>(static_cast<int>(k));
            return i < keys_just_released.size() && keys_just_released[i];
        }

        // ----------------------------------------------------------------
        // Helpers — mouse buttons
        // ----------------------------------------------------------------

        /// Returns true if the mouse button is currently held down.
        [[nodiscard]] bool isMouseButtonHeld(MouseButton btn) const noexcept
        {
            const auto i = static_cast<int>(btn);
            return i >= 0 && i < 8 && ((mouse_held >> i) & 1u);
        }

        /// Returns true if the mouse button went down this frame.
        [[nodiscard]] bool isMouseButtonJustPressed(MouseButton btn) const noexcept
        {
            const auto i = static_cast<int>(btn);
            return i >= 0 && i < 8 && ((mouse_just_pressed >> i) & 1u);
        }

        /// Returns true if the mouse button went up this frame.
        [[nodiscard]] bool isMouseButtonJustReleased(MouseButton btn) const noexcept
        {
            const auto i = static_cast<int>(btn);
            return i >= 0 && i < 8 && ((mouse_just_released >> i) & 1u);
        }
    };

} // namespace lux::window
