#pragma once

#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/editor/commands/Command.hpp>
#include <lux/engine/ui/Shortcut.hpp>
#include <span>
#include <vector>

namespace lux::editor::commands
{
    namespace detail
    {
        struct CommandIndexTestAccess;
    }
    class CommandEntry final
    {
    public:
        using Query = cxx::move_only_function<CommandResult<CommandState>(const CommandQuery&)>;
        using Execute = cxx::move_only_function<CommandResult<DispatchReceipt>(const CommandInvocation&)>;
        // Fixed declarations have static storage, including plugin literals retained by code.
        template <const CommandDescriptor& Descriptor>
        [[nodiscard]] static std::shared_ptr<CommandEntry> bind(
            lux::object::CodeLease code,
            Query query,
            Execute execute
        )
        {
            static_assert(
                Descriptor.id.isValid() && !Descriptor.label.empty() && Descriptor.input_version != 0,
                "Fixed command metadata must be a valid constant declaration."
            );
            // The UI borrows these literals directly for its C-string backend. Dynamic declarations
            // use create(), which freezes and terminates arbitrary input views in its single backing.
            static_assert(
                Descriptor.label.data()[Descriptor.label.size()] == '\0',
                "Fixed display labels must be terminated static text."
            );
            static_assert(
                Descriptor.shortcut.empty() || Descriptor.shortcut.data()[Descriptor.shortcut.size()] == '\0',
                "Fixed shortcut labels must be terminated static text."
            );
            return std::shared_ptr<CommandEntry>(
                new CommandEntry(std::move(code), Descriptor, std::move(query), std::move(execute))
            );
        }
        // Copies dynamic text once into immutable entry-owned storage before returning.
        // Every view in the input must be valid for this call; no input view escapes.
        [[nodiscard]] static std::shared_ptr<CommandEntry> create(
            lux::object::CodeLease,
            const CommandDescriptor&,
            Query,
            Execute
        );
        ~CommandEntry();
        CommandEntry(const CommandEntry&) = delete;
        CommandEntry& operator=(const CommandEntry&) = delete;
        CommandEntry(CommandEntry&&) = delete;
        CommandEntry& operator=(CommandEntry&&) = delete;
        [[nodiscard]] const CommandDescriptor& descriptor() const noexcept;
        [[nodiscard]] const lux::ui::ShortcutResult& shortcut() const noexcept;

        [[nodiscard]] bool usesCode(const lux::object::CodeLease& code) const noexcept
        {
            return code_.sameOwner(code);
        }

    private:
        friend class CommandRegistry;
        friend class CommandRegistrySnapshot;
        CommandEntry(lux::object::CodeLease, const CommandDescriptor&, Query, Execute);
        struct DescriptorStorage;
        lux::object::CodeLease code_;
        std::unique_ptr<const DescriptorStorage> storage_;
        const CommandDescriptor* descriptor_;
        lux::ui::ShortcutResult shortcut_;
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
        // External identity resolution: hash lookup followed by exact canonical-name validation.
        [[nodiscard]] CommandResult<CommandHandle> find(CommandIdView) const;
        // Index belongs to this immutable snapshot. Retain the resulting handle across publication.
        [[nodiscard]] CommandResult<CommandHandle> at(std::size_t) const;
        // Current-registration resolution. An unchanged entry does not compare or hash names.
        [[nodiscard]] CommandResult<CommandHandle> resolve(const CommandHandle&) const;
        [[nodiscard]] std::span<const std::shared_ptr<CommandEntry>> entries() const noexcept;

    private:
        friend struct detail::CommandIndexTestAccess;
        struct Data;
        [[nodiscard]] std::shared_ptr<CommandEntry> findHash(std::uint64_t) const noexcept;
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
            // Discard an uncommitted candidate inside all participating publication guards.
            // The batch continues to reject ordinary publication until destruction.
            void clearRetained() noexcept;

        private:
            friend class CommandRegistry;
            Batch(CommandRegistry&, std::optional<CommandRegistrySnapshot>) noexcept;
            CommandRegistry* owner_;
            std::optional<CommandRegistrySnapshot> candidate_;
            bool publication_;
        };
        // Read scopes may nest inside an executing command. They hold publication, not command execution
        // permission; releasing an inner scope never releases another reader or the active call/dispatch.
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
} // namespace lux::editor::commands
