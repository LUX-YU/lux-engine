#pragma once

#include <lux/engine/editor/EditorContext.hpp>
#include <lux/engine/editor/inspector/ComponentEditorBinding.hpp>
#include <lux/engine/editor/inspector/InspectorUndoJournal.hpp>
#include <lux/engine/editor/inspector/visibility.h>
#include <lux/engine/ui/Pane.hpp>

#include <cstddef>

namespace lux::editor::inspector
{
    enum class EInspectorMode : std::uint8_t
    {
        EDITABLE,
        READ_ONLY,
    };
    inline constexpr lux::ui::PaneTypeIdView kEntityInspectorPaneType{"lux.editor.entity-inspector"};

    struct InspectorDrawStats final
    {
        std::size_t visible_components{};
        std::size_t missing_bindings{};
        bool stale_selection{};
    };

    class LUX_EDITOR_INSPECTOR_PUBLIC EntityInspector final : public object::Object<EntityInspector, lux::ui::Pane>
    {
    public:
        EntityInspector(
            object::ObjectDispatcherRef dispatcher,
            lux::ui::PaneId id,
            EditorContext& context,
            ComponentEditorBindingTable bindings,
            EInspectorMode mode = EInspectorMode::EDITABLE
        );

        [[nodiscard]] InspectorUndoJournal& undoJournal() noexcept;
        [[nodiscard]] const InspectorUndoJournal& undoJournal() const noexcept;
        [[nodiscard]] const InspectorDrawStats& lastDrawStats() const noexcept;

    protected:
        void draw(lux::ui::Frame& frame, lux::ui::PaneDrawContext& context) override;

    private:
        EditorContext* context_{};
        ComponentEditorBindingTable bindings_;
        InspectorUndoJournal undo_;
        InspectorDrawStats last_draw_;
        EInspectorMode mode_;
    };
} // namespace lux::editor::inspector
