#include <lux/engine/editor/app/EditorActions.hpp>

#include <lux/engine/input/ActionMapper.hpp>
#include <lux/engine/input/InputContext.hpp>
#include <lux/engine/input/PhysicalInput.hpp>

#include <memory>
#include <utility>

namespace lux::editor::actions
{
    namespace
    {
        // Lazily-built singleton editor context. Lives for the editor
        // process's lifetime; `editorContext()` returns the raw pointer for
        // `InputContextStack::push`. We can't use a Meyers-static-local
        // `InputContext` because we need to defer construction until after
        // registerAll() has stamped the action IDs.
        std::unique_ptr<input::InputContext> g_ctx;

        void buildContext()
        {
            if (g_ctx) return;

            auto ctx = std::make_unique<input::InputContext>(
                "EditorViewport",
                /*consumes_keyboard=*/false,
                /*consumes_mouse=*/false,
                /*priority=*/0);
            auto& m = ctx->actionMap();

            using K = lux::input::EKey;
            using B = lux::input::EMouseButton;
            using A = input::MouseAxisInput::EAxis;

            // ── Picking ────────────────────────────────────────────────
            // LMB is shared with CameraOrbitMode below; picking vs orbit
            // disambiguation is the responsibility of the consumer (one
            // sample LMB pulse on press = pick; LMB held + drag = orbit).
            m.bindMouseButton(PickPrimary,       B::MOUSE_BUTTON_LEFT);

            // ── Camera mode holds ─────────────────────────────────────
            m.bindMouseButton(CameraFlyMode,     B::MOUSE_BUTTON_RIGHT);
            m.bindMouseButton(CameraOrbitMode,   B::MOUSE_BUTTON_LEFT);
            m.bindMouseButton(CameraPanMode,     B::MOUSE_BUTTON_MIDDLE);
            m.bindKey       (CameraAltModifier,  K::KEY_LEFT_ALT);
            m.bindKey       (CameraAltModifier,  K::KEY_RIGHT_ALT);

            // ── Fly-mode axes ─────────────────────────────────────────
            // Each key contributes ±1 to the action's Axis1D value; the
            // ActionMapper sums all matching bindings, so W+S gives 0.
            m.bindKey(CameraMoveForward, K::KEY_W,
                      input::InputValue::makeAxis1D(+1.f));
            m.bindKey(CameraMoveForward, K::KEY_S,
                      input::InputValue::makeAxis1D(-1.f));

            m.bindKey(CameraMoveRight,   K::KEY_D,
                      input::InputValue::makeAxis1D(+1.f));
            m.bindKey(CameraMoveRight,   K::KEY_A,
                      input::InputValue::makeAxis1D(-1.f));

            m.bindKey(CameraMoveUp,      K::KEY_E,
                      input::InputValue::makeAxis1D(+1.f));
            m.bindKey(CameraMoveUp,      K::KEY_Q,
                      input::InputValue::makeAxis1D(-1.f));

            // Mouse delta — raw axis scale stays 1.0 here; the controller
            // multiplies by user-facing sensitivity (config::look_sensitivity).
            m.bindMouseAxis(CameraLookYaw,   A::DELTA_X);
            m.bindMouseAxis(CameraLookPitch, A::DELTA_Y);

            // Scroll wheel (vertical only) → fly-speed change.
            m.bindMouseAxis(CameraSpeedScroll, A::SCROLL_Y);

            // ── Hotkeys ───────────────────────────────────────────────
            m.bindKey(CameraFocusSelected, K::KEY_F);
            m.bindKey(CameraReset,         K::KEY_END);

            g_ctx = std::move(ctx);
        }
    } // namespace

    void registerAll(input::ActionMapper& mapper)
    {
        auto& reg = mapper.actionRegistry();

        auto ensure = [&](input::ActionId& slot,
                          const char*         name,
                          input::EInputValueType vt)
        {
            input::InputActionDesc desc;
            desc.name       = name;
            desc.value_type = vt;
            // Camera actions explicitly do NOT consume — picking and the
            // camera both listen to LMB / RMB / mouse axes simultaneously,
            // disambiguated by the controller's mode state machine.
            desc.consume_input = false;
            slot = reg.registerAction(std::move(desc));
        };

        using VT = input::EInputValueType;

        ensure(PickPrimary,         "editor.pick.primary",      VT::BOOL);

        ensure(CameraFlyMode,       "editor.camera.flyMode",    VT::BOOL);
        ensure(CameraOrbitMode,     "editor.camera.orbitMode",  VT::BOOL);
        ensure(CameraPanMode,       "editor.camera.panMode",    VT::BOOL);
        ensure(CameraAltModifier,   "editor.camera.altMod",     VT::BOOL);

        ensure(CameraMoveForward,   "editor.camera.moveFwd",    VT::AXIS_1D);
        ensure(CameraMoveRight,     "editor.camera.moveRight",  VT::AXIS_1D);
        ensure(CameraMoveUp,        "editor.camera.moveUp",     VT::AXIS_1D);
        ensure(CameraLookYaw,       "editor.camera.lookYaw",    VT::AXIS_1D);
        ensure(CameraLookPitch,     "editor.camera.lookPitch",  VT::AXIS_1D);
        ensure(CameraSpeedScroll,   "editor.camera.speedScroll",VT::AXIS_1D);

        ensure(CameraFocusSelected, "editor.camera.focus",      VT::BOOL);
        ensure(CameraReset,         "editor.camera.reset",      VT::BOOL);

        // Build the default-binding context once the IDs are known.
        buildContext();
    }

    input::InputContext* editorContext()
    {
        // Caller invariant: registerAll() must have run first.
        return g_ctx.get();
    }

} // namespace lux::editor::actions
