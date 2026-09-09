#include <lux/engine/editor/ui/actions/HistoryActions.hpp>
#include <utility>

namespace lux::editor::ui
{
    namespace
    {
        struct ActionGate final
        {
            bool &busy;
            explicit ActionGate(bool &value) noexcept : busy(value)
            {
                busy = true;
            }
            ~ActionGate() noexcept
            {
                busy = false;
            }
        };
    } // namespace

    HistoryActions::HistoryActions(lux::object::ObjectDispatcherRef dispatcher, editing::EditHistoryTarget &target)
        : Object(std::move(dispatcher)), target_(&target), identity_(target.historyId())
    {
    }
    HistoryActions::~HistoryActions() noexcept = default;

    editing::EditResult<editing::HistoryTargetView> HistoryActions::view() const noexcept
    {
        using namespace editing;
        if (owner_ != std::this_thread::get_id())
            return lux::cxx::unexpected(makeEditFailure(EEditError::WRONG_THREAD));
        if (busy_)
            return lux::cxx::unexpected(makeEditFailure(EEditError::BUSY));
        ActionGate gate{busy_};
        if (target_->historyId() != identity_)
            return lux::cxx::unexpected(makeEditFailure(EEditError::CONTRACT_VIOLATION));
        auto result = target_->historyView();
        if (result && result->history.history != identity_)
            return lux::cxx::unexpected(makeEditFailure(EEditError::CONTRACT_VIOLATION));
        return result;
    }

    void HistoryActions::invoke(editing::EHistoryAction action) noexcept
    {
        using namespace editing;
        // Object signals cannot be delivered from an unowned thread. No target virtual is called there.
        if (owner_ != std::this_thread::get_id() || busy_)
            return;
        const auto current = view();
        ActionGate gate{busy_};
        const auto failure = [&](EditFailure error) { notify<failed>(HistoryActionFailure{identity_, action, error}); };
        if (!current)
        {
            failure(current.error());
            return;
        }
        const auto availability = action == EHistoryAction::UNDO ? current->undo : current->redo;
        if (availability != EHistoryActionAvailability::READY)
        {
            const auto code = availability == EHistoryActionAvailability::BUSY      ? EEditError::BUSY
                              : availability == EHistoryActionAvailability::BLOCKED ? EEditError::BLOCKED_BY_HOST
                              : availability == EHistoryActionAvailability::CLOSED  ? EEditError::CLOSED
                              : action == EHistoryAction::UNDO                      ? EEditError::NO_UNDO
                                                                                    : EEditError::NO_REDO;
            failure(makeEditFailure(code));
            return;
        }
        const auto result = action == EHistoryAction::UNDO ? target_->undo() : target_->redo();
        if (!result)
            failure(result.error());
        else if (result->content.current.history != identity_)
            failure(makeEditFailure(EEditError::CONTRACT_VIOLATION));
    }

    void HistoryActions::undo() noexcept
    {
        invoke(editing::EHistoryAction::UNDO);
    }
    void HistoryActions::redo() noexcept
    {
        invoke(editing::EHistoryAction::REDO);
    }
    bool HistoryActions::canUndo() const noexcept
    {
        const auto result = view();
        return result && result->undo == editing::EHistoryActionAvailability::READY;
    }
    bool HistoryActions::canRedo() const noexcept
    {
        const auto result = view();
        return result && result->redo == editing::EHistoryActionAvailability::READY;
    }
    std::string_view HistoryActions::undoLabel() const noexcept
    {
        const auto result = view();
        return result ? result->undo_label : std::string_view{};
    }
    std::string_view HistoryActions::redoLabel() const noexcept
    {
        const auto result = view();
        return result ? result->redo_label : std::string_view{};
    }
    HistoryMenuActions::HistoryMenuActions(lux::object::ObjectDispatcherRef dispatcher, ActiveEditHistory &router)
        : Object(std::move(dispatcher)), router_(router)
    {
    }
    HistoryMenuActions::~HistoryMenuActions() noexcept = default;
    editing::EditResult<void> HistoryMenuActions::capture() noexcept
    {
        using namespace editing;
        if (owner_ != std::this_thread::get_id())
            return lux::cxx::unexpected(makeEditFailure(EEditError::WRONG_THREAD));
        if (busy_)
            return lux::cxx::unexpected(makeEditFailure(EEditError::BUSY));
        ActionGate gate{busy_};
        const auto current = router_.view();
        if (!current)
            return lux::cxx::unexpected(current.error());
        // Opening an empty menu must not preserve a previous document's token.
        captured_ = current->handle;
        identity_ = current->has_target ? current->target.history.history : HistoryId{};
        return {};
    }
    editing::EditResult<void> HistoryMenuActions::cancel() noexcept
    {
        using namespace editing;
        if (owner_ != std::this_thread::get_id())
            return lux::cxx::unexpected(makeEditFailure(EEditError::WRONG_THREAD));
        if (busy_)
            return lux::cxx::unexpected(makeEditFailure(EEditError::BUSY));
        captured_ = {};
        identity_ = {};
        return {};
    }
    HistoryTargetHandle HistoryMenuActions::capturedTarget() const noexcept
    {
        return owner_ == std::this_thread::get_id() ? captured_ : HistoryTargetHandle{};
    }
    editing::EditResult<editing::HistoryTargetView> HistoryMenuActions::view() const noexcept
    {
        using namespace editing;
        if (owner_ != std::this_thread::get_id())
            return lux::cxx::unexpected(makeEditFailure(EEditError::WRONG_THREAD));
        if (busy_)
            return lux::cxx::unexpected(makeEditFailure(EEditError::BUSY));
        if (!captured_.valid())
            return lux::cxx::unexpected(makeEditFailure(EEditError::NO_ACTIVE_TARGET));
        return router_.view(captured_);
    }
    void HistoryMenuActions::invoke(editing::EHistoryAction action) noexcept
    {
        using namespace editing;
        if (owner_ != std::this_thread::get_id() || busy_)
            return;
        ActionGate gate{busy_};
        const auto result = action == EHistoryAction::UNDO ? router_.undo(captured_) : router_.redo(captured_);
        if (!result)
            notify<failed>(HistoryActionFailure{identity_, action, result.error()});
    }
    void HistoryMenuActions::undo() noexcept
    {
        invoke(editing::EHistoryAction::UNDO);
    }
    void HistoryMenuActions::redo() noexcept
    {
        invoke(editing::EHistoryAction::REDO);
    }
    bool HistoryMenuActions::canUndo() const noexcept
    {
        const auto current = view();
        return current && current->undo == editing::EHistoryActionAvailability::READY;
    }
    bool HistoryMenuActions::canRedo() const noexcept
    {
        const auto current = view();
        return current && current->redo == editing::EHistoryActionAvailability::READY;
    }
} // namespace lux::editor::ui
