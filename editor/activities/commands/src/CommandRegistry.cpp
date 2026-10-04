#include <lux/engine/editor/commands/CommandRegistry.hpp>
#if defined(LUX_COMMAND_TEST_ACCESS)
#include <lux/engine/editor/commands/CommandIndexTestAccess.hpp>
#endif
#include <algorithm>
#include <lux/engine/editor/commands/CommandIndex.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>
#include <thread>
#include <unordered_map>
#include <utility>

namespace lux::editor::commands
{
    namespace
    {
#if defined(LUX_COMMAND_TEST_ACCESS)
        // Qualification counters only; absent from non-test libraries and all installed headers.
        // Slots: business hashes, name compares, lookups, numeric comparisons, index bytes,
        // owned text bytes, text backings, entries, shortcut parses. Caller-created IDs are outside scope.
        thread_local std::uint64_t measurements[9]{};
        thread_local bool measuring{};
        void count(std::size_t slot, std::uint64_t amount = 1) noexcept
        {
            if (measuring)
            {
                measurements[slot] += amount;
            }
        }
#else
        void count(std::size_t, std::uint64_t = 1) noexcept {}
#endif
        auto failure(ECommandError error)
        {
            return cxx::unexpected(CommandFailure{error, "command"});
        }
        auto dependencyFailure(services::ServiceFailure error)
        {
            using enum services::EServiceError;
            const auto code = error.code == BUSY     ? ECommandError::BUSY
                              : error.code == CLOSED ? ECommandError::CLOSED
                                                     : ECommandError::DOMAIN_FAILURE;
            const auto domain_code = error.domain.empty() ? static_cast<std::uint64_t>(error.code) : error.domain_code;
            return cxx::unexpected(CommandFailure{
                code,
                error.domain.empty() ? "command.services" : std::move(error.domain),
                domain_code,
                std::move(error.detail)
            });
        }
        CommandResult<void> validate(const CommandDescriptor& entry, const CommandQuery& query)
        {
            const bool is_scope_mismatch = static_cast<std::size_t>(entry.scope) != query.target.index();
            const bool is_argument_mismatch = entry.argument_type != query.arguments.type() || !query.arguments.valid();
            if (is_scope_mismatch || is_argument_mismatch)
            {
                return failure(ECommandError::INVALID_ARGUMENT);
            }
            if (const auto* target = std::get_if<SessionTarget>(&query.target))
            {
                const bool is_invalid_id = !target->id.valid();
                const bool is_invalid_stamp =
                    target->based_on && (target->based_on->session != target->id || !target->based_on->state.valid());
                if (is_invalid_id || is_invalid_stamp)
                {
                    return failure(ECommandError::INVALID_ARGUMENT);
                }
            }
            if (const auto* target = std::get_if<views::ViewId>(&query.target); target && !target->valid())
            {
                return failure(ECommandError::INVALID_ARGUMENT);
            }
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
    } // namespace
#if defined(LUX_COMMAND_TEST_ACCESS)
    extern "C" void luxEc3CommandCounts(std::uint64_t* output, bool enabled) noexcept
    {
        std::copy(std::begin(measurements), std::end(measurements), output);
        std::fill(std::begin(measurements), std::end(measurements), 0);
        measuring = enabled;
    }
#endif
    struct CommandArguments::Data final
    {
        lux::object::CodeLease code;
        cxx::TypeToken type;
        std::shared_ptr<const void> value;
    };
    CommandArguments::CommandArguments(
        lux::object::CodeLease code,
        cxx::TypeToken type,
        std::shared_ptr<const void> value
    )
        : data_(std::make_shared<Data>(std::move(code), type, std::move(value)))
    {
    }
    CommandArguments::~CommandArguments()
    {
        const auto code = data_ ? data_->code : lux::object::CodeLease::builtin();
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
    {
    }
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
    struct CommandEntry::DescriptorStorage final
    {
        std::string text;
        std::vector<std::string> dependency_names;
        std::vector<services::ServiceDependency> dependencies;
        CommandDescriptor descriptor;
        explicit DescriptorStorage(const CommandDescriptor& input)
        {
            const auto id_size = input.id.name().size();
            const auto label_size = input.label.size();
            const auto group_size = input.group.size();
            const auto shortcut_size = input.shortcut.size();
            const auto argument_size = input.argument_type.name().size();
            text.reserve(id_size + label_size + group_size + shortcut_size + argument_size + 5);
            const auto append = [&](std::string_view value)
            {
                const auto offset = text.size();
                text.append(value).push_back('\0');
                return offset;
            };
            const auto id = append(input.id.name());
            const auto label = append(input.label);
            const auto group = append(input.group);
            const auto shortcut = append(input.shortcut);
            const auto argument = append(input.argument_type.name());
            // Final storage does not move. Display slices also have a terminator for UI backends.
            const std::string_view bytes{text};
            count(0);
            count(5, text.size());
            count(6);
            descriptor = {
                CommandIdView{bytes.substr(id, id_size)},
                bytes.substr(label, label_size),
                bytes.substr(group, group_size),
                bytes.substr(shortcut, shortcut_size),
                input.scope,
                input.input_version,
                {input.argument_type.hash(), bytes.substr(argument, argument_size)}
            };
            dependency_names.reserve(input.dependencies.size() * 4);
            const auto name = [&](std::string_view value) -> std::string_view
            {
                dependency_names.emplace_back(value);
                return dependency_names.back();
            };
            for (auto value : input.dependencies)
            {
                value.contract = services::ServiceNameView{name(value.contract.name())};
                if (value.implementation.isValid())
                {
                    value.implementation = services::ServiceNameView{name(value.implementation.name())};
                }
                value.type = {value.type.hash(), name(value.type.name())};
                value.qualifier = name(value.qualifier);
                dependencies.push_back(value);
            }
            descriptor.dependencies = dependencies;
            descriptor.create = input.create;
        }
    };
    CommandEntry::CommandEntry(
        lux::object::CodeLease code,
        const CommandDescriptor& descriptor,
        Query query,
        Execute execute
    )
        : code_(std::move(code)), descriptor_(&descriptor), shortcut_(lux::ui::parseShortcut(descriptor.shortcut)),
          binding_(std::move(query), std::move(execute))
    {
        count(7);
        count(8);
    }
    std::shared_ptr<CommandEntry> CommandEntry::create(
        lux::object::CodeLease code,
        const CommandDescriptor& descriptor,
        Query query,
        Execute execute
    )
    {
        auto entry = std::shared_ptr<CommandEntry>(
            new CommandEntry(std::move(code), descriptor, std::move(query), std::move(execute))
        );
        entry->storage_ = std::make_unique<const DescriptorStorage>(descriptor);
        entry->descriptor_ = &entry->storage_->descriptor;
        return entry;
    }
    CommandEntry::~CommandEntry() = default;
    const CommandDescriptor& CommandEntry::descriptor() const noexcept
    {
        return *descriptor_;
    }
    const lux::ui::ShortcutResult& CommandEntry::shortcut() const noexcept
    {
        return shortcut_;
    }
    const CommandDescriptor& CommandHandle::descriptor() const noexcept
    {
        return entry_->descriptor();
    }
    struct CommandRegistrySnapshot::Data final
    {
        std::vector<std::shared_ptr<CommandEntry>> entries;
        std::vector<detail::CommandIndex> index;
    };
#if defined(LUX_COMMAND_TEST_ACCESS)
    CommandResult<CommandRegistrySnapshot> detail::CommandIndexTestAccess::withSingleHash(
        const CommandRegistrySnapshot& source,
        std::uint64_t hash
    )
    {
        if (source.entries().size() != 1)
        {
            return failure(ECommandError::INVALID_ARGUMENT);
        }
        CommandRegistrySnapshot result;
        result.data_ = std::make_shared<CommandRegistrySnapshot::Data>(
            source.data_->entries,
            std::vector<detail::CommandIndex>{{hash, 0}}
        );
        return result;
    }
#endif
    CommandResult<CommandRegistrySnapshot> CommandRegistrySnapshot::create(
        std::vector<std::shared_ptr<CommandEntry>> entries,
        std::size_t capacity
    )
    {
        for (auto& entry : entries)
        {
            if (entry)
            {
                auto code = entry->code_;
                entry = lux::object::pinCodeOwner(std::move(code), std::move(entry));
            }
        }
        if (entries.size() > capacity)
        {
            return failure(ECommandError::CAPACITY);
        }
        std::unordered_map<std::uint64_t, std::string_view> dependency_names;
        const auto check_name = [&](services::ServiceNameView value)
        {
            if (!value.isValid())
            {
                return true;
            }
            const auto [found, inserted] = dependency_names.emplace(value.hash(), value.name());
            return inserted || found->second == value.name();
        };
        for (std::size_t i{}; i < entries.size(); ++i)
        {
            if (!entries[i])
            {
                return failure(ECommandError::INVALID_ARGUMENT);
            }
            const auto& entry = *entries[i];
            const auto& descriptor = entry.descriptor();
            count(0);
            const bool is_invalid_identity =
                !descriptor.id.isValid() || descriptor.id.hash() != cxx::Fnv1a64::hash(descriptor.id.name());
            const bool is_invalid_description =
                descriptor.label.empty() || descriptor.input_version == 0 ||
                static_cast<unsigned>(descriptor.scope) > static_cast<unsigned>(ECommandScope::VIEW);
            const bool is_direct_binding = entry.binding_.query && entry.binding_.execute;
            const bool has_direct_binding = entry.binding_.query || entry.binding_.execute;
            const bool is_invalid_binding =
                !entry.code_.valid() ||
                (descriptor.create ? has_direct_binding : (!is_direct_binding || !descriptor.dependencies.empty()));
            const bool is_invalid = is_invalid_identity || is_invalid_description || is_invalid_binding;
            if (is_invalid)
            {
                return failure(ECommandError::INVALID_ARGUMENT);
            }
            for (const auto& dependency : descriptor.dependencies)
            {
                const bool is_invalid_dependency = !dependency.contract.isValid() || !dependency.version ||
                                                   !dependency.type.isValid() ||
                                                   dependency.kind > services::EDependencyKind::BORROWED ||
                                                   dependency.scope > services::EDependencyScope::ROOT;
                if (is_invalid_dependency)
                {
                    return failure(ECommandError::INVALID_ARGUMENT);
                }
                const bool has_collision = !check_name(dependency.contract) || !check_name(dependency.implementation);
                if (has_collision)
                {
                    return failure(ECommandError::HASH_COLLISION);
                }
            }
        }
        auto index = detail::commandIndex(entries, [](CommandIdView id) { return id.hash(); });
        if (!index)
        {
            return cxx::unexpected(index.error());
        }
        for (std::size_t i{}; i < entries.size(); ++i)
        {
            const auto& binding = entries[i]->shortcut();
            if (!binding)
            {
                return cxx::unexpected(CommandFailure{
                    ECommandError::INVALID_ARGUMENT,
                    "command.shortcut",
                    static_cast<std::uint64_t>(binding.error())
                });
            }
            if (binding->key == lux::ui::EKey::NONE)
            {
                continue;
            }
            // Bounded cold validation of the effective default set, never part of event dispatch.
            for (std::size_t previous{}; previous < i; ++previous)
            {
                if (*binding == *entries[previous]->shortcut())
                {
                    return cxx::unexpected(CommandFailure{
                        ECommandError::SHORTCUT_CONFLICT,
                        "command.shortcut",
                        0,
                        std::string(entries[previous]->descriptor().id.name()) + " / " +
                            std::string(entries[i]->descriptor().id.name())
                    });
                }
            }
        }
        CommandRegistrySnapshot result;
        count(4, index->capacity() * sizeof(detail::CommandIndex));
        result.data_ = std::make_shared<Data>(std::move(entries), std::move(*index));
        return result;
    }
    std::shared_ptr<CommandEntry> CommandRegistrySnapshot::findHash(std::uint64_t hash) const noexcept
    {
        if (!data_)
        {
            return {};
        }
        count(2);
        const auto found = std::ranges::lower_bound(
            data_->index,
            hash,
            [](std::uint64_t left, std::uint64_t right)
            {
                count(3);
                return left < right;
            },
            &detail::CommandIndex::hash
        );
        if (found == data_->index.end() || found->hash != hash)
        {
            return {};
        }
        return data_->entries[found->entry];
    }
    CommandResult<CommandHandle> CommandRegistrySnapshot::find(CommandIdView id) const
    {
        auto entry = findHash(id.hash());
        if (entry)
        {
            count(1);
        }
        if (!entry || entry->descriptor().id.name() != id.name())
        {
            return failure(ECommandError::NOT_FOUND);
        }
        return CommandHandle{std::move(entry)};
    }
    CommandResult<CommandHandle> CommandRegistrySnapshot::at(std::size_t index) const
    {
        if (!data_ || index >= data_->entries.size())
        {
            return failure(ECommandError::INVALID_ARGUMENT);
        }
        return CommandHandle{data_->entries[index]};
    }
    CommandResult<CommandHandle> CommandRegistrySnapshot::resolve(const CommandHandle& original) const
    {
        if (!original.valid())
        {
            return failure(ECommandError::INVALID_ARGUMENT);
        }
        const auto& before = original.descriptor();
        auto entry = findHash(before.id.hash());
        if (!entry)
        {
            return failure(ECommandError::NOT_FOUND);
        }
        if (entry == original.entry_)
        {
            return original;
        }
        // A changed catalog may contain a different canonical name with the same hash.
        const auto& after = entry->descriptor();
        count(1);
        if (after.id.name() != before.id.name())
        {
            return failure(ECommandError::NOT_FOUND);
        }
        const bool is_incompatible = after.scope != before.scope || after.input_version != before.input_version ||
                                     after.argument_type != before.argument_type;
        if (is_incompatible)
        {
            return failure(ECommandError::INCOMPATIBLE_REGISTRATION);
        }
        return CommandHandle{std::move(entry)};
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
        bool batch_active{};
        std::size_t readers{};
        std::uint64_t revision{};
        CommandRegistrySnapshot current;
        services::ServiceRegistry* services{};
        services::ServiceScope* scope{};
        std::size_t binding_capacity{};
        struct BoundCommand final
        {
            lux::object::CodeLease code;
            std::weak_ptr<CommandEntry> entry;
            std::unique_ptr<CommandBinding> binding;
        };
        // Entry and scope identities are fixed; vector relocation never runs foreign binding destructors.
        std::vector<std::unique_ptr<BoundCommand>> bindings;
        CommandResult<void> canBeginBatch() const noexcept
        {
            if (owner != std::this_thread::get_id())
            {
                return failure(ECommandError::WRONG_THREAD);
            }
            const bool is_active = calling || dispatching || batch_active || readers != 0;
            if (is_active)
            {
                return failure(ECommandError::BUSY);
            }
            return {};
        }
        CommandResult<void> canCall() const noexcept
        {
            if (owner != std::this_thread::get_id())
            {
                return failure(ECommandError::WRONG_THREAD);
            }
            if (calling)
            {
                return failure(ECommandError::BUSY);
            }
            return {};
        }
    };
    CommandRegistry::CommandRegistry() : impl_(std::make_unique<Impl>()) {}
    CommandRegistry::CommandRegistry(
        services::ServiceRegistry& services,
        services::ServiceScope& scope,
        std::size_t binding_capacity
    )
        : CommandRegistry()
    {
        impl_->services = &services;
        impl_->scope = &scope;
        impl_->binding_capacity = binding_capacity;
        impl_->bindings.reserve(binding_capacity);
    }
    CommandRegistry::~CommandRegistry()
    {
        if (!impl_->canBeginBatch())
        {
            std::terminate();
        }
        CallScope guard{impl_->calling};
        impl_->bindings.clear();
        impl_->current = {};
    }
    CommandResult<CommandBinding*> CommandRegistry::binding(const std::shared_ptr<CommandEntry>& entry)
    {
        const auto& descriptor = entry->descriptor();
        if (!descriptor.create)
        {
            return &entry->binding_;
        }
        if (!impl_->scope)
        {
            return dependencyFailure({services::EServiceError::INVALID_SCOPE});
        }
        if (!impl_->scope->isOpen())
        {
            return failure(ECommandError::CLOSED);
        }
        for (const auto& record : impl_->bindings)
        {
            if (record->entry.lock() == entry)
            {
                return record->binding.get();
            }
        }
        auto read = impl_->services->readScope();
        if (!read)
        {
            return dependencyFailure(std::move(read.error()));
        }
        std::erase_if(impl_->bindings, [](const auto& record) { return record->entry.expired(); });
        if (impl_->bindings.size() == impl_->binding_capacity)
        {
            return failure(ECommandError::CAPACITY);
        }
        auto record = std::make_unique<Impl::BoundCommand>(entry->code_, entry, nullptr);
        CommandResult<void> outcome;
        auto construct = [&](services::ServiceResolver& resolver) -> services::ServiceResult<void>
        {
            // Candidate cleanup happens inside service admission, including a scope closed by the factory.
            auto candidate = descriptor.create(resolver);
            if (!candidate)
            {
                outcome = cxx::unexpected(std::move(candidate.error()));
            }
            else if (!resolver.isOpen())
            {
                outcome = failure(ECommandError::CLOSED);
            }
            else
            {
                const bool is_valid_binding = *candidate && (*candidate)->query && (*candidate)->execute;
                if (!is_valid_binding)
                {
                    outcome = failure(ECommandError::INVALID_ARGUMENT);
                }
                else
                {
                    record->binding = std::move(*candidate);
                }
            }
            return {};
        };
        auto resolved = impl_->services->withDependencies(*impl_->scope, descriptor.dependencies, construct);
        if (!resolved)
        {
            return dependencyFailure(std::move(resolved.error()));
        }
        if (!outcome)
        {
            return cxx::unexpected(std::move(outcome.error()));
        }
        auto* result = record->binding.get();
        impl_->bindings.push_back(std::move(record));
        return result;
    }
    CommandResult<void> CommandRegistry::canPublish() const noexcept
    {
        if (const auto ready = impl_->canBeginBatch(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        if (impl_->revision == UINT64_MAX)
        {
            return failure(ECommandError::CAPACITY);
        }
        return {};
    }
    CommandRegistry::Batch::Batch(CommandRegistry& owner, std::optional<CommandRegistrySnapshot> candidate) noexcept
        : owner_(&owner), candidate_(std::move(candidate)), publication_(candidate_.has_value())
    {
        if (publication_)
        {
            owner_->impl_->batch_active = true;
        }
        else
        {
            ++owner_->impl_->readers;
        }
    }
    CommandRegistry::Batch::Batch(Batch&& other) noexcept
        : owner_(std::exchange(other.owner_, nullptr)), candidate_(std::move(other.candidate_)),
          publication_(other.publication_)
    {
    }
    CommandRegistry::Batch::~Batch()
    {
        if (owner_ && owner_->impl_->owner != std::this_thread::get_id())
        {
            std::terminate();
        }
        // Abandoned candidate destructors run while the participating owner still rejects publication.
        candidate_.reset();
        if (owner_)
        {
            if (publication_)
            {
                owner_->impl_->batch_active = false;
            }
            else
            {
                --owner_->impl_->readers;
            }
        }
    }
    CommandRegistrySnapshot CommandRegistry::Batch::commit() noexcept
    {
        const bool is_invalid = !owner_ || !candidate_;
        if (is_invalid)
        {
            std::terminate();
        }
        if (owner_->impl_->owner != std::this_thread::get_id())
        {
            std::terminate();
        }
        auto previous = std::exchange(owner_->impl_->current, std::move(*candidate_));
        candidate_.reset();
        ++owner_->impl_->revision;
        return previous;
    }
    void CommandRegistry::Batch::clearRetained() noexcept
    {
        if (owner_ && owner_->impl_->owner != std::this_thread::get_id())
        {
            std::terminate();
        }
        candidate_.reset();
    }
    CommandResult<CommandRegistry::Batch> CommandRegistry::readBatch() noexcept
    {
        if (impl_->owner != std::this_thread::get_id())
        {
            return failure(ECommandError::WRONG_THREAD);
        }
        if (impl_->batch_active)
        {
            return failure(ECommandError::BUSY);
        }
        if (impl_->readers == SIZE_MAX)
        {
            return failure(ECommandError::CAPACITY);
        }
        return Batch{*this, std::nullopt};
    }
    CommandResult<CommandRegistry::Batch> CommandRegistry::preparePublication(CommandRegistrySnapshot candidate
    ) noexcept
    {
        if (const auto ready = canPublish(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
        return Batch{*this, std::move(candidate)};
    }
    CommandResult<CommandRegistrySnapshot> CommandRegistry::publish(CommandRegistrySnapshot candidate) noexcept
    {
        if (const auto ready = canPublish(); !ready)
        {
            return cxx::unexpected(ready.error());
        }
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
        {
            return ready;
        }
        const bool is_active = impl_->dispatching || impl_->batch_active || impl_->readers != 0;
        if (is_active)
        {
            return failure(ECommandError::BUSY);
        }
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
        {
            return cxx::unexpected(ready.error());
        }
        CallScope scope{impl_->calling};
        auto pinned = std::move(handle);
        if (!pinned.valid())
        {
            return failure(ECommandError::INVALID_ARGUMENT);
        }
        if (const auto checked = validate(pinned.descriptor(), input); !checked)
        {
            return cxx::unexpected(checked.error());
        }
        const auto target = binding(pinned.entry_);
        if (!target)
        {
            return cxx::unexpected(target.error());
        }
        if (pinned.entry_->code_.sameOwner(lux::object::CodeLease::builtin()))
        {
            return (*target)->query(input);
        }
        // Foreign callable boundary only; built-in dispatch does not pay for exception containment.
        try
        {
            return (*target)->query(input);
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
        {
            return cxx::unexpected(ready.error());
        }
        CallScope scope{impl_->calling};
        auto pinned = std::move(handle);
        if (!pinned.valid())
        {
            return failure(ECommandError::INVALID_ARGUMENT);
        }
        if (const auto checked = validate(pinned.descriptor(), input.query()); !checked)
        {
            return cxx::unexpected(checked.error());
        }
        auto invoke = [&]() -> CommandResult<DispatchReceipt>
        {
            const auto target = binding(pinned.entry_);
            if (!target)
            {
                return cxx::unexpected(target.error());
            }
            const auto state = (*target)->query(input.query());
            if (!state)
            {
                return cxx::unexpected(state.error());
            }
            if (!state->enabled)
            {
                return cxx::unexpected(CommandFailure{ECommandError::DISABLED, "command", 0, state->reason});
            }
            if (pinned.descriptor().create && !impl_->scope->isOpen())
            {
                return failure(ECommandError::CLOSED);
            }
            return (*target)->execute(input);
        };
        if (pinned.entry_->code_.sameOwner(lux::object::CodeLease::builtin()))
        {
            return invoke();
        }
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
} // namespace lux::editor::commands
