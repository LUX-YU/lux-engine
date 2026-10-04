#include <algorithm>
#include <atomic>
#include <exception>
#include <limits>
#include <lux/engine/object/LuxObject.hpp>
#include <lux/engine/services/ServiceRegistry.hpp>
#include <thread>
#include <unordered_map>

namespace lux::services
{
    namespace
    {
        [[nodiscard]] auto reject(EServiceError code, std::string detail = {}) noexcept
        {
            return cxx::unexpected(ServiceFailure{code, std::move(detail)});
        }
        std::atomic<std::uint64_t> next_registry{};
        struct Callbacks final
        {
            std::thread::id owner{std::this_thread::get_id()};
            std::size_t depth{};
        };
        class CallbackScope final
        {
        public:
            explicit CallbackScope(Callbacks& callbacks) noexcept : callbacks_(callbacks)
            {
                if (callbacks_.owner == std::this_thread::get_id())
                {
                    ++callbacks_.depth;
                }
            }
            ~CallbackScope()
            {
                if (callbacks_.owner == std::this_thread::get_id())
                {
                    --callbacks_.depth;
                }
            }
            CallbackScope(const CallbackScope&) = delete;
            CallbackScope& operator=(const CallbackScope&) = delete;

        private:
            Callbacks& callbacks_;
        };
        struct Lifetime final
        {
            std::atomic<bool> reclaimed{};
        };
        struct Borrow final
        {
            ServiceName contract;
            cxx::TypeToken type;
            void* pointer{};
            std::uint32_t version{};
            std::string qualifier;
        };
        enum class EScopeStage : std::uint8_t
        {
            OPEN,
            REVIEW,
            CLOSED
        };
    } // namespace
    namespace detail
    {
        struct ServiceDefinition final
        {
            std::uint64_t registry{}, generation{};
            std::shared_ptr<const ServiceEntry> entry;
        };
        struct ServiceScopeState final
        {
            std::uint64_t registry{}, generation{};
            EScopeStage stage{EScopeStage::OPEN};
            bool handle_alive{true};
            std::shared_ptr<ServiceScopeState> parent;
            std::vector<std::weak_ptr<ServiceScopeState>> children;
            std::vector<Borrow> borrowed;
            std::vector<std::shared_ptr<void>> retained;
            std::vector<std::weak_ptr<Lifetime>> instances;

            void release() noexcept
            {
                stage = EScopeStage::CLOSED;
                retained.clear();
            }

            [[nodiscard]] bool open() const noexcept
            {
                for (auto* scope = this; scope; scope = scope->parent.get())
                {
                    if (scope->stage != EScopeStage::OPEN)
                    {
                        return false;
                    }
                }
                return true;
            }

            void prune() noexcept
            {
                std::erase_if(children, [](const auto& child) { return child.expired(); });
                std::erase_if(
                    instances,
                    [](const auto& weak)
                    {
                        const auto instance = weak.lock();
                        return !instance || instance->reclaimed.load(std::memory_order_acquire);
                    }
                );
            }

            [[nodiscard]] bool drained() const noexcept
            {
                for (const auto& weak : instances)
                {
                    if (auto value = weak.lock(); value && !value->reclaimed.load(std::memory_order_acquire))
                    {
                        return false;
                    }
                }
                for (const auto& weak : children)
                {
                    if (auto value = weak.lock(); value && !value->drained())
                    {
                        return false;
                    }
                }
                return true;
            }
        };
    } // namespace detail
    struct ServiceEntry::Storage final
    {
        std::vector<std::string> names;
        std::vector<ServiceContract> contracts;
        std::vector<ServiceDependency> dependencies;
        ServiceDescriptor descriptor;

