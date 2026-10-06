#pragma once
#include <lux/engine/editor/editing/EditTypes.hpp>
#include <lux/engine/editor/editing/history_visibility.h>
#include <memory>
namespace lux::editor::editing
{
    // Owner-thread history. Queries borrow labels until mutation; snapshots own only scalar state.
    // EditExecutor is the sole mutation entry. The log owns its gate, including reclamation.
    class LUX_EDIT_HISTORY_PUBLIC EditHistory final
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
    private:
        friend class EditExecutor;
        struct Impl;
        explicit EditHistory(std::unique_ptr<Impl> impl) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::editor::editing
