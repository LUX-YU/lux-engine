#pragma once
#include <lux/engine/editor/editing/EditTypes.hpp>
#include <lux/engine/editor/editing/visibility.h>
#include <memory>
namespace lux::editor::editing
{
    // Business-owned staged image. Preparation must not mutate live content, selection or memento.
    class LUX_EDITOR_EDITING_PUBLIC PreparedEdit
    {
    public:
        virtual ~PreparedEdit() noexcept;
        PreparedEdit(const PreparedEdit&) = delete;
        PreparedEdit& operator=(const PreparedEdit&) = delete;
        PreparedEdit(PreparedEdit&&) = delete;
        PreparedEdit& operator=(PreparedEdit&&) = delete;
        [[nodiscard]] virtual EEditEffect effect() const noexcept = 0;

    protected:
        PreparedEdit() noexcept = default;

    private:
        friend class EditHistory;
        // Only prepared noexcept swaps/scalars: no allocations, callbacks, I/O or fallible model APIs.
        virtual void apply() noexcept = 0;
        // Called after both content and history commit. Borrowed labels cannot be queued.
        virtual void publish(const CommitInfo&) noexcept = 0;
    };
    using PreparedEditPtr = std::unique_ptr<PreparedEdit>;
    class LUX_EDITOR_EDITING_PUBLIC EditOperation
    {
    public:
        virtual ~EditOperation() noexcept;
        EditOperation(const EditOperation&) = delete;
        EditOperation& operator=(const EditOperation&) = delete;
        EditOperation(EditOperation&&) = delete;
        EditOperation& operator=(EditOperation&&) = delete;
        [[nodiscard]] virtual HistoryId historyId() const noexcept = 0;
        [[nodiscard]] virtual StateId baseState() const noexcept = 0;
        [[nodiscard]] virtual std::string_view label() const noexcept = 0;
        [[nodiscard]] virtual std::size_t retainedBytesUpperBound() const noexcept = 0;
        [[nodiscard]] virtual EditResult<PreparedEditPtr> prepare(
            const ApplyContext& context, EditPreparationBudget& budget
        ) const noexcept = 0;

    protected:
        EditOperation() noexcept = default;
    };
    using EditOperationPtr = std::unique_ptr<EditOperation>;
} // namespace lux::editor::editing