        explicit Storage(const ServiceDescriptor& input) : descriptor(input)
        {
            names.reserve(5 + input.contracts.size() * 2 + input.dependencies.size() * 4);
            auto name = [&](std::string_view value) -> std::string_view
            {
                names.emplace_back(value);
                return names.back();
            };
            descriptor.implementation = ServiceNameView{name(input.implementation.name())};
            descriptor.configuration.id = input.configuration.id.isValid()
                                              ? ServiceNameView{name(input.configuration.id.name())}
                                              : ServiceNameView{};
            descriptor.allocation_type = {input.allocation_type.hash(), name(input.allocation_type.name())};
            descriptor.configuration.type = {input.configuration.type.hash(), name(input.configuration.type.name())};
            descriptor.definition_type = {input.definition_type.hash(), name(input.definition_type.name())};
            for (auto value : input.contracts)
            {
                value.id = ServiceNameView{name(value.id.name())};
                value.type = {value.type.hash(), name(value.type.name())};
                contracts.push_back(value);
            }
            for (auto value : input.dependencies)
            {
                value.contract = ServiceNameView{name(value.contract.name())};
                value.implementation = value.implementation.isValid()
                                           ? ServiceNameView{name(value.implementation.name())}
                                           : ServiceNameView{};
                value.qualifier = name(value.qualifier);
                value.type = {value.type.hash(), name(value.type.name())};
                dependencies.push_back(value);
            }
            descriptor.contracts = contracts;
            descriptor.dependencies = dependencies;
        }
    };
    ServiceEntry::ServiceEntry(object::CodeLease code, const ServiceDescriptor& descriptor)
        : code_(std::move(code)), descriptor_(&descriptor)
    {
    }
    ServiceEntry::~ServiceEntry() = default;
    std::shared_ptr<const ServiceEntry> ServiceEntry::create(
        object::CodeLease code,
        const ServiceDescriptor& descriptor
    )
    {
        return create(std::move(code), descriptor, {}, {});
    }
    std::shared_ptr<const ServiceEntry> ServiceEntry::create(
        object::CodeLease code,
        const ServiceDescriptor& descriptor,
        cxx::TypeToken definition_type,
        std::shared_ptr<const void> definition
    )
    {
        auto entry = std::shared_ptr<ServiceEntry>(new ServiceEntry(std::move(code), descriptor));
        entry->storage_ = std::make_unique<Storage>(descriptor);
        entry->descriptor_ = &entry->storage_->descriptor;
        entry->definition_type_ = definition_type;
        entry->definition_ = std::move(definition);
        return entry;
    }
    ServiceResult<std::shared_ptr<const void>> ServiceEntry::definition(cxx::TypeToken type) const noexcept
    {
        const bool is_type_mismatch = type.hash() != definition_type_.hash() || type.name() != definition_type_.name();
        if (is_type_mismatch)
        {
            return reject(EServiceError::TYPE_MISMATCH);
        }
        if (!definition_)
        {
            return reject(EServiceError::INVALID_DESCRIPTOR);
        }
        return definition_;
    }
    const ServiceDescriptor& ServiceHandle::descriptor() const noexcept
    {
        if (!definition_)
        {
            std::terminate();
        }
        return definition_->entry->descriptor();
    }
    namespace
    {
        struct Instance final
        {
            std::shared_ptr<const detail::ServiceDefinition> definition;
            std::shared_ptr<detail::ServiceScopeState> scope;
            std::string qualifier;
            ServiceConfiguration configuration;
            std::shared_ptr<Lifetime> lifetime{std::make_shared<Lifetime>()};
            std::weak_ptr<void> allocation;
            std::vector<void*> projections;
            bool creating{true};
        };
        // Compiled in this host provider, never in a plugin. Clearing a weak control block cannot
        // call into unloaded code. Both destruction and its code-owner cleanup stay guarded.
        struct ServiceDelete final
        {
            std::shared_ptr<Callbacks> callbacks;
            std::shared_ptr<const ServiceEntry> entry;
            std::shared_ptr<Lifetime> lifetime;
            void* allocation{};

            void operator()(void*) noexcept
            {
                CallbackScope callback{*callbacks};
                entry->descriptor().destroy(allocation);
                allocation = nullptr;
                entry.reset();
                lifetime->reclaimed.store(true, std::memory_order_release);
            }
        };
        [[nodiscard]] bool sameConfiguration(const ServiceConfiguration& a, const ServiceConfiguration& b) noexcept
        {
            const bool same_schema = a.schema == b.schema && a.version == b.version && a.type == b.type;
            return same_schema && std::ranges::equal(a.bytes.view(), b.bytes.view());
        }
    } // namespace
    struct ServiceRegistry::Impl final
    {
        object::ObjectDispatcherRef dispatcher;
        ServiceLimits limits;
        std::shared_ptr<Callbacks> callbacks{std::make_shared<Callbacks>()};
        std::uint64_t domain{}, next_definition{}, next_scope{};
        std::vector<std::shared_ptr<const detail::ServiceDefinition>> definitions;
        std::vector<std::shared_ptr<detail::ServiceScopeState>> scopes;
        std::vector<std::unique_ptr<Instance>> instances;
        std::size_t catalog_readers{};

