#pragma once
#include <lux/engine/editor/editing/EditOperation.hpp>
#include <lux/engine/editor/editing/visibility.h>
#include <memory>
namespace lux::editor::editing
{
    namespace detail
    {
        struct EditHistoryTestAccess;
    }
    class LUX_EDITOR_EDITING_PUBLIC EditHistory final
    {
    public:
        using CreateResult = EditResult<std::unique_ptr<EditHistory>>;
        [[nodiscard]] static CreateResult create(HistoryCreateInfo info) noexcept;
        ~EditHistory() noexcept;
        EditHistory(const EditHistory&) = delete;
        EditHistory& operator=(const EditHistory&) = delete;
        EditHistory(EditHistory&&) = delete;
        EditHistory& operator=(EditHistory&&) = delete;
        [[nodiscard]] HistoryId id() const noexcept;
        [[nodiscard]] EditResult<HistoryView> view() const noexcept;
        [[nodiscard]] EditResult<HistoryEntryView> entry(std::size_t index) const noexcept;
        [[nodiscard]] EditResult<ApplyResult> execute(EditOperationPtr& operation) noexcept;
        [[nodiscard]] EditResult<ApplyResult> undo() noexcept;
        [[nodiscard]] EditResult<ApplyResult> redo() noexcept;
        [[nodiscard]] EditResult<SaveTicket> beginSave() noexcept;
        [[nodiscard]] EditResult<void> finishSave(SaveTicket ticket, ESaveOutcome outcome) noexcept;
        [[nodiscard]] EditResult<void> clear() noexcept;
        [[nodiscard]] EditResult<void> close() noexcept;

    private:
        friend struct detail::EditHistoryTestAccess;
        [[nodiscard]] EditResult<ApplyResult> replay(EApplyKind kind) noexcept;
        struct Impl;
        explicit EditHistory(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::editing
