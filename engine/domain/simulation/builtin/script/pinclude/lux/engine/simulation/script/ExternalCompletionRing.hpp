#pragma once

#include <lux/engine/simulation/scripting/ScriptRuntime.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>

namespace lux::simulation::script::detail
{
    enum class EExternalCompletionTicketState : std::uint32_t
    {
        CLOSED,
        ACTIVE,
        CLAIMED,
    };

    struct ExternalCompletionRecord final
    {
        ScriptInstanceId instance;
        ScriptAwaitableId awaitable;
        EScriptAwaitableState state{EScriptAwaitableState::CANCELLED};
        ScriptStepError error;
        lux::semantic::TypeId type{lux::semantic::InvalidTypeId};
        std::uint32_t size{};
        alignas(std::max_align_t) std::array<std::byte, ScriptOwnedBytes::InlineCapacity> bytes{};
    };

    struct ExternalCompletionRing final
    {
        struct Cell final
        {
            std::atomic<std::size_t> sequence{};
            ExternalCompletionRecord record;
        };

        struct Ticket final
        {
            std::atomic<std::uint64_t> state{};
            std::atomic<lux::semantic::TypeId> type{lux::semantic::InvalidTypeId};
            std::atomic<std::uint32_t> size{};
        };

        std::unique_ptr<Cell[]> cells;
        std::unique_ptr<Ticket[]> tickets;
        std::size_t capacity{};
        std::size_t ticket_capacity{};
        std::atomic<std::size_t> enqueue_position{};
        std::size_t dequeue_position{};
        std::atomic<std::size_t> count{};
        std::atomic<std::size_t> high_water{};
        std::atomic<std::size_t> capacity_failures{};
        std::atomic<bool> closed{};

        [[nodiscard]] static constexpr std::uint64_t ticketState(
            ScriptAwaitableId awaitable,
            EExternalCompletionTicketState state
        ) noexcept
        {
            return (static_cast<std::uint64_t>(awaitable.generation) << 32U) |
                static_cast<std::uint32_t>(state);
        }

        void prepare(std::size_t queue_capacity, std::size_t awaitable_capacity)
        {
            cells = std::make_unique<Cell[]>(queue_capacity);
            tickets = std::make_unique<Ticket[]>(awaitable_capacity);
            capacity = queue_capacity;
            ticket_capacity = awaitable_capacity;
            for (std::size_t index{}; index < capacity; ++index)
                cells[index].sequence.store(index, std::memory_order_relaxed);
        }

        void open(ScriptAwaitableId awaitable, const std::optional<PreparedResumeType>& type) noexcept
        {
            if (!awaitable.valid() || awaitable.slot > ticket_capacity)
                std::terminate();
            auto& ticket = tickets[awaitable.slot - 1U];
            ticket.type.store(
                type ? type->type_id : lux::semantic::InvalidTypeId,
                std::memory_order_relaxed
            );
            ticket.size.store(type ? type->size : 0U, std::memory_order_relaxed);
            ticket.state.store(
                ticketState(awaitable, EExternalCompletionTicketState::ACTIVE),
                std::memory_order_release
            );
        }

        void close(ScriptAwaitableId awaitable) noexcept
        {
            if (!awaitable.valid() || awaitable.slot > ticket_capacity)
                return;
            auto& ticket = tickets[awaitable.slot - 1U];
            auto observed = ticket.state.load(std::memory_order_acquire);
            const auto active = ticketState(awaitable, EExternalCompletionTicketState::ACTIVE);
            const auto claimed = ticketState(awaitable, EExternalCompletionTicketState::CLAIMED);
            const auto closed_state = ticketState(awaitable, EExternalCompletionTicketState::CLOSED);
            while (observed == active || observed == claimed)
            {
                if (ticket.state.compare_exchange_weak(
                        observed,
                        closed_state,
                        std::memory_order_acq_rel,
                        std::memory_order_acquire
                    ))
                {
                    return;
                }
            }
        }

        [[nodiscard]] bool active(ScriptAwaitableId awaitable) const noexcept
        {
            if (closed.load(std::memory_order_acquire) || !awaitable.valid() || awaitable.slot > ticket_capacity)
                return false;
            return tickets[awaitable.slot - 1U].state.load(std::memory_order_acquire) ==
                ticketState(awaitable, EExternalCompletionTicketState::ACTIVE);
        }

