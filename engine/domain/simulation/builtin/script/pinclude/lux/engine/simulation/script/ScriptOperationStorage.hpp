#pragma once

#include <lux/engine/simulation/script/ScriptWaitSource.hpp>
#include <vector>

namespace lux::simulation::script::detail
{
    // These are the only persistent execution and wait bodies. Directories below contain tickets only.
    struct ScriptExecutionState final
    {
        ScriptContinuationId id;
        ScriptInstanceId instance;
        ScriptBackendContinuation backend;
        ScriptAwaitableId waiting_on;
        std::uint32_t method_slot{};
        bool hook_single_flight{};
        ScriptContinuationId instance_previous;
        ScriptContinuationId instance_next;
        ScriptCellTicket cell;
    };

    struct ScriptWaitState final
    {
        ScriptAwaitableId id;
        ScriptInstanceId instance;
        ScriptContinuationId continuation;
        ScriptCellTicket attached_execution;
        LocalWaitTicket location;
        EScriptAwaitableState state{EScriptAwaitableState::PENDING};
        std::optional<PreparedResumeType> result_type;
        ScriptStepError error;
        bool resume_enqueued{};
        bool external_completion{};
        bool release_pending{};
        std::uint32_t write_pins{};
        ScriptAwaitableId instance_previous;
        ScriptAwaitableId instance_next;
        ScriptWaitSource source;
    };

    struct ScriptLocalWait final
    {
        ScriptWaitState state;
        // Never initialize unread result bytes. copy must succeed before the READY latch is published.
        alignas(std::max_align_t) std::byte bytes[ScriptOwnedBytes::InlineCapacity];
        ScriptLocalWait() noexcept {}
    };

    struct ScriptBoxedWait final
    {
        ScriptWaitState state;
        ScriptOwnedResumeValue value;
    };

    struct ScriptExecutionCellBody final
    {
        std::optional<ScriptExecutionState> execution;
        std::optional<ScriptLocalWait> wait;
    };

    enum class EScriptCellBody : std::uint8_t { EMPTY, EXECUTION, BOXED };

    struct ScriptOperationCell final
    {
        // Header lifetime spans the bank lifetime, including every body destruction and reuse.
        std::uint64_t epoch{};
        std::uint64_t wait_epoch{};
        std::size_t next_free{};
        EScriptCellBody kind{EScriptCellBody::EMPTY};
        union Body
        {
            ScriptExecutionCellBody execution;
            ScriptBoxedWait boxed;
            Body() noexcept {}
            ~Body() noexcept {}
        } body;
    };

    // Fixed public-identity directory: no growth, no generation wrapping, no result/execution objects.
    template<class Value, class Tag>
    class ScriptCellDirectory final
    {
    public:
        struct Key final
        {
            std::uint32_t index{UINT32_MAX};
            std::uint32_t gen{};
            [[nodiscard]] static constexpr Key invalid() noexcept { return {}; }
        };
        using key_type = Key;
        void reserve(std::size_t count)
        {
            entries_.resize(count);
            for (std::size_t i = 0; i < count; ++i)
                entries_[i].next = i + 1U;
            first_ = 0;
        }
        [[nodiscard]] std::optional<Key> tryEmplace(Value value) noexcept
        {
            if (first_ >= entries_.size()) return std::nullopt;
            const auto index = first_;
            auto& entry = entries_[index];
            first_ = entry.next;
            entry.value = value;
            entry.occupied = true;
            ++size_;
            return Key{static_cast<std::uint32_t>(index), entry.generation};
        }
        [[nodiscard]] Value* find(Key key) noexcept
        {
            if (key.index >= entries_.size()) return nullptr;
            auto& entry = entries_[key.index];
            return entry.occupied && entry.generation == key.gen ? &entry.value : nullptr;
        }
        [[nodiscard]] const Value* find(Key key) const noexcept
        {
            return const_cast<ScriptCellDirectory*>(this)->find(key);
        }
        [[nodiscard]] bool erase(Key key) noexcept
        {
            if (find(key) == nullptr) return false;
            auto& entry = entries_[key.index];
            entry.occupied = false;
            entry.value = {};
            --size_;
            if (++entry.generation != UINT32_MAX)
            {
                entry.next = first_;
                first_ = key.index;
            }
            return true;
        }
        [[nodiscard]] std::size_t size() const noexcept { return size_; }
        [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }
        [[nodiscard]] std::size_t capacity() const noexcept { return entries_.size(); }
        [[nodiscard]] std::size_t storageBytes() const noexcept { return entries_.capacity() * sizeof(Entry); }
        void clear() noexcept
        {
            if (size_ != 0U) std::terminate();
            entries_.clear();
            first_ = 0;
        }
    private:
        struct Entry final
        {
            Value value;
            std::size_t next{};
            std::uint32_t generation{1U};
            bool occupied{};
        };
        std::vector<Entry> entries_;
        std::size_t first_{};
        std::size_t size_{};
    };

