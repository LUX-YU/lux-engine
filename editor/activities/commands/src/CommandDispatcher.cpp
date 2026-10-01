#include <lux/engine/editor/commands/CommandRegistry.hpp>
#include <deque>
#include <thread>

namespace lux::editor::commands
{
    namespace
    {
        auto failure(ECommandError error)
        {
            return cxx::unexpected(CommandFailure{error, "command.dispatch"});
        }
    }
    struct CommandDispatcher::Impl final
    {
        struct Pending final
        {
            std::uint64_t ticket;
            CommandHandle handle;
            CommandInvocation input;
        };
        CommandRegistry& registry;
        const std::thread::id owner{std::this_thread::get_id()};
        const std::size_t capacity;
        std::uint64_t next{1};
        bool draining{};
        std::deque<Pending> queue;
        Impl(CommandRegistry& value, std::size_t limit) noexcept : registry(value), capacity(limit) {}
        CommandResult<void> ready() const
        {
            if (owner != std::this_thread::get_id())
                return failure(ECommandError::WRONG_THREAD);
            if (draining)
                return failure(ECommandError::BUSY);
            return {};
        }
        CommandResult<CommandHandle> resolve(const Pending& pending) const
        {
            if (pending.input.registration() == ERegistryBinding::PINNED)
                return pending.handle;
            const auto& original = pending.handle.descriptor();
            auto current = registry.snapshot().find(CommandIdView{original.id.name()});
            if (!current)
                return cxx::unexpected(current.error());
            const auto& replacement = current->descriptor();
            const bool is_incompatible = replacement.scope != original.scope ||
                                         replacement.input_version != original.input_version ||
                                         replacement.argument_type != original.argument_type;
            if (is_incompatible)
                return failure(ECommandError::INCOMPATIBLE_REGISTRATION);
            return current;
        }
    };
    CommandDispatcher::CommandDispatcher(CommandRegistry& registry, std::size_t capacity)
        : impl_(std::make_unique<Impl>(registry, capacity))
    {}
    CommandDispatcher::~CommandDispatcher() = default;
    CommandResult<std::uint64_t> CommandDispatcher::enqueue(CommandHandle handle, CommandInvocation& input)
    {
        if (const auto ready = impl_->ready(); !ready)
            return cxx::unexpected(ready.error());
        const bool is_full = impl_->queue.size() == impl_->capacity || impl_->next == UINT64_MAX;
        if (is_full)
            return failure(ECommandError::CAPACITY);
        if (!handle.valid())
            return failure(ECommandError::INVALID_ARGUMENT);
        // Query at dispatch, not at click: Save captures then-current content of this fixed session.
        const auto ticket = impl_->next++;
        impl_->queue.push_back({ticket, std::move(handle), std::move(input)});
        return ticket;
    }
    CommandResult<std::vector<CommandCompletion>> CommandDispatcher::drain()
    {
        if (const auto ready = impl_->ready(); !ready)
            return cxx::unexpected(ready.error());
        const auto admitted = impl_->registry.beginDispatch();
        if (!admitted)
            return cxx::unexpected(admitted.error());
        struct DrainScope final
        {
            bool& active;
            CommandRegistry& registry;
            explicit DrainScope(bool& value, CommandRegistry& registry) : active(value), registry(registry)
            {
                active = true;
            }
            ~DrainScope()
            {
                active = false;
                registry.endDispatch();
            }
        } scope{impl_->draining, impl_->registry};
        std::vector<CommandCompletion> completed;
        const auto count = impl_->queue.size();
        completed.reserve(count);
        for (std::size_t i{}; i < count; ++i)
        {
            auto& pending = impl_->queue.front();
            auto handle = impl_->resolve(pending);
            auto result = handle ? impl_->registry.execute(std::move(*handle), pending.input)
                                 : CommandResult<DispatchReceipt>{cxx::unexpected(handle.error())};
            if (!result && result.error().code == ECommandError::BUSY)
                break;
            completed.push_back({pending.ticket, std::move(result)});
            impl_->queue.pop_front();
        }
        return completed;
    }
    std::size_t CommandDispatcher::pending() const noexcept
    {
        return impl_->queue.size();
    }
}
