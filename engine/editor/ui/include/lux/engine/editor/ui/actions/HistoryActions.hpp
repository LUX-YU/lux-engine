#pragma once

#include <lux/engine/editor/ui/visibility.h>
#include <lux/engine/editor/editing/EditHistoryTarget.hpp>
#include <lux/engine/editor/ui/actions/ActiveEditHistory.hpp>
#include <lux/engine/object/Object.hpp>
#include <lux/engine/object/ObjectAnnotations.hpp>
#include <thread>

namespace lux::editor::ui
{
    struct HistoryActionFailure final
    {
        editing::HistoryId target;
        editing::EHistoryAction action;
        editing::EditFailure failure;
    };

    class LUX_EDITOR_UI_PUBLIC LUX_OBJECT() HistoryActions final : public lux::object::Object<HistoryActions>
    {
    public:
        static const signal_type<HistoryActionFailure> failed;
        HistoryActions(lux::object::ObjectDispatcherRef, editing::EditHistoryTarget &);
        ~HistoryActions() noexcept override;
        HistoryActions(const HistoryActions &) = delete;
        HistoryActions &operator=(const HistoryActions &) = delete;
        HistoryActions(HistoryActions &&) = delete;
        HistoryActions &operator=(HistoryActions &&) = delete;
        void undo() noexcept;
        void redo() noexcept;
        [[nodiscard]] bool canUndo() const noexcept;
        [[nodiscard]] bool canRedo() const noexcept;
        [[nodiscard]] std::string_view undoLabel() const noexcept;
        [[nodiscard]] std::string_view redoLabel() const noexcept;

    private:
        [[nodiscard]] editing::EditResult<editing::HistoryTargetView> view() const noexcept;
        void invoke(editing::EHistoryAction) noexcept;
        const std::thread::id owner_{std::this_thread::get_id()};
        editing::EditHistoryTarget *const target_;
        const editing::HistoryId identity_;
        mutable bool busy_{};
    };
    class LUX_EDITOR_UI_PUBLIC LUX_OBJECT() HistoryMenuActions final : public lux::object::Object<HistoryMenuActions>
    {
    public:
        static const signal_type<HistoryActionFailure> failed;
        HistoryMenuActions(lux::object::ObjectDispatcherRef, ActiveEditHistory &);
        ~HistoryMenuActions() noexcept override;
        [[nodiscard]] editing::EditResult<void> capture() noexcept;
        [[nodiscard]] editing::EditResult<void> cancel() noexcept;
        [[nodiscard]] HistoryTargetHandle capturedTarget() const noexcept;
        void undo() noexcept;
        void redo() noexcept;
        [[nodiscard]] bool canUndo() const noexcept;
        [[nodiscard]] bool canRedo() const noexcept;

    private:
        [[nodiscard]] editing::EditResult<editing::HistoryTargetView> view() const noexcept;
        void invoke(editing::EHistoryAction) noexcept;
        const std::thread::id owner_{std::this_thread::get_id()};
        ActiveEditHistory &router_;
        HistoryTargetHandle captured_;
        editing::HistoryId identity_;
        bool busy_{};
    };
} // namespace lux::editor::ui