    class ScriptOperationStorage final
    {
        struct ContinuationTag;
        struct AwaitableTag;
        using Executions = ScriptCellDirectory<ScriptCellTicket, ContinuationTag>;
        using Waits = ScriptCellDirectory<LocalWaitTicket, AwaitableTag>;
    public:
        using ContinuationKey = Executions::key_type;
        using AwaitableKey = Waits::key_type;

        // Small owner-local facades preserve separate admission and unlink domains.
        class Continuations final
        {
        public:
            using key_type = ContinuationKey;
            explicit Continuations(ScriptOperationStorage& owner) noexcept : owner_(owner) {}
            void reserve(std::size_t count) { owner_.executions_.reserve(count); }
            [[nodiscard]] auto size() const noexcept { return owner_.executions_.size(); }
            [[nodiscard]] bool empty() const noexcept { return owner_.executions_.empty(); }
            void clear() noexcept { owner_.executions_.clear(); }
            [[nodiscard]] ScriptExecutionState* find(key_type key) noexcept
            {
                const auto* ticket = owner_.executions_.find(key);
                return ticket ? owner_.execution(*ticket, {key.index + 1U, key.gen}) : nullptr;
            }
            [[nodiscard]] ScriptExecutionState& operator[](key_type key) noexcept { return *find(key); }
            [[nodiscard]] std::optional<key_type> tryEmplace(ScriptExecutionState value) noexcept
            {
                // Selection does not change the wait. Invalid/already-bound waits still fail at attach.
                ScriptCellTicket cell;
                const auto* selected = owner_.waits_.find({value.waiting_on.slot - 1U, value.waiting_on.generation});
                auto* wait = selected ? owner_.wait(*selected) : nullptr;
                if (wait && wait->instance == value.instance)
                {
                    auto* candidate = wait->location.cell.cell;
                    if (candidate->kind == EScriptCellBody::EXECUTION && !candidate->body.execution.execution)
                        cell = wait->location.cell;
                }
                const bool promoted = static_cast<bool>(cell);
                if (!promoted) cell = owner_.acquire(EScriptCellBody::EXECUTION);
                if (!cell) return std::nullopt;
                const auto inserted = owner_.executions_.tryEmplace(cell);
                if (!inserted)
                {
                    if (!promoted) owner_.releaseEmpty(cell);
                    return std::nullopt;
                }
                value.id = {inserted->index + 1U, inserted->gen};
                value.cell = cell;
                cell.cell->body.execution.execution.emplace(value);
                return inserted;
            }
            [[nodiscard]] bool erase(key_type key) noexcept
            {
                const auto* found = owner_.executions_.find(key);
                if (!found) return false;
                const auto cell = *found;
                static_cast<void>(owner_.executions_.erase(key));
                cell.cell->body.execution.execution.reset();
                owner_.releaseEmpty(cell);
                return true;
            }
        private:
            ScriptOperationStorage& owner_;
        };

