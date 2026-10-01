#include <lux/engine/editor/commands/CommandRegistry.hpp>
#include <algorithm>
#include <thread>
#include <utility>

namespace lux::editor::commands
{
    namespace
    {
        auto failure(ECommandError error)
        {
            return cxx::unexpected(CommandFailure{error, "command"});
        }
        bool validShortcut(std::string_view shortcut)
        {
            if (shortcut.empty())
                return true;
            for (const auto modifier : {"Ctrl+", "Shift+", "Alt+"})
                if (shortcut.starts_with(modifier))
                    shortcut.remove_prefix(std::char_traits<char>::length(modifier));
            const bool is_letter = shortcut.size() == 1 && shortcut.front() >= 'A' && shortcut.front() <= 'Z';
            const bool is_named = shortcut == "Delete" || shortcut == "Enter" || shortcut == "Escape";
            return is_letter || is_named;
        }
        CommandResult<void> validate(const CommandDescriptor& entry, const CommandQuery& query)
        {
            const bool is_scope_mismatch = static_cast<std::size_t>(entry.scope) != query.target.index();
            const bool is_argument_mismatch = entry.argument_type != query.arguments.type() || !query.arguments.valid();
            if (is_scope_mismatch || is_argument_mismatch)
                return failure(ECommandError::INVALID_ARGUMENT);
            if (const auto* target = std::get_if<SessionTarget>(&query.target))
            {
                const bool is_invalid_id = !target->id.valid();
                const bool is_invalid_stamp =
                    target->based_on && (target->based_on->session != target->id || !target->based_on->state.valid());
                if (is_invalid_id || is_invalid_stamp)
                    return failure(ECommandError::INVALID_ARGUMENT);
            }
            if (const auto* target = std::get_if<views::ViewId>(&query.target); target && !target->valid())
                return failure(ECommandError::INVALID_ARGUMENT);
            return {};
        }
        struct CallScope final
        {
            bool& active;
            explicit CallScope(bool& value) noexcept : active(value)
            {
                active = true;
            }
            ~CallScope()
            {
                active = false;
            }
        };
    }
    struct CommandArguments::Data final
    {
        contracts::CodeLease code;
        cxx::TypeToken type;
        std::shared_ptr<const void> value;
    };
    CommandArguments::CommandArguments(
        contracts::CodeLease code,
        cxx::TypeToken type,
        std::shared_ptr<const void> value
    )
        : data_(std::make_shared<Data>(std::move(code), type, std::move(value)))
    {}
    CommandArguments::~CommandArguments()
    {
        const auto code = data_ ? data_->code : contracts::CodeLease::builtin();
        data_.reset();
    }
    CommandArguments& CommandArguments::operator=(CommandArguments other) noexcept
    {
        data_.swap(other.data_);
        return *this;
    }
    cxx::TypeToken CommandArguments::type() const noexcept
    {
        return data_ ? data_->type : cxx::TypeToken{};
    }
    const void* CommandArguments::data() const noexcept
    {
        return data_ ? data_->value.get() : nullptr;
    }
    bool CommandArguments::valid() const noexcept
    {
        return !data_ || (data_->code.valid() && data_->type.isValid() && bool(data_->value));
    }
    CommandInvocation::CommandInvocation(
        VCommandTarget target,
        CommandArguments arguments,
        ERegistryBinding registration
    ) noexcept
        : target_(std::move(target)), arguments_(std::move(arguments)), registration_(registration)
    {}
    const VCommandTarget& CommandInvocation::target() const noexcept
    {
        return target_;
    }
    const CommandArguments& CommandInvocation::arguments() const noexcept
    {
        return arguments_;
    }
    ERegistryBinding CommandInvocation::registration() const noexcept
    {
        return registration_;
    }
    CommandQuery CommandInvocation::query() const noexcept
    {
        return {target_, arguments_};
    }
    CommandEntry::CommandEntry(contracts::CodeLease code, CommandDescriptor descriptor, Query query, Execute execute)
        : code_(std::move(code)), descriptor_(std::move(descriptor)), query_(std::move(query)),
          execute_(std::move(execute))
    {}
    CommandEntry::~CommandEntry() = default;
    const CommandDescriptor& CommandEntry::descriptor() const noexcept
    {
        return descriptor_;
    }
    const CommandDescriptor& CommandHandle::descriptor() const noexcept
    {
        return entry_->descriptor();
    }
    struct CommandRegistrySnapshot::Data final
    {
        std::vector<std::shared_ptr<CommandEntry>> entries;
    };
    CommandResult<CommandRegistrySnapshot> CommandRegistrySnapshot::create(
        std::vector<std::shared_ptr<CommandEntry>> entries,
        std::size_t capacity
    )
    {
        for (auto& entry : entries)
            if (entry)
            {
                auto code = entry->code_;
                entry = contracts::pinCodeOwner(std::move(code), std::move(entry));
            }
        if (entries.size() > capacity)
            return failure(ECommandError::CAPACITY);
        for (std::size_t i{}; i < entries.size(); ++i)
        {
            if (!entries[i])
                return failure(ECommandError::INVALID_ARGUMENT);
            const auto& entry = *entries[i];
            const bool is_invalid =
                !entry.code_.valid() || !entry.descriptor_.id.isValid() || entry.descriptor_.label.empty() ||
                entry.descriptor_.input_version == 0 ||
                static_cast<unsigned>(entry.descriptor_.scope) > static_cast<unsigned>(ECommandScope::VIEW) ||
                !validShortcut(entry.descriptor_.shortcut) || !entry.query_ || !entry.execute_;
            if (is_invalid)
                return failure(ECommandError::INVALID_ARGUMENT);
            for (std::size_t j{}; j < i; ++j)
                if (entries[j]->descriptor().id == entry.descriptor().id)
                    return failure(ECommandError::INVALID_ARGUMENT);
        }
        CommandRegistrySnapshot result;
        result.data_ = std::make_shared<Data>(std::move(entries));
        return result;
    }
    CommandResult<CommandHandle> CommandRegistrySnapshot::find(CommandIdView id) const
    {
        for (const auto& entry : entries())
            if (entry->descriptor().id.name() == id.name())
                return CommandHandle{entry};
        return failure(ECommandError::NOT_FOUND);
    }
    std::span<const std::shared_ptr<CommandEntry>> CommandRegistrySnapshot::entries() const noexcept
    {
        return data_ ? std::span<const std::shared_ptr<CommandEntry>>{data_->entries}
                     : std::span<const std::shared_ptr<CommandEntry>>{};
    }
    struct CommandRegistry::Impl final
    {
        std::thread::id owner{std::this_thread::get_id()};
        bool calling{};
        bool dispatching{};
        std::uint64_t revision{};
        CommandRegistrySnapshot current;
        CommandResult<void> canCall() const noexcept
        {
            if (owner != std::this_thread::get_id())
                return failure(ECommandError::WRONG_THREAD);
            if (calling)
                return failure(ECommandError::BUSY);
            return {};
        }
    };
    CommandRegistry::CommandRegistry() : impl_(std::make_unique<Impl>()) {}
    CommandRegistry::~CommandRegistry() = default;
    CommandResult<void> CommandRegistry::canPublish() const noexcept
    {
        if (impl_->owner != std::this_thread::get_id())
            return failure(ECommandError::WRONG_THREAD);
        if (impl_->calling || impl_->dispatching)
            return failure(ECommandError::BUSY);
        if (impl_->revision == UINT64_MAX)
            return failure(ECommandError::CAPACITY);
        return {};
    }
    CommandResult<CommandRegistrySnapshot> CommandRegistry::publish(CommandRegistrySnapshot candidate) noexcept
    {
        if (const auto ready = canPublish(); !ready)
            return cxx::unexpected(ready.error());
        auto previous = std::exchange(impl_->current, std::move(candidate));
        ++impl_->revision;
        return previous;
    }
    CommandRegistrySnapshot CommandRegistry::snapshot() const noexcept
    {
        return impl_->current;
    }
    CommandResult<void> CommandRegistry::beginDispatch() noexcept
    {
        const auto ready = impl_->canCall();
        if (!ready)
            return ready;
        if (impl_->dispatching)
            return failure(ECommandError::BUSY);
        impl_->dispatching = true;
        return {};
    }
    void CommandRegistry::endDispatch() noexcept
    {
        impl_->dispatching = false;
    }
    std::uint64_t CommandRegistry::revision() const noexcept
    {
        return impl_->revision;
    }
    CommandResult<CommandState> CommandRegistry::query(CommandHandle handle, const CommandQuery& input)
    {
        if (const auto ready = impl_->canCall(); !ready)
            return cxx::unexpected(ready.error());
        CallScope scope{impl_->calling};
        auto pinned = std::move(handle);
        if (!pinned.valid())
            return failure(ECommandError::INVALID_ARGUMENT);
        if (const auto checked = validate(pinned.descriptor(), input); !checked)
            return cxx::unexpected(checked.error());
        if (pinned.entry_->code_.sameOwner(contracts::CodeLease::builtin()))
            return pinned.entry_->query_(input);
        // Foreign callable boundary only; built-in dispatch does not pay for exception containment.
        try
        {
            return pinned.entry_->query_(input);
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (...)
        {
            return cxx::unexpected(CommandFailure{ECommandError::DOMAIN_FAILURE, "plugin.command.query"});
        }
    }
    CommandResult<DispatchReceipt> CommandRegistry::execute(CommandHandle handle, const CommandInvocation& input)
    {
        if (const auto ready = impl_->canCall(); !ready)
            return cxx::unexpected(ready.error());
        CallScope scope{impl_->calling};
        auto pinned = std::move(handle);
        if (!pinned.valid())
            return failure(ECommandError::INVALID_ARGUMENT);
        if (const auto checked = validate(pinned.descriptor(), input.query()); !checked)
            return cxx::unexpected(checked.error());
        auto invoke = [&]() -> CommandResult<DispatchReceipt> {
            const auto state = pinned.entry_->query_(input.query());
            if (!state)
                return cxx::unexpected(state.error());
            if (!state->enabled)
                return cxx::unexpected(CommandFailure{ECommandError::DISABLED, "command", 0, state->reason});
            return pinned.entry_->execute_(input);
        };
        if (pinned.entry_->code_.sameOwner(contracts::CodeLease::builtin()))
            return invoke();
        try
        {
            return invoke();
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (...)
        {
            return cxx::unexpected(CommandFailure{ECommandError::DOMAIN_FAILURE, "plugin.command.execute"});
        }
    }
}
