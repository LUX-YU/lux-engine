#pragma once
/**
 * @file EditorEvents.hpp
 * @brief Editor-wide application events that have no single state home — the few
 *        that are genuinely many-to-many. Kept in the StateRegistry alongside
 *        Selection (`states.ensure<EditorEvents>()`), so any subsystem holding
 *        the registry can emit / subscribe without a central wiring class.
 *
 * Scope note (deliberate): this hub holds ONLY `content_changed` today. Scene /
 * project lifecycle is intentionally NOT eventified here:
 *   - `ProjectController::openProject` is an ordered PIPELINE (register assets ->
 *     index -> mount vfs -> point browser -> load scene) whose steps have strict
 *     data dependencies; scattering it across subscribers would trade a clear
 *     sequential algorithm for fragile emit-order coupling.
 *   - Scene load/unload side-effects (viewport texture, selection rebind, the
 *     pick/resize forwarding) are already handled directly where they belong
 *     (SceneController), so a broadcast would add indirection for no gain.
 * Add an event here only when a real, ORDER-INDEPENDENT fan-out appears.
 */

#include <lux/cxx/event/Signal.hpp>

namespace lux::editor
{
    /// The project's on-disk asset set changed — an asset was imported, a
    /// material instance authored, a graph material saved, … Subscribers refresh
    /// their views (the AssetBrowser filesystem walk + the AssetRegistry index).
    /// Carries no payload: the consumers do full, cheap re-scans.
    ///
    /// Why an event: producers (import / create-instance / save-material / the
    /// CLI --import path) used to each call a DIFFERENT subset of {browser
    /// rescan, registry refresh} — so a new texture might be missing from the
    /// pickers, or a new material missing from the browser. Emitting one event
    /// makes every producer refresh BOTH views consistently.
    struct ContentChanged {};

    struct EditorEvents
    {
        lux::cxx::event::Signal<ContentChanged> content_changed;
    };

} // namespace lux::editor
