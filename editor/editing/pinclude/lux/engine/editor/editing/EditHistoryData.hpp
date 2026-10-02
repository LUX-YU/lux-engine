#pragma once
#include <lux/engine/editor/editing/EditHistory.hpp>
#include <lux/engine/editor/editing/EditOperation.hpp>
#include <string>
#include <thread>
#include <vector>

namespace lux::editor::editing
{
    struct HistoryEntry final
    {
        EditOperationPtr operation;
        StateId before, after;
        std::string label;
        std::size_t charged{};
    };

    struct EditHistory::Impl final
    {
        using Entries = std::vector<HistoryEntry>;
        using Retired = std::vector<HistoryEntry>;
        const std::thread::id owner{std::this_thread::get_id()};
        HistoryCreateInfo info;
        HistoryId identity;
        StateId base, current;
        Revision revision;
        std::uint64_t serial{1U}, event{};
        EHistoryPhase phase{EHistoryPhase::IDLE};
        Entries entries;
        Retired retired;
        std::size_t cursor{}, retained_bytes{}, applied_bytes{};

        [[nodiscard]] EditResult<void> check(bool query = false) const noexcept
        {
            if (owner != std::this_thread::get_id())
            {
                return lux::cxx::unexpected(makeEditFailure(EEditError::WRONG_THREAD));
            }
            if (phase == EHistoryPhase::CLOSED)
            {
                return lux::cxx::unexpected(makeEditFailure(EEditError::CLOSED));
            }
            if (!query && phase != EHistoryPhase::IDLE)
            {
                return lux::cxx::unexpected(makeEditFailure(EEditError::BUSY));
            }
            return {};
        }

        [[nodiscard]] auto reject(EEditError code) noexcept
        {
            phase = EHistoryPhase::RECLAIMING;
            return lux::cxx::unexpected(makeEditFailure(code));
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
                revision,
                event,
                entries.size(),
                cursor,
                retained_bytes,
                (entries.capacity() + retired.capacity()) * sizeof(HistoryEntry),
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

}
