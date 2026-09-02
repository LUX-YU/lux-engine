#include <lux/engine/editor/inspector/InspectorUndoJournal.hpp>

#include <new>
#include <utility>

namespace lux::editor::inspector
{
    InspectorUndoJournal::~InspectorUndoJournal() = default;

    bool InspectorUndoJournal::cancel(EditorContext& context)
    {
        if (active_ == nullptr)
            return false;
        const bool restored = active_->apply(context, false);
        active_.reset();
        poisoned_ |= !restored;
        return restored;
    }

    bool InspectorUndoJournal::transfer(
        EditorContext& context,
        std::vector<std::unique_ptr<OperationBase>>& from,
        std::vector<std::unique_ptr<OperationBase>>& to,
        bool use_after
    )
    {
        if (poisoned_ || active_ != nullptr || from.empty())
            return false;
        try
        {
            to.reserve(to.size() + 1U);
        }
        catch (const std::bad_alloc&)
        {
            return false;
        }
        if (!from.back()->apply(context, use_after))
        {
            poisoned_ = true;
            return false;
        }
        to.push_back(std::move(from.back()));
        from.pop_back();
        return true;
    }

    bool InspectorUndoJournal::undo(EditorContext& context)
    {
        return transfer(context, undo_, redo_, false);
    }

    bool InspectorUndoJournal::redo(EditorContext& context)
    {
        return transfer(context, redo_, undo_, true);
    }

    void InspectorUndoJournal::clear() noexcept
    {
        undo_.clear();
        redo_.clear();
        active_.reset();
        poisoned_ = false;
    }

    bool InspectorUndoJournal::hasActiveGesture() const noexcept { return active_ != nullptr; }
    bool InspectorUndoJournal::canUndo() const noexcept { return !poisoned_ && !undo_.empty(); }
    bool InspectorUndoJournal::canRedo() const noexcept { return !poisoned_ && !redo_.empty(); }
    bool InspectorUndoJournal::poisoned() const noexcept { return poisoned_; }
    std::size_t InspectorUndoJournal::undoDepth() const noexcept { return undo_.size(); }
} // namespace lux::editor::inspector
