#include <lux/engine/editor/editing/EditHistory.hpp>
#include <lux/engine/editor/editing/detail/EditDiagnostics.hpp>

#include <atomic>
#include <limits>
#include <new>
#include <thread>
#include <utility>
#include <vector>

namespace lux::editor::editing
{
    namespace
    {
        constexpr auto kMaxCounter = (std::numeric_limits<std::uint64_t>::max)();
        constexpr auto kMaxSize = (std::numeric_limits<std::size_t>::max)();
        std::atomic<std::uint64_t> history_identity{0U};

        [[nodiscard]] auto failure(EEditError code) noexcept
        {
            return lux::cxx::unexpected(makeEditFailure(code));
        }

        struct Entry final
        {
            EditOperationPtr operation;
            StateId before, after;
            detail::EditLabel label;
            std::size_t charged{};
        };

        struct PhaseGuard final
        {
            EHistoryPhase& phase;
            explicit PhaseGuard(EHistoryPhase& value) noexcept : phase(value)
            {
                phase = EHistoryPhase::PREPARING;
            }
            ~PhaseGuard() noexcept
            {
                if (phase != EHistoryPhase::CLOSED)
                {
                    phase = EHistoryPhase::IDLE;
                }
            }
        };
    } // namespace

    struct EditHistory::Impl final
    {
        using Entries = std::vector<Entry, detail::EditAllocator<Entry, detail::EEditAllocationSite::ENTRIES>>;
        using Retired = std::vector<Entry, detail::EditAllocator<Entry, detail::EEditAllocationSite::RECLAIM>>;
        const std::thread::id owner{std::this_thread::get_id()};
        HistoryCreateInfo info;
        HistoryId identity;
        StateId base, current;
        std::optional<StateId> saved;
        std::optional<SaveTicket> pending;
        Revision revision;
        std::uint64_t serial{1U}, event{}, request{};
        EHistoryPhase phase{EHistoryPhase::IDLE};
        Entries entries;
        Retired retired;
        std::size_t cursor{}, retained_bytes{}, applied_bytes{};

#if defined(LUX_EDITOR_EDITING_TEST_DIAGNOSTICS)
        Impl() noexcept
        {
            ++detail::allocationStatistics().live_objects;
        }
        ~Impl() noexcept
        {
            --detail::allocationStatistics().live_objects;
        }
#endif

        [[nodiscard]] EditResult<void> check(bool query = false) const noexcept
        {
            if (owner != std::this_thread::get_id())
            {
                return failure(EEditError::WRONG_THREAD);
            }
            if (phase == EHistoryPhase::CLOSED)
            {
                return failure(EEditError::CLOSED);
            }
            if (!query && phase != EHistoryPhase::IDLE)
            {
                return failure(EEditError::BUSY);
            }
            return {};
        }

