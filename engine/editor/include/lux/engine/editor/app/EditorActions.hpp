#pragma once
/**
 * @file EditorActions.hpp
 * @brief Editor-wide ActionId table for the input/action layer.
 *
 * One `ActionId` slot per editor-side input (picking trigger, camera mode
 * holds, fly-mode WASD axes, focus / reset hotkeys, …). The IDs themselves
 * are runtime-allocated by `InputActionRegistry::registerAction` —
 * `registerAll(mapper)` fills the inline statics with the freshly minted
 * values and also installs a default `InputContext` carrying the bindings
 * (key → action, mouse button → action, mouse axis → action).
 *
 * The variables below are deliberately `inline static` so every TU that
 * `#include`s this header sees the same address — there is no DLL boundary
 * issue because they live in editor-side code that links statically into
 * `lux_editor.exe`.
 *
 * Usage:
 *   1. After `meta_module_init()` and the GLFW window are up, but before the
 *      main loop starts:
 *          actions::registerAll(*mapper);
 *          mapper->pushContext(actions::editorContext());
 *   2. Inside the main loop, after `actionMapper->update(snapshot, ...)`,
 *      query the IDs via `mapper.active(actions::CameraFlyMode)` etc.
 */

#include <lux/engine/input/ActionId.hpp>

namespace lux::input { class ActionMapper; class InputContext; }

namespace lux::editor::actions
{
    // ── Picking ──────────────────────────────────────────────────────────

    /// LMB click in the viewport. Triggered-pulse semantics — fires once
    /// per press, regardless of how long the button is held.
    inline input::ActionId PickPrimary       = input::InvalidActionId;

    // ── UE-style camera: mode holds ─────────────────────────────────────

    /// RMB held → Fly mode (WASD + mouse look + scroll-wheel speed).
    inline input::ActionId CameraFlyMode     = input::InvalidActionId;

    /// LMB held → Orbit mode (when not over a pickable item).
    inline input::ActionId CameraOrbitMode   = input::InvalidActionId;

    /// MMB held → Pan mode (screen-space drag of focal target).
    inline input::ActionId CameraPanMode     = input::InvalidActionId;

    /// Alt held → modifier; combined with RMB / LMB switches modes.
    inline input::ActionId CameraAltModifier = input::InvalidActionId;

    // ── UE-style camera: fly-mode axes ──────────────────────────────────

    /// W (+1) / S (-1) along view forward.
    inline input::ActionId CameraMoveForward = input::InvalidActionId;

    /// D (+1) / A (-1) along view right.
    inline input::ActionId CameraMoveRight   = input::InvalidActionId;

    /// E (+1) / Q (-1) along world up.
    inline input::ActionId CameraMoveUp      = input::InvalidActionId;

    /// Mouse delta X — yaw. Only sampled when CameraFlyMode is active.
    inline input::ActionId CameraLookYaw     = input::InvalidActionId;

    /// Mouse delta Y — pitch. Only sampled when CameraFlyMode is active.
    inline input::ActionId CameraLookPitch   = input::InvalidActionId;

    /// Scroll-wheel during fly mode — adjusts fly speed multiplicatively.
    inline input::ActionId CameraSpeedScroll = input::InvalidActionId;

    // ── UE-style camera: hotkeys ────────────────────────────────────────

    /// F — focus the camera on the currently selected entity.
    inline input::ActionId CameraFocusSelected = input::InvalidActionId;

    /// End — reset camera to the editor scene's initial pose.
    inline input::ActionId CameraReset         = input::InvalidActionId;

    // ── API ─────────────────────────────────────────────────────────────

    /// Register every action above into @p mapper's `InputActionRegistry`.
    /// Idempotent — re-registering the same names returns the existing IDs.
    /// Must be called BEFORE `editorContext()` is queried.
    void registerAll(input::ActionMapper& mapper);

    /// Lazily-built `InputContext` carrying the default editor bindings
    /// (WASD / QE / F / End / mouse buttons / mouse axes / scroll wheel).
    /// The returned pointer is owned by an internal `unique_ptr` with
    /// static lifetime; callers `push()` it on their `InputContextStack`.
    /// `registerAll()` MUST have been called at least once first.
    input::InputContext* editorContext();

} // namespace lux::editor::actions
