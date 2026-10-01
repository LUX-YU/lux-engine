#pragma once

#include <lux/engine/editor/commands/Command.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <span>
#include <vector>

namespace lux::editor::commands
{
    class CommandEntry final
    {
    public:
        using Query = cxx::move_only_function<CommandResult<CommandState>(const CommandQuery&)>;
        using Execute = cxx::move_only_function<CommandResult<DispatchReceipt>(const CommandInvocation&)>;
        CommandEntry(contracts::CodeLease, CommandDescriptor, Query, Execute);
        ~CommandEntry();
        CommandEntry(const CommandEntry&) = delete;
        CommandEntry& operator=(const CommandEntry&) = delete;
        CommandEntry(CommandEntry&&) = delete;
        CommandEntry& operator=(CommandEntry&&) = delete;
        [[nodiscard]] const CommandDescriptor& descriptor() const noexcept;

        [[nodiscard]] bool usesCode(const contracts::CodeLease& code) const noexcept
        {
            return code_.sameOwner(code);
        }

    private:
        friend class CommandRegistry;
        friend class CommandRegistrySnapshot;
        contracts::CodeLease code_;
        CommandDescriptor descriptor_;
        Query query_;
        Execute execute_;
    };
    class CommandHandle;
    class CommandRegistrySnapshot final
    {
    public:
        CommandRegistrySnapshot() noexcept = default;
        [[nodiscard]] static CommandResult<CommandRegistrySnapshot> create(
            std::vector<std::shared_ptr<CommandEntry>>,
            std::size_t capacity = 256
        );
        [[nodiscard]] CommandResult<CommandHandle> find(CommandIdView) const;
        [[nodiscard]] std::span<const std::shared_ptr<CommandEntry>> entries() const noexcept;

    private:
        struct Data;
        std::shared_ptr<const Data> data_;
    };
    class CommandHandle final
    {
    public:
        [[nodiscard]] const CommandDescriptor& descriptor() const noexcept;
        [[nodiscard]] bool valid() const noexcept
        {
            return bool(entry_);
        }

    private:
        friend class CommandRegistrySnapshot;
        friend class CommandRegistry;
        explicit CommandHandle(std::shared_ptr<CommandEntry> entry) noexcept : entry_(std::move(entry)) {}
        std::shared_ptr<CommandEntry> entry_;
    };
    // Fixed-address owner-thread publication/call boundary. prepare candidates before replacing current.
    // Replacement returns the old snapshot: a multi-catalog publisher releases all old values only after
    // all catalogs have changed, then notifies. No callback or allocation occurs during replacement.
    class CommandRegistry final
    {
    public:
        // Owner-thread scope shared with a compound catalog publisher. Ordinary publication/dispatch
        // is BUSY throughout callbacks, cleanup and notification; pinned query/execute remain available.
        // The registry outlives the scope. Only preparePublication supplies a one-use commit permission.
        class Batch final
        {
        public:
            ~Batch();
            Batch(Batch&&) noexcept;
            Batch(const Batch&) = delete;
            Batch& operator=(const Batch&) = delete;
            Batch& operator=(Batch&&) = delete;
            // Prepared non-allocating swap, with no callbacks. Returns old owners for guarded cleanup.
            // Requires the original owner thread and an unconsumed publication permission.
            [[nodiscard]] CommandRegistrySnapshot commit() noexcept;

        private:
            friend class CommandRegistry;
            Batch(CommandRegistry&, std::optional<CommandRegistrySnapshot>) noexcept;
            CommandRegistry* owner_;
            std::optional<CommandRegistrySnapshot> candidate_;
        };
        [[nodiscard]] CommandResult<Batch> readBatch() noexcept;
        [[nodiscard]] CommandResult<Batch> preparePublication(CommandRegistrySnapshot) noexcept;
        CommandRegistry();
        ~CommandRegistry();
        CommandRegistry(const CommandRegistry&) = delete;
        CommandRegistry& operator=(const CommandRegistry&) = delete;
        CommandRegistry(CommandRegistry&&) = delete;
        CommandRegistry& operator=(CommandRegistry&&) = delete;
        [[nodiscard]] CommandResult<void> canPublish() const noexcept;
        [[nodiscard]] CommandResult<CommandRegistrySnapshot> publish(CommandRegistrySnapshot) noexcept;
        [[nodiscard]] CommandRegistrySnapshot snapshot() const noexcept;
        [[nodiscard]] std::uint64_t revision() const noexcept;
        [[nodiscard]] CommandResult<CommandState> query(CommandHandle, const CommandQuery&);
        [[nodiscard]] CommandResult<DispatchReceipt> execute(CommandHandle, const CommandInvocation&);

    private:
        friend class CommandDispatcher;
        [[nodiscard]] CommandResult<void> beginDispatch() noexcept;
        void endDispatch() noexcept;
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
    struct CommandCompletion final
    {
        std::uint64_t ticket;
        CommandResult<DispatchReceipt> result;
    };
    class CommandDispatcher final
    {
    public:
        explicit CommandDispatcher(CommandRegistry&, std::size_t capacity = 256);
        ~CommandDispatcher();
        CommandDispatcher(const CommandDispatcher&) = delete;
        CommandDispatcher& operator=(const CommandDispatcher&) = delete;
        CommandDispatcher(CommandDispatcher&&) = delete;
        CommandDispatcher& operator=(CommandDispatcher&&) = delete;
        // Refusal does not consume the invocation. A BUSY head retains its original identity and payload.
        [[nodiscard]] CommandResult<std::uint64_t> enqueue(CommandHandle, CommandInvocation&);
        [[nodiscard]] CommandResult<std::vector<CommandCompletion>> drain();
        [[nodiscard]] std::size_t pending() const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