        [[nodiscard]] auto reject(EEditError code) noexcept
        {
            phase = EHistoryPhase::RECLAIMING;
            return failure(code);
        }
        [[nodiscard]] auto reject(EditFailure error) noexcept
        {
            phase = EHistoryPhase::RECLAIMING;
            return lux::cxx::unexpected(error);
        }
        [[nodiscard]] HistorySnapshot snapshot() const noexcept
        {
            return HistorySnapshot{
                identity,
                current,
                saved,
                revision,
                event,
                entries.size(),
                cursor,
                retained_bytes,
                (entries.capacity() + retired.capacity()) * sizeof(Entry),
                pending.has_value(),
                saved.has_value() && *saved == current,
                phase == EHistoryPhase::CLOSED
            };
        }
        void notice(EHistoryEvent kind) const noexcept
        {
            if (info.observer.changed)
            {
                const HistoryNotice value{kind, snapshot()};
                info.observer.changed(info.observer.context, value);
            }
        }
        void collect() noexcept
        {
            // Entries are appended in descending original index order.
            for (auto& entry : retired)
            {
                entry.operation.reset();
            }
            retired.clear();
        }
        void retireAll() noexcept
        {
            for (auto index = entries.size(); index != 0U; --index)
            {
                retired.push_back(std::move(entries[index - 1U]));
            }
            entries.clear();
            cursor = retained_bytes = applied_bytes = 0U;
            base = current;
        }
    };

#if defined(LUX_EDITOR_EDITING_TEST_DIAGNOSTICS)
    namespace detail
    {
        EditAllocationStatistics& allocationStatistics() noexcept
        {
            static thread_local EditAllocationStatistics statistics;
            return statistics;
        }
        EditDiagnostics& editDiagnostics() noexcept
        {
            static thread_local EditDiagnostics diagnostics;
            return diagnostics;
        }
        void EditHistoryTestAccess::counters(
            EditHistory& history,
            std::uint64_t serial,
            std::uint64_t revision,
            std::uint64_t event,
            std::uint64_t request
        ) noexcept
        {
            auto& state = *history.impl_;
            if (!state.check())
            {
                failEditContract(EEditContract::HISTORY_LIFETIME);
            }
            state.serial = serial;
            state.revision.value = revision;
            state.event = event;
            state.request = request;
        }
        void EditHistoryTestAccess::identityCounter(std::uint64_t value) noexcept
        {
            history_identity.store(value, std::memory_order_relaxed);
        }
    } // namespace detail
#endif