        [[nodiscard]] lux::cxx::expected<void, EScriptAwaitableCompletionError> push(
            ExternalCompletionRecord record
        ) noexcept
        {
            if (closed.load(std::memory_order_acquire))
                return lux::cxx::unexpected(EScriptAwaitableCompletionError::STOPPING);
            if (!record.awaitable.valid() || record.awaitable.slot > ticket_capacity)
                return lux::cxx::unexpected(EScriptAwaitableCompletionError::INVALID_ID);

            auto& ticket = tickets[record.awaitable.slot - 1U];
            auto expected = ticketState(record.awaitable, EExternalCompletionTicketState::ACTIVE);
            const auto claimed = ticketState(record.awaitable, EExternalCompletionTicketState::CLAIMED);
            if (!ticket.state.compare_exchange_strong(
                    expected,
                    claimed,
                    std::memory_order_acq_rel,
                    std::memory_order_acquire
                ))
            {
                const auto generation = static_cast<std::uint32_t>(expected >> 32U);
                const auto state = static_cast<EExternalCompletionTicketState>(
                    static_cast<std::uint32_t>(expected)
                );
                const bool is_duplicate = generation == record.awaitable.generation &&
                    state == EExternalCompletionTicketState::CLAIMED;
                return lux::cxx::unexpected(
                    is_duplicate
                        ? EScriptAwaitableCompletionError::ALREADY_TERMINAL
                        : EScriptAwaitableCompletionError::INVALID_ID
                );
            }

            const auto expected_type = ticket.type.load(std::memory_order_relaxed);
            const auto expected_size = ticket.size.load(std::memory_order_relaxed);
            const bool expects_value = expected_type != lux::semantic::InvalidTypeId;
            const bool has_value = record.type != lux::semantic::InvalidTypeId;
            const bool is_invalid_value = expects_value != has_value ||
                (expects_value && (expected_type != record.type || expected_size != record.size ||
                    record.size > ScriptOwnedBytes::InlineCapacity));
            const bool is_invalid_ready = record.state == EScriptAwaitableState::READY && is_invalid_value;
            const bool is_invalid_failure = record.state != EScriptAwaitableState::READY &&
                (record.state != EScriptAwaitableState::FAILED || !record.error.valid() || has_value);
            if (is_invalid_ready || is_invalid_failure)
            {
                ticket.state.compare_exchange_strong(expected = claimed, ticketState(
                    record.awaitable,
                    EExternalCompletionTicketState::ACTIVE
                ));
                return lux::cxx::unexpected(EScriptAwaitableCompletionError::INVALID_VALUE);
            }

            auto depth = count.load(std::memory_order_relaxed);
            while (depth < capacity && !count.compare_exchange_weak(
                       depth,
                       depth + 1U,
                       std::memory_order_acq_rel,
                       std::memory_order_relaxed
                   ))
            {}
            if (depth >= capacity)
            {
                ticket.state.compare_exchange_strong(expected = claimed, ticketState(
                    record.awaitable,
                    EExternalCompletionTicketState::ACTIVE
                ));
                capacity_failures.fetch_add(1U, std::memory_order_relaxed);
                return lux::cxx::unexpected(EScriptAwaitableCompletionError::RESUME_QUEUE_FULL);
            }

            const auto position = enqueue_position.fetch_add(1U, std::memory_order_relaxed);
            auto* cell = std::addressof(cells[position % capacity]);
            while (cell->sequence.load(std::memory_order_acquire) != position)
            {}
            cell->record = record;
            cell->sequence.store(position + 1U, std::memory_order_release);
            auto observed_high_water = high_water.load(std::memory_order_relaxed);
            const auto current_depth = depth + 1U;
            while (current_depth > observed_high_water && !high_water.compare_exchange_weak(
                       observed_high_water,
                       current_depth,
                       std::memory_order_relaxed
                   ))
            {}
            return {};
        }

        [[nodiscard]] const ExternalCompletionRecord* front() const noexcept
        {
            if (capacity == 0U)
                return nullptr;
            const auto& cell = cells[dequeue_position % capacity];
            const auto sequence = cell.sequence.load(std::memory_order_acquire);
            return sequence == dequeue_position + 1U ? std::addressof(cell.record) : nullptr;
        }

        void pop() noexcept
        {
            auto& cell = cells[dequeue_position % capacity];
            close(cell.record.awaitable);
            cell.sequence.store(dequeue_position + capacity, std::memory_order_release);
            ++dequeue_position;
            count.fetch_sub(1U, std::memory_order_relaxed);
        }

        void stop() noexcept
        {
            closed.store(true, std::memory_order_release);
        }
    };
}
