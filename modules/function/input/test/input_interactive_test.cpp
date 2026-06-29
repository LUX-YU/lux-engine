/// @file input_interactive_test.cpp
/// Interactive input system test with a real GLFW window.
///
/// Opens a window, captures actual keyboard/mouse input through
/// LuxWindow::captureInputSnapshot(), runs the ActionMapper pipeline,
/// and prints state changes to the console so you can verify correctness.
///
/// Usage:
///   1. Run the executable — a window appears.
///   2. Focus the window and press keys / move mouse / scroll.
///   3. Observe console output for action events.
///   4. Press ESC to exit.

#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/input/ActionMapper.hpp>

#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

using namespace lux::input;
using namespace lux::window;

// ═══════════════════════════════════════════════════════════════════════════ //
//  Action definitions — runtime-allocated by InputActionRegistry            //
// ═══════════════════════════════════════════════════════════════════════════ //

namespace Act {
    ActionId Jump          = InvalidActionId;
    ActionId Move          = InvalidActionId;
    ActionId Look          = InvalidActionId;
    ActionId Fire          = InvalidActionId;
    ActionId Aim           = InvalidActionId;
    ActionId Sprint        = InvalidActionId;
    ActionId Zoom          = InvalidActionId;

    // ── Advanced trigger tests ──
    ActionId QuickTap      = InvalidActionId;
    ActionId ChargeRelease = InvalidActionId;
    ActionId AutoFire      = InvalidActionId;
    ActionId SprintFire    = InvalidActionId;
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  Helpers                                                                  //
// ═══════════════════════════════════════════════════════════════════════════ //

struct ActionEntry { const char* name; ActionId id; };

/// Format an InputValue for display.
static const char* fmtValue(const InputValue& v)
{
    // Two alternating buffers so we can format two values in the same printf.
    static char bufs[2][128];
    static int  idx = 0;
    char* buf = bufs[idx];
    idx = 1 - idx;

    switch (v.type) {
    case EInputValueType::BOOL:
        std::snprintf(buf, 128, "%s", v.asBool() ? "true" : "false");
        break;
    case EInputValueType::AXIS_1D:
        std::snprintf(buf, 128, "%.3f", v.as1D());
        break;
    case EInputValueType::AXIS_2D:
        std::snprintf(buf, 128, "(%.2f, %.2f)", v.as2D().x, v.as2D().y);
        break;
    case EInputValueType::AXIS_3D:
        std::snprintf(buf, 128, "(%.2f, %.2f, %.2f)",
                      v.as3D().x, v.as3D().y, v.as3D().z);
        break;
    }
    return buf;
}

/// Build a compact event string from ActionState::events bitfield.
static const char* fmtEvents(uint8_t events)
{
    static char buf[128];
    buf[0] = '\0';

    if (events & ActionEvent_Started)   std::strcat(buf, "STARTED ");
    if (events & ActionEvent_Ongoing)   std::strcat(buf, "ONGOING ");
    if (events & ActionEvent_Triggered) std::strcat(buf, "TRIGGERED ");
    if (events & ActionEvent_Completed) std::strcat(buf, "COMPLETED ");
    if (events & ActionEvent_Canceled)  std::strcat(buf, "CANCELED ");

    // Remove trailing space.
    size_t len = std::strlen(buf);
    if (len > 0 && buf[len - 1] == ' ')
        buf[len - 1] = '\0';

    return buf;
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  Setup                                                                    //
// ═══════════════════════════════════════════════════════════════════════════ //

static void registerActions(InputActionRegistry& reg)
{
    Act::Jump   = reg.registerAction({0, "jump",   EInputValueType::BOOL});
    Act::Fire   = reg.registerAction({0, "fire",   EInputValueType::BOOL});
    Act::Sprint = reg.registerAction({0, "sprint", EInputValueType::BOOL});
    Act::Zoom   = reg.registerAction({0, "zoom",   EInputValueType::AXIS_1D});
    Act::Look   = reg.registerAction({0, "look",   EInputValueType::AXIS_2D});

    {
        InputActionDesc d;
        d.name       = "move";
        d.value_type = EInputValueType::AXIS_2D;
        d.action_modifiers = {{EModifierKind::NORMALIZE_2D}};
        Act::Move = reg.registerAction(d);
    }
    {
        InputActionDesc d;
        d.name       = "aim";
        d.value_type = EInputValueType::BOOL;
        // Hold 0.5s before triggering.
        d.action_triggers = {
            {ETriggerKind::HOLD, ETriggerLogicType::EXPLICIT, 0.5f, 0.5f}
        };
        Act::Aim = reg.registerAction(d);
    }

    // ── Tap: press+release Q within 0.3s ──
    Act::QuickTap = reg.registerAction({0, "quick_tap", EInputValueType::BOOL});

    // ── HoldAndRelease: hold E ≥ 0.4s then release ──
    Act::ChargeRelease = reg.registerAction({0, "charge_release", EInputValueType::BOOL});

    // ── Pulse: hold R, fires every 0.2s ──
    Act::AutoFire = reg.registerAction({0, "auto_fire", EInputValueType::BOOL});

    // ── ChordAction: Sprint must be active, then press F ──
    Act::SprintFire = reg.registerAction({0, "sprint_fire", EInputValueType::BOOL});
}

static void setupBindings(ActionMap& am)
{
    // Jump: Space
    am.bindKey(Act::Jump, KeyEnum::KEY_SPACE,
               InputValue::makeBool(true));

    // Move: WASD → Axis2D
    am.bindKey(Act::Move, KeyEnum::KEY_W, InputValue::makeAxis2D( 0.f,  1.f));
    am.bindKey(Act::Move, KeyEnum::KEY_S, InputValue::makeAxis2D( 0.f, -1.f));
    am.bindKey(Act::Move, KeyEnum::KEY_A, InputValue::makeAxis2D(-1.f,  0.f));
    am.bindKey(Act::Move, KeyEnum::KEY_D, InputValue::makeAxis2D( 1.f,  0.f));

    // Look: mouse delta → Axis2D
    am.bindMouseAxis(Act::Look, MouseAxisInput::EAxis::DELTA_X,
                     InputValue::makeAxis2D(1.f, 0.f));
    am.bindMouseAxis(Act::Look, MouseAxisInput::EAxis::DELTA_Y,
                     InputValue::makeAxis2D(0.f, 1.f));

    // Fire: left click
    am.bindMouseButton(Act::Fire, MouseButton::MOUSE_BUTTON_LEFT,
                       InputValue::makeBool(true));

    // Aim: right click (Hold 0.5s trigger defined on the action)
    am.bindMouseButton(Act::Aim, MouseButton::MOUSE_BUTTON_RIGHT,
                       InputValue::makeBool(true));

    // Sprint: left shift
    am.bindKey(Act::Sprint, KeyEnum::KEY_LEFT_SHIFT,
               InputValue::makeBool(true));

    // Zoom: scroll wheel → Axis1D
    am.bindMouseAxis(Act::Zoom, MouseAxisInput::EAxis::SCROLL_Y,
                     InputValue::makeAxis1D(1.f));

    // ── Advanced trigger bindings ──

    // QuickTap: Q key, Tap trigger (must release within 0.3s)
    am.bindKey(Act::QuickTap, KeyEnum::KEY_Q,
               InputValue::makeBool(true), {},
               {{ETriggerKind::TAP, ETriggerLogicType::EXPLICIT, 0.5f, 0.0f, 0.3f}});

    // ChargeRelease: E key, HoldAndRelease trigger (hold ≥ 0.4s then release)
    am.bindKey(Act::ChargeRelease, KeyEnum::KEY_E,
               InputValue::makeBool(true), {},
               {{ETriggerKind::HOLD_AND_RELEASE, ETriggerLogicType::EXPLICIT, 0.5f, 0.4f}});

    // AutoFire: R key, Pulse trigger (fires every 0.2s while held)
    am.bindKey(Act::AutoFire, KeyEnum::KEY_R,
               InputValue::makeBool(true), {},
               {{ETriggerKind::PULSE, ETriggerLogicType::EXPLICIT, 0.5f, 0.0f, 0.2f, 0.2f}});

    // SprintFire: F key, ChordAction trigger (requires Sprint to be active)
    am.bindKey(Act::SprintFire, KeyEnum::KEY_F,
               InputValue::makeBool(true), {},
               {{ETriggerKind::CHORD_ACTION, ETriggerLogicType::EXPLICIT, 0.5f, 0.0f, 0.2f, 0.1f, Act::Sprint}});
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  Main                                                                     //
// ═══════════════════════════════════════════════════════════════════════════ //

int main()
{
    // ── GLFW init ───────────────────────────────────────────────────── //
    GlfwRuntime runtime;
    if (!runtime.valid()) {
        std::fprintf(stderr, "FATAL: glfwInit() failed\n");
        return 1;
    }

    // ── Window ──────────────────────────────────────────────────────── //
    LuxWindow window(960, 540, "[ Input System Test ] - Focus this window");
    if (!window.init()) {
        std::fprintf(stderr,
            "FATAL: window.init() failed (Vulkan not available?)\n");
        return 1;
    }

    // ── ActionMapper ────────────────────────────────────────────────── //
    ActionMapper mapper;
    registerActions(mapper.actionRegistry());

    InputContext ctx("gameplay");
    setupBindings(ctx.actionMap());

    InputContextStack stack;
    stack.push(&ctx);

    // ── Action tracking ─────────────────────────────────────────────── //
    constexpr int kNumActions = 11;
    ActionEntry entries[kNumActions] = {
        {"Jump",       Act::Jump},
        {"Move",       Act::Move},
        {"Look",       Act::Look},
        {"Fire",       Act::Fire},
        {"Aim",        Act::Aim},
        {"Sprint",     Act::Sprint},
        {"Zoom",       Act::Zoom},
        {"QuickTap",   Act::QuickTap},
        {"ChrgRel",    Act::ChargeRelease},
        {"AutoFire",   Act::AutoFire},
        {"SprintFire", Act::SprintFire},
    };

    // Track previous frame events per action for change detection.
    uint8_t prev_events[kNumActions] = {};

    // ── Print instructions ──────────────────────────────────────────── //
    std::printf(
        "===================================================\n"
        "  Interactive Input System Test\n"
        "===================================================\n"
        "  Focus the window, then:\n"
        "\n"
        "  SPACE         -> Jump    (Bool)\n"
        "  W/A/S/D       -> Move    (Axis2D, Normalized)\n"
        "  Mouse move    -> Look    (Axis2D)\n"
        "  Left click    -> Fire    (Bool)\n"
        "  Right hold    -> Aim     (Bool, Hold 0.5s)\n"
        "  Left Shift    -> Sprint  (Bool)\n"
        "  Scroll wheel  -> Zoom    (Axis1D)\n"
        "\n"
        "  --- Advanced Triggers ---\n"
        "  Q (tap)       -> QuickTap     (Tap: release within 0.3s)\n"
        "  E (hold+rel)  -> ChargeRelease(HoldAndRelease: hold>=0.4s)\n"
        "  R (hold)      -> AutoFire     (Pulse: every 0.2s)\n"
        "  Shift + F     -> SprintFire   (ChordAction: Sprint active)\n"
        "\n"
        "  ESC           -> Exit\n"
        "===================================================\n"
        "\n"
        "  Output: only printed when action events change.\n"
        "\n"
    );

    // ── Main loop ───────────────────────────────────────────────────── //
    int frame = 0;

    while (!window.shouldClose())
    {
        LuxWindow::pollEvents();
        auto snap = window.captureInputSnapshot();

        if (snap.isKeyJustPressed(KeyEnum::KEY_ESCAPE)) {
            window.exit();
            break;
        }

        float dt = snap.sample_dt > 0.f ? snap.sample_dt : 1.f / 60.f;
        mapper.update(snap, stack, dt,
                      !snap.keyboard_captured_by_ui,
                      !snap.mouse_captured_by_ui);

        ++frame;

        // Print only when events change for an action.
        for (int i = 0; i < kNumActions; ++i) {
            const auto& st = mapper.state(entries[i].id);

            if (st.events == ActionEvent_None && prev_events[i] == ActionEvent_None)
                continue; // Nothing happening.

            if (st.events == prev_events[i])
                continue; // Same event pattern as last frame — skip.

            // Events changed — print this line.
            if (st.events != ActionEvent_None) {
                std::printf("[frame %05d] %-12s  %-30s  value=%-20s  held=%.2fs\n",
                            frame,
                            entries[i].name,
                            fmtEvents(st.events),
                            fmtValue(st.value),
                            st.held_seconds);
            }

            prev_events[i] = st.events;
        }

        // Throttle to ~60fps to avoid burning CPU.
        std::this_thread::sleep_for(std::chrono::milliseconds(16));
    }

    std::printf("\n=== Test ended at frame %d ===\n", frame);
    return 0;
}
