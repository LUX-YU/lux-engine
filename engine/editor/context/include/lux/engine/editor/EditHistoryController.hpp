#pragma once
#include <lux/engine/editor/ActiveEditHistory.hpp>
#include <lux/engine/editor/context/visibility.h>
#include <lux/engine/object/Object.hpp>
#include <lux/engine/object/ObjectAnnotations.hpp>
namespace lux::editor
{
    struct EditHistoryActionFailure final
    {
        HistoryTargetHandle target;
        editing::EHistoryAction action{editing::EHistoryAction::UNDO};
        editing::EditFailure failure;
    };
    class LUX_EDITOR_CONTEXT_PUBLIC LUX_OBJECT() EditHistoryController final
        : public object::Object<EditHistoryController>
    {
    public:
        static const signal_type<EditHistoryActionFailure> failed;
        explicit EditHistoryController(object::ObjectDispatcherRef dispatcher, ActiveEditHistory& histories) noexcept;
        ~EditHistoryController() override;
        void undo() noexcept;
        void redo() noexcept;
        [[nodiscard]] bool canUndo() const noexcept;
        [[nodiscard]] bool canRedo() const noexcept;
        [[nodiscard]] std::string_view undoLabel() const noexcept;
        [[nodiscard]] std::string_view redoLabel() const noexcept;

    private:
        ActiveEditHistory* histories_{};
    };
} // namespace lux::editor