        class Awaitables final
        {
        public:
            using key_type = AwaitableKey;
            explicit Awaitables(ScriptOperationStorage& owner) noexcept : owner_(owner) {}
            void reserve(std::size_t count) { owner_.waits_.reserve(count); }
            [[nodiscard]] auto size() const noexcept { return owner_.waits_.size(); }
            [[nodiscard]] auto capacity() const noexcept { return owner_.waits_.capacity(); }
            [[nodiscard]] auto storageBytes() const noexcept { return owner_.waits_.storageBytes(); }
            [[nodiscard]] bool empty() const noexcept { return owner_.waits_.empty(); }
            void clear() noexcept { owner_.waits_.clear(); }
            [[nodiscard]] ScriptWaitState* find(key_type key) noexcept
            {
                const auto* ticket = owner_.waits_.find(key);
                auto* result = ticket ? owner_.wait(*ticket) : nullptr;
                return result && result->id == ScriptAwaitableId{key.index + 1U, key.gen} ? result : nullptr;
            }
            [[nodiscard]] std::optional<key_type> admit(ScriptInstanceId instance,
                std::optional<PreparedResumeType> type, bool external, ScriptCellTicket* scope) noexcept
            {
                const bool eligible = !external && scope && (!type ||
                    (type->size <= ScriptOwnedBytes::InlineCapacity && type->alignment <= alignof(std::max_align_t)));
                ScriptCellTicket cell;
                bool local{};
                if (eligible)
                {
                    cell = *scope;
                    if (!owner_.valid(cell) || cell.cell->kind != EScriptCellBody::EXECUTION)
                        cell = {};
                    if (!cell) cell = owner_.acquire(EScriptCellBody::EXECUTION);
                    local = cell && !cell.cell->body.execution.wait;
                }
                if (!local) cell = owner_.acquire(EScriptCellBody::BOXED);
                if (!cell) return std::nullopt;
                if (cell.cell->wait_epoch == UINT64_MAX)
                {
                    owner_.releaseEmpty(cell);
                    return std::nullopt;
                }
                const LocalWaitTicket ticket{cell, ++cell.cell->wait_epoch};
                const auto inserted = owner_.waits_.tryEmplace(ticket);
                if (!inserted)
                {
                    owner_.releaseEmpty(cell);
                    return std::nullopt;
                }
                auto& record = local ? cell.cell->body.execution.wait.emplace().state : cell.cell->body.boxed.state;
                record.id = {inserted->index + 1U, inserted->gen};
                record.instance = instance;
                record.result_type = type;
                record.external_completion = external;
                record.location = ticket;
                if (!local && !external && type)
                {
                    auto& value = cell.cell->body.boxed.value;
                    value.type = *type;
                    if (!value.bytes.resize(type->size, type->alignment))
                    {
                        static_cast<void>(erase(*inserted));
                        return std::nullopt;
                    }
                }
                if (local) *scope = cell;
                return inserted;
            }
            [[nodiscard]] bool erase(key_type key) noexcept
            {
                const auto* found = owner_.waits_.find(key);
                if (!found) return false;
                const auto ticket = *found;
                auto* record = owner_.wait(ticket);
                if (!record || record->write_pins != 0U) std::terminate();
                static_cast<void>(owner_.waits_.erase(key));
                if (ticket.cell.cell->kind == EScriptCellBody::EXECUTION)
                {
                    ticket.cell.cell->body.execution.wait.reset();
                    owner_.releaseEmpty(ticket.cell);
                }
                else owner_.release(ticket.cell);
                return true;
            }
        private:
            ScriptOperationStorage& owner_;
        };

