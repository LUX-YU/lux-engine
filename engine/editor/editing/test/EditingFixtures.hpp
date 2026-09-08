#pragma once

#include <lux/engine/editor/editing/EditHistory.hpp>
#include <lux/engine/editor/editing/EditHistoryTarget.hpp>

#include <array>
#include <cassert>
#include <functional>
#include <limits>
#include <map>
#include <new>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace lux::editor::editing::test
{
    inline constexpr HistoryLimits kLimits{10032U, 64U * 1024U * 1024U, 16U * 1024U * 1024U, 256U};
    enum class EStage
    {
        METADATA,
        PREPARE,
        APPLY,
        PUBLISH,
        PLAN_DESTROY,
        OPERATION_DESTROY,
        NOTICE,
        TARGET
    };
    struct Statistics final
    {
        std::size_t metadata{}, prepares{}, applies{}, publishes{}, notices{}, allocations{};
        std::size_t operations{}, operations_destroyed{}, plans{}, plans_destroyed{}, target_calls{};
        std::size_t staging_used{};
    };

    class Session : public EditHistoryTarget
    {
    public:
        Statistics stats;
        std::function<void(EStage)> callback;
        std::function<void(const HistoryNotice&)> observer;
        std::size_t fail_allocation{}, allocation_index{};
        bool reject_prepare{}, empty_plan{}, force_no_change{}, blocked{}, preview{};
        std::array<char, 257> published_label{};
        std::vector<HistoryNotice> trace;
        std::unique_ptr<EditHistory> history;

        explicit Session(HistoryLimits limits = kLimits, bool saved = false)
        {
            trace.reserve(40000U);
            auto made = EditHistory::create({limits, {this, onNotice}, saved});
            assert(made);
            history = std::move(*made);
        }
        ~Session() noexcept override
        {
            assert(history->close());
        }
        void stage(EStage value) noexcept
        {
            if (callback)
            {
                callback(value);
            }
        }
        void allocate()
        {
            ++stats.allocations;
            if (++allocation_index == fail_allocation)
            {
                throw std::bad_alloc{};
            }
        }
        [[nodiscard]] EditResult<void> writable() const noexcept
        {
            const auto view = history->view();
            if (!view)
            {
                return lux::cxx::unexpected(view.error());
            }
            if (view->snapshot.closed)
            {
                return error(EEditError::CLOSED);
            }
            if (view->phase != EHistoryPhase::IDLE)
            {
                return error(EEditError::BUSY);
            }
            if (blocked)
            {
                return error(EEditError::BLOCKED_BY_HOST);
            }
            return {};
        }
        [[nodiscard]] HistoryId historyId() const noexcept override
        {
            return history->id();
        }
        [[nodiscard]] EditResult<HistoryTargetView> historyView() const noexcept override
        {
            const auto view = history->view();
            if (!view)
            {
                return lux::cxx::unexpected(view.error());
            }
            auto undo = view->can_undo ? EHistoryActionAvailability::READY : EHistoryActionAvailability::EMPTY;
            auto redo = view->can_redo ? EHistoryActionAvailability::READY : EHistoryActionAvailability::EMPTY;
            if (preview)
            {
                undo = EHistoryActionAvailability::READY;
                redo = EHistoryActionAvailability::BLOCKED;
            }
            if (blocked)
            {
                undo = redo = EHistoryActionAvailability::BLOCKED;
            }
            if (view->phase != EHistoryPhase::IDLE)
            {
                undo = redo = EHistoryActionAvailability::BUSY;
            }
            if (view->snapshot.closed)
            {
                undo = redo = EHistoryActionAvailability::CLOSED;
            }
            return HistoryTargetView{view->snapshot, undo, redo, view->undo_label, view->redo_label};
        }
        [[nodiscard]] EditResult<HistoryTargetResult> undo() noexcept override
        {
            if (auto ready = writable(); !ready)
            {
                return lux::cxx::unexpected(ready.error());
            }
            ++stats.target_calls;
            stage(EStage::TARGET);
            if (preview)
            {
                preview = false;
                const auto value = history->view()->snapshot;
                return HistoryTargetResult{
                    EHistoryTargetOutcome::TRANSIENT_CANCELLED,
                    {EEditEffect::NO_CHANGE, value.current, value.revision, value.event_sequence}
                };
            }
            const auto result = history->undo();
            if (!result)
            {
                return lux::cxx::unexpected(result.error());
            }
            return HistoryTargetResult{EHistoryTargetOutcome::CONTENT_APPLIED, *result};
        }
        [[nodiscard]] EditResult<HistoryTargetResult> redo() noexcept override
        {
            if (auto ready = writable(); !ready)
            {
                return lux::cxx::unexpected(ready.error());
            }
            if (preview)
            {
                return error(EEditError::BLOCKED_BY_HOST);
            }
            ++stats.target_calls;
            stage(EStage::TARGET);
            const auto result = history->redo();
            if (!result)
            {
                return lux::cxx::unexpected(result.error());
            }
            return HistoryTargetResult{EHistoryTargetOutcome::CONTENT_APPLIED, *result};
        }
        [[nodiscard]] static lux::cxx::unexpected<EditFailure> error(EEditError code) noexcept
        {
            return lux::cxx::unexpected(makeEditFailure(code, 17U, "fixture rejection"));
        }

    private:
        static void onNotice(void* context, const HistoryNotice& notice) noexcept
        {
            auto& session = *static_cast<Session*>(context);
            ++session.stats.notices;
            assert(session.trace.size() < session.trace.capacity());
            session.trace.push_back(notice);
            session.stage(EStage::NOTICE);
            if (session.observer)
            {
                session.observer(notice);
            }
        }
    };

    class Operation : public EditOperation
    {
    public:
        Session& session;
        HistoryId identity;
        StateId base;
        std::string title{"edit"};
        std::size_t charge{sizeof(Operation)};
        explicit Operation(Session& owner)
            : session(owner), identity(owner.historyId()), base(owner.history->view()->snapshot.current)
        {
            ++session.stats.operations;
        }
        ~Operation() noexcept override
        {
            ++session.stats.operations_destroyed;
            session.stage(EStage::OPERATION_DESTROY);
        }
        [[nodiscard]] HistoryId historyId() const noexcept override
        {
            metadata();
            return identity;
        }
        [[nodiscard]] StateId baseState() const noexcept override
        {
            metadata();
            return base;
        }
        [[nodiscard]] std::string_view label() const noexcept override
        {
            metadata();
            return title;
        }
        [[nodiscard]] std::size_t retainedBytesUpperBound() const noexcept override
        {
            metadata();
            return charge;
        }

        [[nodiscard]] virtual std::string memento() const
        {
            return {};
        }

    private:
        void metadata() const noexcept
        {
            ++session.stats.metadata;
            session.stage(EStage::METADATA);
        }
    };

    class TextSession final : public Session
    {
        class Replace;
        class Plan;
        std::string text_;

    public:
        explicit TextSession(std::string initial = "alpha", HistoryLimits limits = kLimits, bool saved = false)
            : Session(limits, saved), text_(std::move(initial))
        {
        }
        ~TextSession() noexcept override
        {
            assert(history->close());
        }
        [[nodiscard]] const std::string& text() const noexcept
        {
            return text_;
        }
        [[nodiscard]] EditOperationPtr replace(std::size_t offset, std::string old_text, std::string new_text);
        [[nodiscard]] EditResult<void> select() const noexcept
        {
            return writable();
        }
    };
    class TextSession::Plan final : public PreparedEdit
    {
        TextSession& session_;
        std::string image_;
        EEditEffect effect_;

    public:
        Plan(TextSession& session, std::string image, EEditEffect effect) noexcept
            : session_(session), image_(std::move(image)), effect_(effect)
        {
            ++session_.stats.plans;
        }
        ~Plan() noexcept override
        {
            ++session_.stats.plans_destroyed;
            session_.stage(EStage::PLAN_DESTROY);
        }
        [[nodiscard]] EEditEffect effect() const noexcept override
        {
            return effect_;
        }

    private:
        void apply() noexcept override
        {
            session_.stage(EStage::APPLY);
            static_assert(noexcept(session_.text_.swap(image_)));
            session_.text_.swap(image_);
            ++session_.stats.applies;
        }
        void publish(const CommitInfo& info) noexcept override
        {
            ++session_.stats.publishes;
            const auto size = (std::min)(info.label.size(), session_.published_label.size() - 1U);
            std::copy_n(info.label.data(), size, session_.published_label.data());
            session_.published_label[size] = '\0';
            session_.stage(EStage::PUBLISH);
        }
    };
    class TextSession::Replace final : public Operation
    {
        TextSession& model_;
        std::size_t offset_;
        const std::string old_, next_;

    public:
        [[nodiscard]] std::string memento() const override
        {
            return std::to_string(offset_) + ':' + old_ + std::string(1U, '\0') + next_;
        }
        Replace(TextSession& session, std::size_t offset, std::string old_text, std::string new_text)
            : Operation(session), model_(session), offset_(offset), old_(std::move(old_text)),
              next_(std::move(new_text))
        {
            charge = sizeof(Replace) + old_.capacity() + next_.capacity() + title.capacity() + 3U;
        }
        [[nodiscard]] EditResult<PreparedEditPtr> prepare(const ApplyContext& context, EditPreparationBudget& budget)
            const noexcept override
        {
            ++session.stats.prepares;
            session.stage(EStage::PREPARE);
            const auto& before = context.direction == EDirection::FORWARD ? old_ : next_;
            const auto& after = context.direction == EDirection::FORWARD ? next_ : old_;
            const bool is_valid_range =
                offset_ <= model_.text_.size() && before.size() <= model_.text_.size() - offset_;
            const bool is_matching_content =
                is_valid_range && model_.text_.compare(offset_, before.size(), before) == 0;
            if (!is_matching_content || session.reject_prepare)
            {
                return Session::error(EEditError::PRECONDITION_FAILED);
            }
            if (after.size() > (std::numeric_limits<std::size_t>::max)() - (model_.text_.size() - before.size()))
            {
                return Session::error(EEditError::PRECONDITION_FAILED);
            }
            const auto size = model_.text_.size() - before.size() + after.size();
            if (size > ((std::numeric_limits<std::size_t>::max)() - sizeof(Plan) - 32U) / 2U)
            {
                return Session::error(EEditError::STAGING_LIMIT);
            }
            if (auto reserved = budget.reserve(sizeof(Plan) + 2U * size + 32U); !reserved)
            {
                return lux::cxx::unexpected(reserved.error());
            }
            session.stats.staging_used = budget.used();
            if (session.empty_plan)
            {
                return PreparedEditPtr{};
            }
            try
            {
                session.allocate();
                std::string image;
                image.reserve(size);
                image.append(model_.text_, 0U, offset_);
                image.append(after);
                image.append(model_.text_, offset_ + before.size());
                session.allocate();
                const auto effect =
                    before == after || session.force_no_change ? EEditEffect::NO_CHANGE : EEditEffect::CHANGE;
                return PreparedEditPtr(new Plan(model_, std::move(image), effect));
            }
            catch (const std::bad_alloc&)
            {
                return Session::error(EEditError::ALLOCATION_FAILURE);
            }
        }
    };
    inline EditOperationPtr TextSession::replace(std::size_t offset, std::string old_text, std::string new_text)
    {
        return std::make_unique<Replace>(*this, offset, std::move(old_text), std::move(new_text));
    }

    class RecordSession final : public Session
    {
        class Patch;
        class Plan;
        std::map<int, int> records_;
        std::optional<int> selected_;

    public:
        explicit RecordSession(HistoryLimits limits = kLimits) : Session(limits)
        {
        }
        ~RecordSession() noexcept override
        {
            assert(history->close());
        }
        [[nodiscard]] const std::map<int, int>& records() const noexcept
        {
            return records_;
        }
        [[nodiscard]] std::optional<int> selected() const noexcept
        {
            return selected_;
        }
        [[nodiscard]] EditOperationPtr patch(int key, std::optional<int> before, std::optional<int> after);
        [[nodiscard]] EditResult<void> select(std::optional<int> key) noexcept
        {
            if (auto ready = writable(); !ready)
            {
                return ready;
            }
            if (key && !records_.contains(*key))
            {
                return error(EEditError::PRECONDITION_FAILED);
            }
            selected_ = key;
            return {};
        }
    };
    class RecordSession::Plan final : public PreparedEdit
    {
        RecordSession& session_;
        std::map<int, int> image_;
        std::optional<int> selection_;
        EEditEffect effect_;

    public:
        Plan(
            RecordSession& session, std::map<int, int> image, std::optional<int> selection, EEditEffect effect
        ) noexcept
            : session_(session), image_(std::move(image)), selection_(selection), effect_(effect)
        {
            ++session_.stats.plans;
        }
        ~Plan() noexcept override
        {
            ++session_.stats.plans_destroyed;
            session_.stage(EStage::PLAN_DESTROY);
        }
        [[nodiscard]] EEditEffect effect() const noexcept override
        {
            return effect_;
        }

    private:
        void apply() noexcept override
        {
            session_.stage(EStage::APPLY);
            static_assert(noexcept(session_.records_.swap(image_)));
            session_.records_.swap(image_);
            std::swap(session_.selected_, selection_);
            ++session_.stats.applies;
        }
        void publish(const CommitInfo&) noexcept override
        {
            ++session_.stats.publishes;
            session_.stage(EStage::PUBLISH);
        }
    };
    class RecordSession::Patch final : public Operation
    {
        RecordSession& model_;
        int key_;
        const std::optional<int> before_, after_, selection_before_;

    public:
        [[nodiscard]] std::string memento() const override
        {
            const auto value = [](std::optional<int> item) { return item ? std::to_string(*item) : "none"; };
            return std::to_string(key_) + ':' + value(before_) + ':' + value(after_) + ':' + value(selection_before_);
        }
        Patch(RecordSession& session, int key, std::optional<int> before, std::optional<int> after)
            : Operation(session), model_(session), key_(key), before_(before), after_(after),
              selection_before_(session.selected_)
        {
            charge = sizeof(Patch) + title.capacity() + 1U;
        }
        [[nodiscard]] EditResult<PreparedEditPtr> prepare(const ApplyContext& context, EditPreparationBudget& budget)
            const noexcept override
        {
            ++session.stats.prepares;
            session.stage(EStage::PREPARE);
            const auto before = context.direction == EDirection::FORWARD ? before_ : after_;
            const auto after = context.direction == EDirection::FORWARD ? after_ : before_;
            const auto found = model_.records_.find(key_);
            const auto live = found == model_.records_.end() ? std::nullopt : std::optional<int>(found->second);
            if (live != before || session.reject_prepare)
            {
                return Session::error(EEditError::PRECONDITION_FAILED);
            }
            const auto bytes = sizeof(Plan) + (model_.records_.size() + 1U) * 128U;
            if (auto reserved = budget.reserve(bytes); !reserved)
            {
                return lux::cxx::unexpected(reserved.error());
            }
            session.stats.staging_used = budget.used();
            if (session.empty_plan)
            {
                return PreparedEditPtr{};
            }
            try
            {
                session.allocate();
                auto image = model_.records_;
                if (after)
                {
                    image[key_] = *after;
                }
                else
                {
                    image.erase(key_);
                }
                auto selected = model_.selected_;
                if (context.direction == EDirection::BACKWARD && !selected && selection_before_)
                {
                    selected = selection_before_;
                }
                if (selected && !image.contains(*selected))
                {
                    selected.reset();
                }
                session.allocate();
                const auto effect =
                    before == after || session.force_no_change ? EEditEffect::NO_CHANGE : EEditEffect::CHANGE;
                return PreparedEditPtr(new Plan(model_, std::move(image), selected, effect));
            }
            catch (const std::bad_alloc&)
            {
                return Session::error(EEditError::ALLOCATION_FAILURE);
            }
        }
    };
    inline EditOperationPtr RecordSession::patch(int key, std::optional<int> before, std::optional<int> after)
    {
        return std::make_unique<Patch>(*this, key, before, after);
    }

    inline void execute(Session& session, EditOperationPtr operation)
    {
        const auto result = session.history->execute(operation);
        assert(result && !operation);
    }
    inline bool sameSnapshot(const HistorySnapshot& a, const HistorySnapshot& b)
    {
        return a.history == b.history && a.current == b.current && a.saved == b.saved && a.revision == b.revision &&
               a.event_sequence == b.event_sequence && a.entry_count == b.entry_count && a.cursor == b.cursor &&
               a.charged_retained_bytes == b.charged_retained_bytes &&
               a.history_metadata_bytes == b.history_metadata_bytes && a.save_pending == b.save_pending &&
               a.clean == b.clean && a.closed == b.closed;
    }
    struct CapturedEntry final
    {
        StateId before, after;
        std::string label;
        std::size_t charged{};
        bool applied{};
        bool operator==(const CapturedEntry&) const = default;
    };
    struct Snapshot final
    {
        HistorySnapshot history;
        std::vector<CapturedEntry> entries;
        explicit Snapshot(const Session& session) : history(session.history->view()->snapshot)
        {
            for (std::size_t i = 0; i < history.entry_count; ++i)
            {
                const auto entry = *session.history->entry(i);
                entries.push_back(
                    {entry.before, entry.after, std::string(entry.label), entry.charged_bytes, entry.applied}
                );
            }
        }
        bool operator==(const Snapshot& other) const
        {
            return sameSnapshot(history, other.history) && entries == other.entries;
        }
    };
} // namespace lux::editor::editing::test
