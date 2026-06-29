#pragma once
/**
 * @file HierarchyPanel.hpp
 * @brief Lists every entity in the current ECS registry; clicking one sets the
 *        shared selection (read by InspectorPanel + the scene's F-focus/gizmo).
 *
 * Reads the editor's shared `Selection` state (ensured from the `StateRegistry`
 * at construction) every frame — registry + current selection — and writes the
 * selection back on click. No callbacks, no replicated state.
 */

#include <lux/engine/editor/visibility.h>
#include <lux/engine/ui/Panel.hpp>

#include <memory>
#include <string>

namespace lux::editor
{
    class StateRegistry;
    class Selection;

    class LUX_EDITOR_PUBLIC HierarchyPanel : public lux::ui::Panel
    {
    public:
        /// @param states  Editor state registry; the panel ensures + caches the
        ///        shared `Selection` and reads/writes it each frame.
        HierarchyPanel(std::string title, StateRegistry& states);
        ~HierarchyPanel() override = default;

    protected:
        void paint() override;

    private:
        std::shared_ptr<Selection> sel_; ///< ensured at ctor; refcounted; read + write each frame
    };

} // namespace lux::editor
