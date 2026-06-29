#pragma once
// ============================================================================
//  EditorMenuBar — the editor's main menu bar (File → Project / Scene / Import /
//  Cook, Window). Pure UI: it orchestrates LuxEditor operations and enqueues
//  scene-swapping actions through the editor's deferred-action queue (so they
//  run outside the ImGui frame). Registered as the UISystem menu-bar hook.
//
//  Holds a LuxEditor& — the menu bar IS the editor's top-level command surface,
//  so it calls the editor's public operations directly.
//
//  Private editor header (engine/editor/src/app — not installed).
// ============================================================================

namespace lux::editor
{
    class LuxEditor;

    class EditorMenuBar
    {
    public:
        explicit EditorMenuBar(LuxEditor& editor) noexcept : editor_(editor) {}

        /// Paint the menu bar — call inside the UISystem menu-bar hook (a live
        /// ImGui frame, before the dockspace / panel paints).
        void paint();

    private:
        LuxEditor& editor_;
    };

} // namespace lux::editor
