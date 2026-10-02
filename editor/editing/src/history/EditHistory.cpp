#include <lux/engine/editor/editing/EditHistoryData.hpp>
#include <atomic>
#include <cstdlib>
#include <limits>
#include <utility>

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
    }

    EditHistory::EditHistory(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    EditHistory::CreateResult EditHistory::create(HistoryCreateInfo info) noexcept
    {
        const auto limits = info.limits;
        const bool is_zero_limit = limits.max_entries == 0U || limits.max_retained_bytes == 0U ||
                                   limits.max_staging_bytes == 0U || limits.max_label_bytes == 0U;
        const bool is_storage_overflow = limits.max_entries > (kMaxSize / sizeof(HistoryEntry) - 1U) / 2U;
        if (is_zero_limit || is_storage_overflow)
        {
            return failure(EEditError::INVALID_LIMITS);
        }
        const bool is_invalid_observer = info.observer.changed == nullptr && info.observer.context != nullptr;
        if (is_invalid_observer)
        {
            return failure(EEditError::INVALID_ARGUMENT);
        }
        auto state = std::make_unique<Impl>();
        state->info = info;
        state->entries.reserve(limits.max_entries);
        state->retired.reserve(limits.max_entries + 1U);

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
        return result;
    }

    EditHistory::~EditHistory() noexcept
    {
        auto& state = *impl_;
        const bool is_wrong_owner = state.owner != std::this_thread::get_id();
        const bool is_busy = state.phase != EHistoryPhase::IDLE && state.phase != EHistoryPhase::CLOSED;
        if (is_wrong_owner || is_busy)
        {
            std::abort();
        }
        state.phase = EHistoryPhase::CLOSED;
        state.retireAll();
        state.collect();
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
            state.snapshot(),
            state.phase,
            undo,
            redo,
            is_idle && !undo.empty(),
            is_idle && !redo.empty()
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

} // namespace lux::editor::editing