        void prepare(std::size_t continuations, std::size_t awaitables)
        {
            // Caller validates limit arithmetic; allocation failure is handled by the existing prepare boundary.
            capacity_ = continuations + awaitables;
            cells_ = std::make_unique<ScriptOperationCell[]>(capacity_);
            for (std::size_t i = 0; i < capacity_; ++i) cells_[i].next_free = i + 1U;
            first_ = 0;
        }
        [[nodiscard]] bool valid(ScriptCellTicket ticket) const noexcept
        {
            return ticket.cell && ticket.cell->epoch == ticket.epoch && ticket.cell->kind != EScriptCellBody::EMPTY;
        }
        [[nodiscard]] ScriptExecutionState* execution(ScriptCellTicket cell, ScriptContinuationId id) noexcept
        {
            if (!valid(cell) || cell.cell->kind != EScriptCellBody::EXECUTION) return nullptr;
            auto& execution = cell.cell->body.execution.execution;
            return execution && execution->id == id ? &*execution : nullptr;
        }
        [[nodiscard]] ScriptWaitState* wait(LocalWaitTicket ticket) noexcept
        {
            if (!valid(ticket.cell) || ticket.cell.cell->wait_epoch != ticket.wait_epoch) return nullptr;
            auto& cell = *ticket.cell.cell;
            if (cell.kind == EScriptCellBody::BOXED) return &cell.body.boxed.state;
            auto& local = cell.body.execution.wait;
            return local ? &local->state : nullptr;
        }
        [[nodiscard]] static bool local(const ScriptWaitState& wait) noexcept
        {
            return wait.location.cell.cell->kind == EScriptCellBody::EXECUTION;
        }
        [[nodiscard]] static std::span<std::byte> bytes(ScriptWaitState& wait) noexcept
        {
            if (local(wait)) return {wait.location.cell.cell->body.execution.wait->bytes,
                wait.result_type ? wait.result_type->size : 0U};
            return wait.location.cell.cell->body.boxed.value.bytes.span();
        }
        [[nodiscard]] static ScriptOwnedResumeValue takeValue(ScriptWaitState& wait) noexcept
        {
            if (!local(wait)) return std::move(wait.location.cell.cell->body.boxed.value);
            ScriptOwnedResumeValue result;
            if (wait.state == EScriptAwaitableState::READY && wait.result_type)
            {
                result.type = *wait.result_type;
                if (!result.bytes.resize(result.type.size, result.type.alignment)) std::terminate();
                std::memcpy(result.bytes.data(), bytes(wait).data(), result.type.size);
            }
            return result;
        }
        static void supply(ScriptWaitState& wait, ScriptOwnedResumeValue value) noexcept
        {
            if (local(wait))
            {
                if (!value.bytes.empty()) std::memcpy(bytes(wait).data(), value.bytes.data(), value.bytes.size());
            }
            else wait.location.cell.cell->body.boxed.value = std::move(value);
        }
        [[nodiscard]] std::size_t capacity() const noexcept { return capacity_; }
        [[nodiscard]] std::size_t used() const noexcept { return used_; }
        [[nodiscard]] std::size_t bankBytes() const noexcept { return capacity_ * sizeof(ScriptOperationCell); }
        [[nodiscard]] std::size_t continuationDirectoryBytes() const noexcept { return executions_.storageBytes(); }
        void clear() noexcept
        {
            if (used_ != 0U) std::terminate();
            cells_.reset();
            capacity_ = first_ = 0;
        }
    private:
        [[nodiscard]] ScriptCellTicket acquire(EScriptCellBody kind) noexcept
        {
            while (first_ < capacity_)
            {
                auto& cell = cells_[first_];
                first_ = cell.next_free;
                if (cell.epoch == UINT64_MAX) continue;
                ++cell.epoch;
                cell.kind = kind;
                if (kind == EScriptCellBody::EXECUTION) std::construct_at(&cell.body.execution);
                else std::construct_at(&cell.body.boxed);
                ++used_;
                return {&cell, cell.epoch};
            }
            return {};
        }
        void releaseEmpty(ScriptCellTicket cell) noexcept
        {
            if (cell.cell->kind == EScriptCellBody::BOXED ||
                (!cell.cell->body.execution.execution && !cell.cell->body.execution.wait)) release(cell);
        }
        void release(ScriptCellTicket ticket) noexcept
        {
            auto& cell = *ticket.cell;
            if (cell.kind == EScriptCellBody::EXECUTION) std::destroy_at(&cell.body.execution);
            else std::destroy_at(&cell.body.boxed);
            cell.kind = EScriptCellBody::EMPTY;
            cell.next_free = first_;
            first_ = static_cast<std::size_t>(&cell - cells_.get());
            --used_;
        }
        std::unique_ptr<ScriptOperationCell[]> cells_;
        Executions executions_;
        Waits waits_;
        std::size_t capacity_{};
        std::size_t first_{};
        std::size_t used_{};
    };
}