    EditHistory::EditHistory(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl))
    {
#if defined(LUX_EDITOR_EDITING_TEST_DIAGNOSTICS)
        ++detail::allocationStatistics().live_objects;
#endif
    }

    EditHistory::CreateResult EditHistory::create(HistoryCreateInfo info) noexcept
    {
        const auto limits = info.limits;
        const bool is_zero_limit = limits.max_entries == 0U || limits.max_retained_bytes == 0U ||
                                   limits.max_staging_bytes == 0U || limits.max_label_bytes == 0U;
        const bool is_storage_overflow = limits.max_entries > (kMaxSize / sizeof(Entry) - 1U) / 2U;
        if (is_zero_limit || is_storage_overflow)
        {
            return failure(EEditError::INVALID_LIMITS);
        }
        const bool is_invalid_observer = info.observer.changed == nullptr && info.observer.context != nullptr;
        if (is_invalid_observer)
        {
            return failure(EEditError::INVALID_ARGUMENT);
        }
        {
            detail::allocationCheckpoint(detail::EEditAllocationSite::FACTORY, sizeof(Impl));
            auto state = std::make_unique<Impl>();
            state->info = info;
            state->entries.reserve(limits.max_entries);
            state->retired.reserve(limits.max_entries + 1U);
            detail::allocationCheckpoint(detail::EEditAllocationSite::FACTORY, sizeof(EditHistory));
            auto result = std::unique_ptr<EditHistory>(new EditHistory(std::move(state)));
            auto issued = history_identity.load(std::memory_order_relaxed);
            do
            {
                if (issued == kMaxCounter)
                {
                    return failure(EEditError::ID_EXHAUSTED);
                }
            } while (!history_identity.compare_exchange_weak(issued, issued + 1U, std::memory_order_relaxed));
            auto& ready = *result->impl_;
            ready.identity = HistoryId{issued + 1U};
            ready.current = ready.base = StateId{ready.identity, 1U};
            if (info.initially_saved)
            {
                ready.saved = ready.current;
            }
            return result;
        }
    }

    EditHistory::~EditHistory() noexcept
    {
        auto& state = *impl_;
        const bool is_wrong_owner = state.owner != std::this_thread::get_id();
        const bool is_busy = state.phase != EHistoryPhase::IDLE && state.phase != EHistoryPhase::CLOSED;
        if (is_wrong_owner || is_busy)
        {
            detail::failEditContract(detail::EEditContract::HISTORY_LIFETIME);
        }
        state.phase = EHistoryPhase::CLOSED;
        state.pending.reset();
        state.retireAll();
        state.collect();
#if defined(LUX_EDITOR_EDITING_TEST_DIAGNOSTICS)
        --detail::allocationStatistics().live_objects;
#endif
    }

    HistoryId EditHistory::id() const noexcept
    {
        return impl_->identity;
    }

    EditResult<HistoryView> EditHistory::view() const noexcept
    {
        const auto& state = *impl_;
        if (state.owner != std::this_thread::get_id())
        {
            return failure(EEditError::WRONG_THREAD);
        }
        const bool is_idle = state.phase == EHistoryPhase::IDLE;
        const auto undo =
            state.cursor != 0U ? std::string_view(state.entries[state.cursor - 1U].label) : std::string_view{};
        const auto redo = state.cursor < state.entries.size() ? std::string_view(state.entries[state.cursor].label)
                                                              : std::string_view{};
        return HistoryView{
            state.snapshot(), state.phase, undo, redo, is_idle && !undo.empty(), is_idle && !redo.empty()
        };
    }

    EditResult<HistoryEntryView> EditHistory::entry(std::size_t index) const noexcept
    {
        const auto& state = *impl_;
        if (auto checked = state.check(true); !checked)
        {
            return lux::cxx::unexpected(checked.error());
        }
        if (index >= state.entries.size())
        {
            return failure(EEditError::INVALID_ARGUMENT);
        }
        const auto& entry = state.entries[index];
        return HistoryEntryView{index, entry.before, entry.after, entry.label, entry.charged, index < state.cursor};
    }

    EditResult<ApplyResult> EditHistory::execute(EditOperationPtr& operation) noexcept
    {
        auto& state = *impl_;
        if (auto checked = state.check(); !checked)
        {
            return lux::cxx::unexpected(checked.error());
        }
        PhaseGuard guard(state.phase);
        if (!operation)
        {
            return state.reject(EEditError::INVALID_ARGUMENT);
        }
        if (operation->historyId() != state.identity)
        {
            return state.reject(EEditError::WRONG_HISTORY);
        }
        if (operation->baseState() != state.current)
        {
            return state.reject(EEditError::STALE_BASE);
        }
        const auto label = operation->label();
        const bool is_invalid_label = label.empty() || label.find('\0') != std::string_view::npos;
        if (is_invalid_label)
        {
            return state.reject(EEditError::INVALID_ARGUMENT);
        }
        const auto payload = operation->retainedBytesUpperBound();
        const auto limits = state.info.limits;
        const bool is_over_limit = label.size() > limits.max_label_bytes || payload > limits.max_retained_bytes;
        if (is_over_limit)
        {
            return state.reject(EEditError::HISTORY_LIMIT);
        }
        const bool is_exhausted =
            state.serial == kMaxCounter || state.revision.value == kMaxCounter || state.event == kMaxCounter;
        if (is_exhausted)
        {
            return state.reject(EEditError::ID_EXHAUSTED);
        }
        Entry incoming;
        // Prepare the owned title before business apply or publication. Allocation exhaustion is fatal.
        {
            incoming.label.assign(label.data(), label.size());
        }
        const auto label_charge = incoming.label.capacity() + 1U;
        const bool is_charge_overflow =
            label_charge > limits.max_retained_bytes || payload > limits.max_retained_bytes - label_charge;
        if (is_charge_overflow)
        {
            return state.reject(EEditError::HISTORY_LIMIT);
        }
        incoming.charged = payload + label_charge;
        std::size_t prune{};
        auto remaining = state.applied_bytes;
        while (state.cursor - prune >= limits.max_entries || remaining > limits.max_retained_bytes - incoming.charged)
        {
            remaining -= state.entries[prune++].charged;
        }
        incoming.before = state.current;
        incoming.after = StateId{state.identity, state.serial + 1U};
        const ApplyContext context{
            state.identity,
            EApplyKind::EXECUTE,
            EDirection::FORWARD,
            incoming.before,
            incoming.after,
            Revision{state.revision.value + 1U}
        };
        EditPreparationBudget budget(limits.max_staging_bytes);
        auto prepared = operation->prepare(context, budget);
        if (!prepared)
        {
            return state.reject(prepared.error());
        }
        auto plan = std::move(*prepared);
        if (!plan)
        {
            return state.reject(EEditError::CONTRACT_VIOLATION);
        }
        const auto effect = plan->effect();
        if (effect == EEditEffect::NO_CHANGE)
        {
            state.phase = EHistoryPhase::RECLAIMING;
            plan.reset();
            operation.reset();
            return ApplyResult{effect, state.current, state.revision, state.event};
        }
        if (effect != EEditEffect::CHANGE)
        {
            return state.reject(EEditError::CONTRACT_VIOLATION);
        }
        state.phase = EHistoryPhase::COMMITTING;
        plan->apply();
        incoming.operation = std::move(operation);
        for (auto index = state.entries.size(); index != state.cursor; --index)
        {
            state.retired.push_back(std::move(state.entries[index - 1U]));
        }
        for (auto index = prune; index != 0U; --index)
        {
            state.retired.push_back(std::move(state.entries[index - 1U]));
        }
        if (prune != 0U)
        {
            for (auto index = prune; index < state.cursor; ++index)
            {
                state.entries[index - prune] = std::move(state.entries[index]);
            }
        }
        state.entries.resize(state.cursor - prune);
        state.entries.push_back(std::move(incoming));
        state.cursor = state.entries.size();
        state.current = context.to;
        state.base = state.entries.front().before;
        state.serial = context.to.serial;
        state.revision = context.next_revision;
        ++state.event;
        state.retained_bytes = state.applied_bytes = remaining + state.entries.back().charged;
        const CommitInfo commit{
            context.kind, context.from, context.to, state.revision, state.event, state.entries.back().label
        };
        state.phase = EHistoryPhase::PUBLISHING;
        plan->publish(commit);
        state.notice(EHistoryEvent::EXECUTED);
        state.phase = EHistoryPhase::RECLAIMING;
        plan.reset();
        state.collect();
        return ApplyResult{effect, state.current, state.revision, state.event};
    }

    EditResult<ApplyResult> EditHistory::undo() noexcept
    {
        return replay(EApplyKind::UNDO);
    }
    EditResult<ApplyResult> EditHistory::redo() noexcept
    {
        return replay(EApplyKind::REDO);
    }

    EditResult<ApplyResult> EditHistory::replay(EApplyKind kind) noexcept
    {
        auto& state = *impl_;
        if (auto checked = state.check(); !checked)
        {
            return lux::cxx::unexpected(checked.error());
        }
        const bool backward = kind == EApplyKind::UNDO;
        const bool is_empty = backward ? state.cursor == 0U : state.cursor == state.entries.size();
        if (is_empty)
        {
            return failure(backward ? EEditError::NO_UNDO : EEditError::NO_REDO);
        }
        PhaseGuard guard(state.phase);
        auto& entry = state.entries[backward ? state.cursor - 1U : state.cursor];
        const auto expected = backward ? entry.after : entry.before;
        if (state.current != expected)
        {
            return state.reject(EEditError::CONTRACT_VIOLATION);
        }
        const bool is_exhausted = state.revision.value == kMaxCounter || state.event == kMaxCounter;
        if (is_exhausted)
        {
            return state.reject(EEditError::ID_EXHAUSTED);
        }
        const ApplyContext context{
            state.identity,
            kind,
            backward ? EDirection::BACKWARD : EDirection::FORWARD,
            state.current,
            backward ? entry.before : entry.after,
            Revision{state.revision.value + 1U}
        };
        EditPreparationBudget budget(state.info.limits.max_staging_bytes);
        auto prepared = entry.operation->prepare(context, budget);
        if (!prepared)
        {
            return state.reject(prepared.error());
        }
        auto plan = std::move(*prepared);
        const bool is_invalid_plan = !plan || plan->effect() != EEditEffect::CHANGE;
        if (is_invalid_plan)
        {
            return state.reject(EEditError::CONTRACT_VIOLATION);
        }
        state.phase = EHistoryPhase::COMMITTING;
        plan->apply();
        if (backward)
        {
            --state.cursor;
            state.applied_bytes -= entry.charged;
        }
        else
        {
            ++state.cursor;
            state.applied_bytes += entry.charged;
        }
        state.current = context.to;
        state.revision = context.next_revision;
        ++state.event;
        const CommitInfo commit{kind, context.from, context.to, state.revision, state.event, entry.label};
        state.phase = EHistoryPhase::PUBLISHING;
        plan->publish(commit);
        state.notice(backward ? EHistoryEvent::UNDONE : EHistoryEvent::REDONE);
        state.phase = EHistoryPhase::RECLAIMING;
        plan.reset();
        return ApplyResult{EEditEffect::CHANGE, state.current, state.revision, state.event};
    }

    EditResult<SaveTicket> EditHistory::beginSave() noexcept
    {
        auto& state = *impl_;
        if (auto checked = state.check(); !checked)
        {
            return lux::cxx::unexpected(checked.error());
        }
        if (state.pending)
        {
            return failure(EEditError::SAVE_IN_PROGRESS);
        }
        const bool is_exhausted = state.event == kMaxCounter || state.request == kMaxCounter;
        if (is_exhausted)
        {
            return failure(EEditError::ID_EXHAUSTED);
        }
        PhaseGuard guard(state.phase);
        const SaveTicket ticket{state.identity, state.current, ++state.request};
        state.pending = ticket;
        ++state.event;
        state.phase = EHistoryPhase::PUBLISHING;
        state.notice(EHistoryEvent::SAVE_STARTED);
        return ticket;
    }

    EditResult<void> EditHistory::finishSave(SaveTicket ticket, ESaveOutcome outcome) noexcept
    {
        auto& state = *impl_;
        if (auto checked = state.check(); !checked)
        {
            return checked;
        }
        const bool is_matching_ticket = ticket.valid() && state.pending && ticket.history() == state.identity &&
                                        ticket.request() == state.pending->request() &&
                                        ticket.state() == state.pending->state();
        if (!is_matching_ticket)
        {
            return failure(EEditError::STALE_SAVE);
        }
        const bool is_invalid_outcome =
            outcome != ESaveOutcome::SUCCEEDED && outcome != ESaveOutcome::FAILED && outcome != ESaveOutcome::CANCELLED;
        if (is_invalid_outcome)
        {
            return failure(EEditError::INVALID_ARGUMENT);
        }
        if (state.event == kMaxCounter)
        {
            return failure(EEditError::ID_EXHAUSTED);
        }
        PhaseGuard guard(state.phase);
        if (outcome == ESaveOutcome::SUCCEEDED)
        {
            state.saved = ticket.state();
        }
        state.pending.reset();
        ++state.event;
        state.phase = EHistoryPhase::PUBLISHING;
        state.notice(EHistoryEvent::SAVE_FINISHED);
        return {};
    }

    EditResult<void> EditHistory::clear() noexcept
    {
        auto& state = *impl_;
        if (auto checked = state.check(); !checked)
        {
            return checked;
        }
        if (state.entries.empty())
        {
            return {};
        }
        if (state.event == kMaxCounter)
        {
            return failure(EEditError::ID_EXHAUSTED);
        }
        PhaseGuard guard(state.phase);
        state.retireAll();
        ++state.event;
        state.phase = EHistoryPhase::PUBLISHING;
        state.notice(EHistoryEvent::CLEARED);
        state.phase = EHistoryPhase::RECLAIMING;
        state.collect();
        return {};
    }

    EditResult<void> EditHistory::close() noexcept
    {
        auto& state = *impl_;
        if (auto checked = state.check(); !checked)
        {
            return checked.error().code == EEditError::CLOSED ? EditResult<void>{} : checked;
        }
        state.phase = EHistoryPhase::RECLAIMING;
        state.pending.reset();
        state.retireAll();
        if (state.event != kMaxCounter)
        {
            ++state.event;
        }
        state.phase = EHistoryPhase::CLOSED;
        state.notice(EHistoryEvent::CLOSED);
        state.collect();
        return {};
    }
} // namespace lux::editor::editing