        [[nodiscard]] ServiceResult<void> admission() const noexcept
        {
            if (callbacks->owner != std::this_thread::get_id())
            {
                return reject(EServiceError::WRONG_THREAD);
            }
            if (callbacks->depth)
            {
                return reject(EServiceError::BUSY);
            }
            return {};
        }
        void prune() noexcept
        {
            CallbackScope callback{*callbacks};
            std::erase_if(
                instances,
                [](const auto& value) { return value->lifetime->reclaimed.load(std::memory_order_acquire); }
            );
            std::erase_if(
                scopes,
                [](const auto& scope)
                {
                    scope->prune();
                    return !scope->handle_alive && scope->stage == EScopeStage::CLOSED && scope->drained();
                }
            );
        }
    };
    ServiceRegistry::ServiceRegistry(object::ObjectDispatcherRef dispatcher, ServiceLimits limits)
        : impl_(std::make_unique<Impl>())
    {
        if (!dispatcher.isCurrent())
        {
            std::terminate();
        }
        auto issued = next_registry.load(std::memory_order_relaxed);
        do
        {
            if (issued == (std::numeric_limits<std::uint64_t>::max)())
            {
                std::terminate();
            }
        } while (!next_registry.compare_exchange_weak(issued, issued + 1, std::memory_order_relaxed));
        impl_->domain = issued + 1;
        impl_->dispatcher = std::move(dispatcher);
        impl_->limits = limits;
        impl_->definitions.reserve(limits.definitions);
        impl_->scopes.reserve(limits.scopes);
        impl_->instances.reserve(limits.instances);
    }
    ServiceRegistry::~ServiceRegistry()
    {
        // Borrowed infrastructure and the code provider must outlive every accepted allocation.
        const bool is_invalid_destruction = !impl_->admission() || impl_->catalog_readers || !drained();
        if (is_invalid_destruction)
        {
            std::terminate();
        }
        if (std::ranges::any_of(impl_->scopes, [](const auto& scope) { return scope->handle_alive; }))
        {
            std::terminate();
        }
        CallbackScope callback{*impl_->callbacks};
        impl_->instances.clear();
        impl_->scopes.clear();
        impl_->definitions.clear();
    }
    bool ServiceRegistry::drained() const noexcept
    {
        if (impl_->callbacks->owner != std::this_thread::get_id())
        {
            return false;
        }
        return std::ranges::all_of(
            impl_->instances,
            [](const auto& value) { return value->lifetime->reclaimed.load(std::memory_order_acquire); }
        );
    }
    ServiceRegistry::ReadScope::ReadScope(ServiceRegistry& owner) noexcept : owner_(&owner)
    {
        ++owner_->impl_->catalog_readers;
    }
    ServiceRegistry::ReadScope::ReadScope(ReadScope&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
    ServiceRegistry::ReadScope::~ReadScope()
    {
        if (!owner_)
        {
            return;
        }
        if (owner_->impl_->callbacks->owner != std::this_thread::get_id())
        {
            std::terminate();
        }
        if (owner_->impl_->catalog_readers == 0)
        {
            std::terminate();
        }
        --owner_->impl_->catalog_readers;
    }
    ServiceResult<ServiceRegistry::ReadScope> ServiceRegistry::readScope() noexcept
    {
        if (auto admitted = impl_->admission(); !admitted)
        {
            return cxx::unexpected(std::move(admitted.error()));
        }
        if (impl_->catalog_readers == (std::numeric_limits<std::size_t>::max)())
        {
            return reject(EServiceError::CAPACITY);
        }
        return ReadScope{*this};
    }
    struct ServiceRegistry::Publication::State final
    {
        ServiceRegistry& owner;
        std::vector<std::shared_ptr<const detail::ServiceDefinition>> candidate;
        bool committed{};
        State(ServiceRegistry& owner, std::vector<std::shared_ptr<const detail::ServiceDefinition>> candidate)
            : owner(owner), candidate(std::move(candidate))
        {
            ++owner.impl_->callbacks->depth;
        }
        ~State()
        {
            if (owner.impl_->callbacks->owner != std::this_thread::get_id())
            {
                std::terminate();
            }
            candidate.clear(); // Plugin cleanup stays inside the original participant guard.
            --owner.impl_->callbacks->depth;
        }
    };
    ServiceRegistry::Publication::Publication(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
    ServiceRegistry::Publication::~Publication() = default;
    ServiceRegistry::Publication::Publication(Publication&&) noexcept = default;
    void ServiceRegistry::Publication::clearRetained() noexcept
    {
        if (!state_)
        {
            return;
        }
        if (state_->owner.impl_->callbacks->owner != std::this_thread::get_id())
        {
            std::terminate();
        }
        state_->committed = true;
        state_->candidate.clear();
    }
    void ServiceRegistry::Publication::commit() noexcept
    {
        const bool invalid = !state_ || state_->committed;
        if (invalid)
        {
            std::terminate();
        }
        if (state_->owner.impl_->callbacks->owner != std::this_thread::get_id())
        {
            std::terminate();
        }
        state_->owner.impl_->definitions.swap(state_->candidate);
        state_->committed = true;
    }
    ServiceResult<void> ServiceRegistry::publish(std::vector<std::shared_ptr<const ServiceEntry>> input) noexcept
    {
        auto publication = preparePublication(std::move(input));
        if (!publication)
        {
            return cxx::unexpected(std::move(publication.error()));
        }
        publication->commit();
        return {};
    }
    ServiceResult<void> validateServiceEntries(
        std::span<const std::shared_ptr<const ServiceEntry>> entries,
        std::size_t capacity
    ) noexcept
    {
        if (entries.size() > capacity)
        {
            return reject(EServiceError::CAPACITY);
        }
        std::unordered_map<std::uint64_t, std::string_view> names;
        auto check_name = [&](ServiceNameView name) -> bool
        {
            if (!name.isValid())
            {
                return true;
            }
            auto [found, inserted] = names.emplace(name.hash(), name.name());
            return inserted || found->second == name.name();
        };
        for (std::size_t i{}; i < entries.size(); ++i)
        {
            const bool is_invalid_entry = !entries[i] || !entries[i]->code().valid();
            if (is_invalid_entry)
            {
                return reject(EServiceError::INVALID_DESCRIPTOR);
            }
            const auto& descriptor = entries[i]->descriptor();
            const auto& entry = *entries[i];
            const bool has_definition = bool(entry.definition_);
            const bool has_definition_type = descriptor.definition_type.isValid();
            const bool is_definition_mismatch = has_definition != has_definition_type ||
                descriptor.definition_type.hash() != entry.definition_type_.hash() ||
                descriptor.definition_type.name() != entry.definition_type_.name();
            if (is_definition_mismatch)
            {
                return reject(EServiceError::INVALID_DESCRIPTOR, "Declaration input does not match its descriptor");
            }
            const bool invalid_identity = !descriptor.implementation.isValid() || descriptor.version == 0 ||
                                          !descriptor.allocation_type.isValid();
            const bool invalid_factory = !descriptor.create || !descriptor.destroy || descriptor.contracts.empty();
            const bool invalid_object = descriptor.object && descriptor.affinity != EServiceAffinity::OWNER;
            const bool invalid_policy =
                (descriptor.retention != EServiceRetention::SHARED && descriptor.retention != EServiceRetention::SCOPED
                ) ||
                (descriptor.affinity != EServiceAffinity::NONE && descriptor.affinity != EServiceAffinity::OWNER);
            const bool invalid_descriptor = invalid_identity || invalid_factory || invalid_object || invalid_policy;
            if (invalid_descriptor)
            {
                return reject(EServiceError::INVALID_DESCRIPTOR);
            }
            const bool has_descriptor_collision =
                !check_name(descriptor.implementation) || !check_name(descriptor.configuration.id);
            if (has_descriptor_collision)
            {
                return reject(EServiceError::HASH_COLLISION);
            }
            for (std::size_t j{}; j < i; ++j)
            {
                if (entries[j]->descriptor().implementation == descriptor.implementation)
                {
                    return reject(EServiceError::DUPLICATE);
                }
            }
            const bool invalid_schema =
                descriptor.configuration.id.isValid()
                    ? descriptor.configuration.version == 0 || !descriptor.configuration.type.isValid() ||
                          !descriptor.configuration.validate
                    : descriptor.configuration.version != 0 || descriptor.configuration.type.isValid() ||
                          descriptor.configuration.validate;
            if (invalid_schema)
            {
                return reject(EServiceError::INVALID_DESCRIPTOR);
            }
            for (std::size_t j{}; j < descriptor.contracts.size(); ++j)
            {
                const auto& contract = descriptor.contracts[j];
                const bool invalid_contract =
                    !contract.id.isValid() || contract.version == 0 || !contract.type.isValid() || !contract.project;
                if (invalid_contract)
                {
                    return reject(EServiceError::INVALID_DESCRIPTOR);
                }
                if (!check_name(contract.id))
                {
                    return reject(EServiceError::HASH_COLLISION);
                }
                for (std::size_t k{}; k < j; ++k)
                {
                    if (descriptor.contracts[k].id == contract.id)
                    {
                        return reject(EServiceError::DUPLICATE);
                    }
                }
                for (std::size_t k{}; k < i; ++k)
                {
                    for (const auto& other : entries[k]->descriptor().contracts)
                    {
                        const bool is_contract_mismatch =
                            other.id == contract.id && other.version == contract.version && other.type != contract.type;
                        if (is_contract_mismatch)
                        {
                            return reject(EServiceError::TYPE_MISMATCH);
                        }
                    }
                }
            }
            for (const auto& dependency : descriptor.dependencies)
            {
                const bool invalid_dependency =
                    !dependency.contract.isValid() || dependency.version == 0 || !dependency.type.isValid() ||
                    dependency.kind > EDependencyKind::BORROWED || dependency.scope > EDependencyScope::ROOT;
                if (invalid_dependency)
                {
                    return reject(EServiceError::INVALID_DESCRIPTOR);
                }
                const bool has_dependency_collision =
                    !check_name(dependency.contract) || !check_name(dependency.implementation);
                if (has_dependency_collision)
                {
                    return reject(EServiceError::HASH_COLLISION);
                }
            }
        }
        return {};
    }
    ServiceResult<ServiceRegistry::Publication> ServiceRegistry::preparePublication(
        std::vector<std::shared_ptr<const ServiceEntry>> input
    ) noexcept
    {
        auto admitted = impl_->admission();
        if (!admitted)
        {
            return cxx::unexpected(std::move(admitted.error()));
        }
        CallbackScope callback{*impl_->callbacks};
        auto entries = std::move(input); // Rejected descriptor/code cleanup is still protected.
        if (impl_->catalog_readers)
        {
            return reject(EServiceError::BUSY);
        }
        if (auto valid = validateServiceEntries(entries, impl_->limits.definitions); !valid)
        {
            return cxx::unexpected(std::move(valid.error()));
        }
        std::vector<std::shared_ptr<const detail::ServiceDefinition>> candidate;
        candidate.reserve(entries.size());
        for (auto& entry : entries)
        {
            auto found =
                std::ranges::find_if(impl_->definitions, [&](const auto& value) { return value->entry == entry; });
            if (found != impl_->definitions.end())
            {
                candidate.push_back(*found);
            }
            else
            {
                if (impl_->next_definition == (std::numeric_limits<std::uint64_t>::max)())
                {
                    return reject(EServiceError::CAPACITY);
                }
                candidate.push_back(
                    std::make_shared<const detail::ServiceDefinition>(impl_->domain, ++impl_->next_definition, entry)
                );
            }
        }
        return Publication{std::make_unique<Publication::State>(*this, std::move(candidate))};
    }
    ServiceResult<ServiceHandle> ServiceRegistry::resolve(
        cxx::TypeToken type,
        ServiceNameView contract,
        std::uint32_t version,
        ServiceNameView implementation
    ) const noexcept
    {
        if (impl_->callbacks->owner != std::this_thread::get_id())
        {
            return reject(EServiceError::WRONG_THREAD);
        }
        ServiceHandle result;
        bool name_found{}, version_found{};
        for (const auto& definition : impl_->definitions)
        {
            const auto& descriptor = definition->entry->descriptor();
            const bool is_other_implementation =
                implementation.isValid() && descriptor.implementation != implementation;
            if (is_other_implementation)
            {
                continue;
            }
            for (std::size_t index{}; index < descriptor.contracts.size(); ++index)
            {
                const auto& offered = descriptor.contracts[index];
                if (contract.isValid() ? offered.id != contract : offered.type != type)
                {
                    continue;
                }
                name_found = true;
                if (offered.version != version)
                {
                    continue;
                }
                version_found = true;
                if (offered.type != type)
                {
                    continue;
                }
                if (result.valid())
                {
                    return reject(EServiceError::AMBIGUOUS_PROVIDER);
                }
                result.definition_ = definition;
                result.contract_ = index;
            }
        }
        if (result.valid())
        {
            return result;
        }
        return reject(
            version_found ? EServiceError::TYPE_MISMATCH
            : name_found  ? EServiceError::VERSION_MISMATCH
                          : EServiceError::NOT_FOUND
        );
    }
    ServiceResult<ServiceScope> ServiceRegistry::createScope(const ServiceScope* parent) noexcept
    {
        if (auto admitted = impl_->admission(); !admitted)
        {
            return cxx::unexpected(std::move(admitted.error()));
        }
        if (parent)
        {
            const bool invalid_parent = parent->registry_ != this || !parent->state_;
            if (invalid_parent)
            {
                return reject(EServiceError::INVALID_SCOPE);
            }
            if (!parent->state_->open())
            {
                return reject(EServiceError::CLOSED);
            }
        }
        auto parent_state = parent ? parent->state_ : std::shared_ptr<detail::ServiceScopeState>{};
        impl_->prune(); // Old code-owner cleanup may destroy the caller's lexical parent handle.
        if (parent_state && !parent_state->open())
        {
            return reject(EServiceError::CLOSED);
        }
        const bool exhausted = impl_->scopes.size() >= impl_->limits.scopes ||
                               impl_->next_scope == (std::numeric_limits<std::uint64_t>::max)();
        if (exhausted)
        {
            return reject(EServiceError::CAPACITY);
        }
        auto scope = std::make_shared<detail::ServiceScopeState>();
        scope->registry = impl_->domain;
        scope->generation = ++impl_->next_scope;
        if (parent_state)
        {
            scope->parent = parent_state;
            parent_state->children.push_back(scope);
        }
        impl_->scopes.push_back(scope);
        return ServiceScope{*this, std::move(scope)};
    }
    ServiceResult<std::shared_ptr<void>> ServiceRegistry::get(
        const ServiceHandle& handle,
        ServiceScope& scope,
        cxx::TypeToken type,
        std::string_view qualifier,
        const ServiceConfiguration& configuration
    ) noexcept
    {
        if (auto admitted = impl_->admission(); !admitted)
        {
            return cxx::unexpected(std::move(admitted.error()));
        }
        const bool invalid_handle = !handle.valid() || handle.definition_->registry != impl_->domain;
        if (invalid_handle)
        {
            return reject(EServiceError::INVALID_DESCRIPTOR);
        }
        if (handle.descriptor().contracts[handle.contract_].type != type)
        {
            return reject(EServiceError::TYPE_MISMATCH);
        }
        const bool invalid_scope = scope.registry_ != this || !scope.state_;
        if (invalid_scope)
        {
            return reject(EServiceError::INVALID_SCOPE);
        }
        return instantiate(handle, scope.state_, qualifier, configuration);
    }
    ServiceResult<std::shared_ptr<void>> ServiceRegistry::instantiate(
        ServiceHandle handle,
        std::shared_ptr<detail::ServiceScopeState> scope,
        std::string_view qualifier,
        const ServiceConfiguration& configuration
    ) noexcept
    {
        if (!scope->open())
        {
            return reject(EServiceError::CLOSED);
        }
        for (const auto& value : impl_->instances)
        {
            const bool matches = value->definition->generation == handle.definition_->generation &&
                                 value->scope->generation == scope->generation && value->qualifier == qualifier;
            const bool is_reclaimed = value->lifetime->reclaimed.load(std::memory_order_acquire);
            const bool is_other_instance = !matches || is_reclaimed;
            if (is_other_instance)
            {
                continue;
            }
            if (value->creating)
            {
                return reject(EServiceError::DEPENDENCY_CYCLE);
            }
            if (!sameConfiguration(value->configuration, configuration))
            {
                return reject(EServiceError::CONFIGURATION_MISMATCH);
            }
            auto owner = value->allocation.lock();
            if (!owner)
            {
                return reject(EServiceError::RETIRING);
            }
            return std::shared_ptr<void>(std::move(owner), value->projections[handle.contract_]);
        }
        const auto& descriptor = handle.descriptor();
        const auto& schema = descriptor.configuration;
        const bool schema_matches = schema.id == configuration.schema.view() &&
                                    schema.version == configuration.version && schema.type == configuration.type;
        const bool unexpected_bytes = !schema.id.isValid() && !configuration.bytes.empty();
        const bool is_invalid_configuration = !schema_matches || unexpected_bytes;
        if (is_invalid_configuration)
        {
            return reject(EServiceError::INVALID_CONFIGURATION);
        }
        // Warm hits above cannot invoke cleanup. Cold construction fixes all borrowed input before
        // pruning releases code owners; TypeToken then refers to the pinned definition.
        std::string fixed_qualifier{qualifier};
        auto fixed_configuration = configuration;
        fixed_configuration.type = schema.type;
        impl_->prune();
        if (!scope->open())
        {
            return reject(EServiceError::CLOSED);
        }
        if (impl_->instances.size() >= impl_->limits.instances)
        {
            return reject(EServiceError::CAPACITY);
        }
        auto record = std::make_unique<Instance>();
        record->definition = handle.definition_;
        record->scope = scope;
        record->qualifier = std::move(fixed_qualifier);
        record->configuration = std::move(fixed_configuration);
        auto* active = record.get();
        impl_->instances.push_back(std::move(record));
        CallbackScope callback{*impl_->callbacks};
        struct Creation final
        {
            Instance& instance;
            ~Creation()
            {
                if (instance.creating)
                {
                    instance.lifetime->reclaimed.store(true, std::memory_order_release);
                }
            }
        } creation{*active};
        if (schema.validate)
        {
            if (auto valid = schema.validate(active->configuration); !valid)
            {
                return cxx::unexpected(std::move(valid.error()));
            }
        }
        if (!scope->open())
        {
            return reject(EServiceError::CLOSED);
        }
        ServiceResolver resolver{*this, descriptor.dependencies, scope, handle.definition_->entry.get()};
        auto created = descriptor.create(resolver, active->configuration);
        if (!created)
        {
            return cxx::unexpected(std::move(created.error()));
        }
        if (!*created)
        {
            return reject(EServiceError::FACTORY_FAILURE, "Factory returned a null allocation");
        }
        ServiceDelete destroy{impl_->callbacks, handle.definition_->entry, active->lifetime, *created};
        std::unique_ptr<void, ServiceDelete> candidate(*created, std::move(destroy));
        if (!scope->open())
        {
            return reject(EServiceError::CLOSED);
        }
        for (const auto& contract : descriptor.contracts)
        {
            auto* projected = contract.project(candidate.get());
            if (!projected)
            {
                return reject(EServiceError::TYPE_MISMATCH, "A declared allocation projection returned null");
            }
            active->projections.push_back(projected);
        }
        // Factory/projection callbacks can surrender a lexical scope. Protect the returned owner first,
        // then recheck admission before publishing; no mutable instance may enter an already closed scope.
        auto* projected_object = descriptor.object ? descriptor.object(candidate.get()) : nullptr;
        if (descriptor.object && !projected_object)
        {
            return reject(EServiceError::TYPE_MISMATCH);
        }
        if (!scope->open())
        {
            return reject(EServiceError::CLOSED);
        }
        std::shared_ptr<void> owner;
        if (descriptor.affinity == EServiceAffinity::OWNER)
        {
            if (descriptor.object)
            {
                std::unique_ptr<object::LuxObject, ServiceDelete> typed(
                    projected_object,
                    std::move(candidate.get_deleter())
                );
                auto* allocation = candidate.release();
                auto shared =
                    object::shareOnDispatcher(impl_->dispatcher, std::move(typed), handle.definition_->entry->code());
                if (!shared)
                {
                    return reject(EServiceError::FACTORY_FAILURE, "Object ownership or dispatcher refused sharing");
                }
                owner = std::shared_ptr<void>(std::move(*shared), allocation);
            }
            else
            {
                auto shared = object::shareOnDispatcher(
                    impl_->dispatcher,
                    std::move(candidate),
                    handle.definition_->entry->code()
                );
                if (!shared)
                {
                    return reject(EServiceError::FACTORY_FAILURE, "Dispatcher refused sharing");
                }
                owner = std::move(*shared);
            }
        }
        else
        {
            owner = std::shared_ptr<void>(std::move(candidate));
        }
        active->allocation = owner;
        active->creating = false;
        if (descriptor.retention == EServiceRetention::SCOPED)
        {
            scope->retained.push_back(owner);
        }
        scope->instances.push_back(active->lifetime);
        return std::shared_ptr<void>(std::move(owner), active->projections[handle.contract_]);
    }
    ServiceScope::ServiceScope(ServiceRegistry& registry, std::shared_ptr<detail::ServiceScopeState> state) noexcept
        : registry_(&registry), state_(std::move(state))
    {
    }
    ServiceScope::ServiceScope(ServiceScope&& other) noexcept
        : registry_(std::exchange(other.registry_, nullptr)), state_(std::move(other.state_))
    {
    }
    ServiceScope::~ServiceScope()
    {
        if (!registry_)
        {
            return;
        }
        auto& callbacks = *registry_->impl_->callbacks;
        if (callbacks.owner != std::this_thread::get_id())
        {
            std::terminate();
        }
        // Releasing an already-owned scope is cleanup, not admission of new work. Nested destruction
        // keeps the outer callback protection active and cannot reopen a closed scope.
        CallbackScope callback{callbacks};
        state_->release();
        state_->handle_alive = false;
    }
    ServiceResult<void> ServiceScope::beginClose() noexcept
    {
        if (!registry_)
        {
            return reject(EServiceError::INVALID_SCOPE);
        }
        if (auto admitted = registry_->impl_->admission(); !admitted)
        {
            return admitted;
        }
        if (!state_->open())
        {
            return reject(EServiceError::CLOSED);
        }
        state_->stage = EScopeStage::REVIEW;
        return {};
    }
    ServiceResult<void> ServiceScope::cancelClose() noexcept
    {
        if (!registry_)
        {
            return reject(EServiceError::INVALID_SCOPE);
        }
        if (auto admitted = registry_->impl_->admission(); !admitted)
        {
            return admitted;
        }
        if (state_->stage != EScopeStage::REVIEW)
        {
            return reject(EServiceError::CLOSED);
        }
        state_->stage = EScopeStage::OPEN;
        return {};
    }
    ServiceResult<void> ServiceScope::release() noexcept
    {
        if (!registry_)
        {
            return reject(EServiceError::INVALID_SCOPE);
        }
        if (auto admitted = registry_->impl_->admission(); !admitted)
        {
            return admitted;
        }
        CallbackScope callback{*registry_->impl_->callbacks};
        // Existing children cannot create after an ancestor closes. Their independent retentions remain
        // until their own domain close is committed; parent drained() includes those allocations.
        state_->release();
        return {};
    }
    bool ServiceScope::drained() const noexcept
    {
        return registry_ && registry_->impl_->callbacks->owner == std::this_thread::get_id() && state_->drained();
    }
    ServiceResult<void> ServiceScope::provide(
        ServiceNameView contract,
        cxx::TypeToken type,
        void* pointer,
        std::uint32_t version,
        std::string_view qualifier
    ) noexcept
    {
        if (!registry_)
        {
            return reject(EServiceError::INVALID_SCOPE);
        }
        if (auto admitted = registry_->impl_->admission(); !admitted)
        {
            return admitted;
        }
        const bool invalid = !contract.isValid() || !type.isValid() || !pointer || !version;
        if (invalid)
        {
            return reject(EServiceError::INVALID_DESCRIPTOR);
        }
        if (!state_->open())
        {
            return reject(EServiceError::CLOSED);
        }
        for (const auto& existing : state_->borrowed)
        {
            const bool has_name_collision =
                existing.contract.hash() == contract.hash() && existing.contract.name() != contract.name();
            if (has_name_collision)
            {
                return reject(EServiceError::HASH_COLLISION);
            }
            const bool is_duplicate = existing.contract.view() == contract && existing.qualifier == qualifier;
            if (is_duplicate)
            {
                return reject(EServiceError::DUPLICATE);
            }
        }
        if (state_->borrowed.size() >= registry_->impl_->limits.borrowed_per_scope)
        {
            return reject(EServiceError::CAPACITY);
        }
        state_->borrowed.push_back({ServiceName{contract.name()}, type, pointer, version, std::string{qualifier}});
        return {};
    }
    ServiceResolver::ServiceResolver(
        ServiceRegistry& registry,
        std::span<const ServiceDependency> dependencies,
        std::shared_ptr<detail::ServiceScopeState> scope,
        const ServiceEntry* entry
    ) noexcept
        : registry_(registry), dependencies_(dependencies), scope_(std::move(scope)), entry_(entry)
    {
    }
    bool ServiceResolver::isOpen() const noexcept
    {
        return registry_.impl_->callbacks->owner == std::this_thread::get_id() && scope_->open();
    }
    ServiceResult<void> ServiceRegistry::withDependencies(
        ServiceScope& scope,
        std::span<const ServiceDependency> dependencies,
        cxx::function_ref<ServiceResult<void>(ServiceResolver&)> invoke
    ) noexcept
    {
        if (auto admitted = impl_->admission(); !admitted)
        {
            return admitted;
        }
        const bool invalid_scope = scope.registry_ != this || !scope.state_;
        if (invalid_scope)
        {
            return reject(EServiceError::INVALID_SCOPE);
        }
        auto pinned_scope = scope.state_;
        if (!pinned_scope->open())
        {
            return reject(EServiceError::CLOSED);
        }
        CallbackScope callback{*impl_->callbacks};
        ServiceResolver resolver{*this, dependencies, std::move(pinned_scope)};
        return invoke(resolver);
    }
    const object::ObjectDispatcherRef& ServiceResolver::dispatcher() const noexcept
    {
        return registry_.impl_->dispatcher;
    }
    ServiceResult<std::shared_ptr<detail::ServiceScopeState>> ServiceRegistry::dependencyScope(
        const ServiceResolver& resolver,
        const ServiceDependency& dependency
    ) const noexcept
    {
        auto scope = resolver.scope_;
        if (dependency.scope == EDependencyScope::PARENT)
        {
            scope = scope->parent;
        }
        else if (dependency.scope == EDependencyScope::ROOT)
        {
            while (auto parent = scope->parent)
            {
                scope = std::move(parent);
            }
        }
        if (!scope)
        {
            return reject(EServiceError::INVALID_SCOPE_DEPENDENCY);
        }
        return scope;
    }
    ServiceResult<std::shared_ptr<void>> ServiceResolver::get(
        std::size_t index,
        cxx::TypeToken type,
        const ServiceConfiguration& configuration
    ) noexcept
    {
        if (!isOpen())
        {
            return reject(EServiceError::CLOSED);
        }
        const auto dependencies = dependencies_;
        if (index >= dependencies.size())
        {
            return reject(EServiceError::UNDECLARED_DEPENDENCY);
        }
        const auto& dependency = dependencies[index];
        const bool invalid = dependency.kind != EDependencyKind::SHARED || dependency.type != type;
        if (invalid)
        {
            return reject(EServiceError::UNDECLARED_DEPENDENCY);
        }
        auto scope = registry_.dependencyScope(*this, dependency);
        if (!scope)
        {
            return cxx::unexpected(std::move(scope.error()));
        }
        auto handle = registry_.resolve(type, dependency.contract, dependency.version, dependency.implementation);
        if (!handle)
        {
            const bool is_optional_absence = dependency.optional && handle.error().code == EServiceError::NOT_FOUND;
            if (is_optional_absence)
            {
                return std::shared_ptr<void>{};
            }
            return cxx::unexpected(std::move(handle.error()));
        }
        return registry_.instantiate(*handle, std::move(*scope), dependency.qualifier, configuration);
    }
    ServiceResult<void*> ServiceResolver::require(std::size_t index, cxx::TypeToken type) noexcept
    {
        if (!isOpen())
        {
            return reject(EServiceError::CLOSED);
        }
        const auto dependencies = dependencies_;
        if (index >= dependencies.size())
        {
            return reject(EServiceError::UNDECLARED_DEPENDENCY);
        }
        const auto& dependency = dependencies[index];
        const bool invalid = dependency.kind != EDependencyKind::BORROWED || dependency.type != type;
        if (invalid)
        {
            return reject(EServiceError::UNDECLARED_DEPENDENCY);
        }
        auto scope = registry_.dependencyScope(*this, dependency);
        if (!scope)
        {
            return cxx::unexpected(std::move(scope.error()));
        }
        for (const auto& borrowed : (*scope)->borrowed)
        {
            const bool matches =
                borrowed.contract.view() == dependency.contract && borrowed.qualifier == dependency.qualifier;
            if (!matches)
            {
                continue;
            }
            if (borrowed.version != dependency.version)
            {
                return reject(EServiceError::VERSION_MISMATCH);
            }
            if (borrowed.type != type)
            {
                return reject(EServiceError::TYPE_MISMATCH);
            }
            return borrowed.pointer;
        }
        return reject(EServiceError::NOT_FOUND);
    }
} // namespace lux::services
