#include <algorithm>
#include <limits>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <unordered_map>
#include <unordered_set>

namespace lux::editor::desktop
{
    namespace
    {
        auto reject(EUiError code, std::string detail = {}) noexcept
        {
            return cxx::unexpected(UiFailure{code, "ui", 0, std::move(detail)});
        }
        UiFailure serviceFailure(services::ServiceFailure failure)
        {
            using enum services::EServiceError;
            const auto code = failure.code == BUSY     ? EUiError::BUSY
                              : failure.code == CLOSED ? EUiError::CLOSED
                                                       : EUiError::DEPENDENCY;
            if (failure.domain.empty())
            {
                return {code, "services", static_cast<std::uint64_t>(failure.code), std::move(failure.detail)};
            }
            return {code, std::move(failure.domain), failure.domain_code, std::move(failure.detail)};
        }
    } // namespace
    struct UiEntry::Storage final
    {
        std::vector<std::string> names;
        std::vector<services::ServiceDependency> dependencies;
        UiDescriptor descriptor;
        explicit Storage(const UiDescriptor& input) : descriptor(input)
        {
            names.reserve(2 + input.dependencies.size() * 4);
            auto name = [&](std::string_view value) -> std::string_view
            {
                names.emplace_back(value);
                return names.back();
            };
            descriptor.type = views::ViewTypeIdView{name(input.type.name())};
            descriptor.label = name(input.label);
            for (auto value : input.dependencies)
            {
                value.contract = services::ServiceNameView{name(value.contract.name())};
                if (value.implementation.isValid())
                {
                    value.implementation = services::ServiceNameView{name(value.implementation.name())};
                }
                value.qualifier = name(value.qualifier);
                value.type = {value.type.hash(), name(value.type.name())};
                dependencies.push_back(value);
            }
            descriptor.dependencies = dependencies;
        }
    };
    UiEntry::UiEntry(object::CodeLease code, const UiDescriptor& descriptor)
        : code_(std::move(code)), descriptor_(&descriptor)
    {
    }
    UiEntry::~UiEntry() = default;
    std::shared_ptr<const UiEntry> UiEntry::create(object::CodeLease code, const UiDescriptor& input)
    {
        auto entry = std::shared_ptr<UiEntry>(new UiEntry(code, input));
        entry->storage_ = std::make_unique<Storage>(input);
        entry->descriptor_ = &entry->storage_->descriptor;
        return object::pinCodeOwner(std::move(code), std::move(entry));
    }
    const UiDescriptor& UiHandle::descriptor() const noexcept
    {
        if (!entry_)
        {
            std::terminate();
        }
        return entry_->descriptor();
    }
    struct UiCatalog::Data final
    {
        struct Index final
        {
            std::uint64_t hash;
            std::size_t index;
        };
        std::vector<std::shared_ptr<const UiEntry>> entries;
        std::vector<Index> index;
        std::unordered_set<const UiEntry*> handles;
    };
    UiResult<UiCatalog> UiCatalog::prepare(std::vector<std::shared_ptr<const UiEntry>> input, std::size_t capacity)
    {
        if (input.size() > capacity)
        {
            return reject(EUiError::CAPACITY);
        }
        auto data = std::make_shared<Data>();
        data->entries = std::move(input);
        data->index.reserve(data->entries.size());
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
        for (std::size_t i{}; i < data->entries.size(); ++i)
        {
            const auto& entry = data->entries[i];
            const bool invalid_entry = !entry || !entry->code().valid();
            if (invalid_entry)
            {
                return reject(EUiError::INVALID_DESCRIPTOR);
            }
            const auto& descriptor = entry->descriptor();
            const bool invalid_identity = !descriptor.type.isValid() || descriptor.label.empty();
            const bool invalid_factory = !descriptor.schema || !descriptor.create;
            if (invalid_identity || invalid_factory)
            {
                return reject(EUiError::INVALID_DESCRIPTOR);
            }
            for (const auto& dependency : descriptor.dependencies)
            {
                const bool invalid_dependency = !dependency.contract.isValid() || !dependency.version ||
                                                !dependency.type.isValid() ||
                                                dependency.kind > services::EDependencyKind::BORROWED ||
                                                dependency.scope > services::EDependencyScope::ROOT;
                if (invalid_dependency)
                {
                    return reject(EUiError::INVALID_DESCRIPTOR, "Invalid declared UI dependency");
                }
                const bool has_collision = !check_name(dependency.contract) || !check_name(dependency.implementation);
                if (has_collision)
                {
                    return reject(EUiError::HASH_COLLISION, "Conflicting declared UI dependency names");
                }
            }
            data->index.push_back({descriptor.type.hash(), i});
            data->handles.insert(entry.get());
        }
        std::ranges::sort(data->index, {}, &Data::Index::hash);
        for (std::size_t i = 1; i < data->index.size(); ++i)
        {
            if (data->index[i - 1].hash != data->index[i].hash)
            {
                continue;
            }
            const bool duplicate = data->entries[data->index[i - 1].index]->descriptor().type.name() ==
                                   data->entries[data->index[i].index]->descriptor().type.name();
            return reject(duplicate ? EUiError::DUPLICATE : EUiError::HASH_COLLISION);
        }
        UiCatalog result;
        result.data_ = std::move(data);
        return result;
    }
    UiResult<UiHandle> UiCatalog::find(views::ViewTypeIdView type) const noexcept
    {
        if (!data_)
        {
            return reject(EUiError::NOT_FOUND);
        }
        const auto found = std::ranges::lower_bound(data_->index, type.hash(), {}, &Data::Index::hash);
        const bool missing = found == data_->index.end() || found->hash != type.hash();
        if (missing)
        {
            return reject(EUiError::NOT_FOUND);
        }
        const auto& entry = data_->entries[found->index];
        if (entry->descriptor().type.name() != type.name())
        {
            return reject(EUiError::HASH_COLLISION);
        }
        return at(found->index);
    }
    UiResult<UiHandle> UiCatalog::at(std::size_t index) const noexcept
    {
        const bool missing = !data_ || index >= data_->entries.size();
        if (missing)
        {
            return reject(EUiError::NOT_FOUND);
        }
        UiHandle result;
        result.entry_ = data_->entries[index];
        return result;
    }
    std::span<const std::shared_ptr<const UiEntry>> UiCatalog::entries() const noexcept
    {
        return data_ ? std::span<const std::shared_ptr<const UiEntry>>{data_->entries}
                     : std::span<const std::shared_ptr<const UiEntry>>{};
    }
    struct UiRegistry::Impl final
    {
        object::ObjectDispatcherRef dispatcher;
        services::ServiceRegistry& services;
        UiCatalog current;
        std::uint64_t revision{};
        bool active{};
        bool catalog_reading{};
        UiResult<void> admission() const noexcept
        {
            if (!dispatcher.isCurrent())
            {
                return reject(EUiError::WRONG_THREAD);
            }
            if (active)
            {
                return reject(EUiError::BUSY);
            }
            return {};
        }
        struct Guard final
        {
            bool& active;
            bool owns{true};
            explicit Guard(bool& active) : active(active)
            {
                active = true;
            }
            ~Guard()
            {
                if (owns)
                {
                    active = false;
                }
            }
            void transfer() noexcept
            {
                owns = false;
            }
            Guard(const Guard&) = delete;
            Guard& operator=(const Guard&) = delete;
        };
    };
    UiRegistry::ReadScope::ReadScope(UiRegistry& owner) noexcept : owner_(&owner)
    {
        owner_->impl_->catalog_reading = true;
    }
    UiRegistry::ReadScope::ReadScope(ReadScope&& other) noexcept : owner_(std::exchange(other.owner_, nullptr)) {}
    UiRegistry::ReadScope::~ReadScope()
    {
        if (!owner_)
        {
            return;
        }
        if (!owner_->impl_->dispatcher.isCurrent())
        {
            std::terminate();
        }
        owner_->impl_->catalog_reading = false;
    }
    UiResult<UiRegistry::ReadScope> UiRegistry::readScope() noexcept
    {
        if (auto admitted = impl_->admission(); !admitted)
        {
            return cxx::unexpected(std::move(admitted.error()));
        }
        if (impl_->catalog_reading)
        {
            return reject(EUiError::BUSY);
        }
        return ReadScope{*this};
    }
    struct UiRegistry::Publication::State final
    {
        Impl& owner;
        UiCatalog candidate;
        bool committed{};
        State(Impl& owner, UiCatalog candidate) : owner(owner), candidate(std::move(candidate)) {}
        ~State()
        {
            if (!owner.dispatcher.isCurrent())
            {
                std::terminate();
            }
            candidate = {};
            owner.active = false;
        }
    };
    UiRegistry::Publication::Publication(std::unique_ptr<State> state) noexcept : state_(std::move(state)) {}
    UiRegistry::Publication::~Publication() = default;
    UiRegistry::Publication::Publication(Publication&&) noexcept = default;
    void UiRegistry::Publication::clearRetained() noexcept
    {
        if (!state_)
        {
            return;
        }
        if (!state_->owner.dispatcher.isCurrent())
        {
            std::terminate();
        }
        state_->committed = true;
        state_->candidate = {};
    }
    void UiRegistry::Publication::commit() noexcept
    {
        const bool invalid = !state_ || state_->committed;
        if (invalid)
        {
            std::terminate();
        }
        if (!state_->owner.dispatcher.isCurrent())
        {
            std::terminate();
        }
        std::swap(state_->owner.current, state_->candidate);
        ++state_->owner.revision;
        state_->committed = true;
    }
    UiRegistry::UiRegistry(object::ObjectDispatcherRef dispatcher, services::ServiceRegistry& services)
        : impl_(std::make_unique<Impl>(std::move(dispatcher), services))
    {
        if (!impl_->dispatcher.isCurrent())
        {
            std::terminate();
        }
    }
    UiRegistry::~UiRegistry()
    {
        if (!impl_->admission() || impl_->catalog_reading)
        {
            std::terminate();
        }
        Impl::Guard guard{impl_->active};
        impl_->current = {};
    }
    UiResult<UiRegistry::Publication> UiRegistry::preparePublication(UiCatalog input) noexcept
    {
        if (auto admission = impl_->admission(); !admission)
        {
            return cxx::unexpected(std::move(admission.error()));
        }
        Impl::Guard guard{impl_->active};
        auto candidate = std::move(input);
        if (impl_->catalog_reading)
        {
            return reject(EUiError::BUSY);
        }
        if (!candidate.data_)
        {
            return reject(EUiError::INVALID_DESCRIPTOR);
        }
        if (impl_->revision == (std::numeric_limits<std::uint64_t>::max)())
        {
            return reject(EUiError::CAPACITY);
        }
        auto state = std::make_unique<Publication::State>(*impl_, std::move(candidate));
        // The state continues the existing protection; no callback can run during the scalar handoff.
        guard.transfer();
        return Publication{std::move(state)};
    }
    UiResult<void> UiRegistry::publish(UiCatalog candidate) noexcept
    {
        auto publication = preparePublication(std::move(candidate));
        if (!publication)
        {
            return cxx::unexpected(std::move(publication.error()));
        }
        publication->commit();
        return {};
    }
    UiCatalog UiRegistry::snapshot() const noexcept
    {
        return impl_->current;
    }
    std::uint64_t UiRegistry::revision() const noexcept
    {
        return impl_->revision;
    }
    UiResult<std::unique_ptr<lux::ui::Pane, object::ObjectDeleter>> UiRegistry::create(
        const UiHandle& handle,
        services::ServiceScope& scope,
        const UiCreateInfo& input
    ) noexcept
    {
        if (auto admission = impl_->admission(); !admission)
        {
            return cxx::unexpected(std::move(admission.error()));
        }
        Impl::Guard guard{impl_->active};
        const auto entry = handle.entry_;
        const bool missing = !entry || !impl_->current.data_ || !impl_->current.data_->handles.contains(entry.get());
        if (missing)
        {
            return reject(EUiError::STALE_REGISTRATION);
        }
        auto fixed = input;
        const bool wrong_identity = !fixed.instance.isValid() || fixed.dispatcher != impl_->dispatcher;
        const bool wrong_configuration =
            fixed.configuration.schema != entry->descriptor().schema || !fixed.content.valid();
        if (wrong_identity || wrong_configuration)
        {
            return reject(EUiError::INVALID_CONFIGURATION);
        }
        using Owner = std::unique_ptr<lux::ui::Pane, object::ObjectDeleter>;
        UiResult<Owner> result = reject(EUiError::FACTORY_FAILURE);
        auto factory = [&](services::ServiceResolver& resolver) -> services::ServiceResult<void>
        {
            const auto& descriptor = entry->descriptor();
            if (descriptor.validate)
            {
                auto valid = descriptor.validate(fixed.configuration.bytes);
                if (!valid)
                {
                    result = cxx::unexpected(std::move(valid.error()));
                    return {};
                }
            }
            if (!resolver.isOpen())
            {
                result = reject(EUiError::CLOSED);
                return {};
            }
            auto created = descriptor.create(resolver, fixed);
            if (!created)
            {
                result = cxx::unexpected(std::move(created.error()));
                return {};
            }
            if (!resolver.isOpen())
            {
                result = reject(EUiError::CLOSED);
                return {};
            }
            const bool null_output = !*created;
            if (null_output)
            {
                result = reject(EUiError::INVALID_OUTPUT);
                return {};
            }
            const auto& pane = **created;
            const bool wrong_output_identity =
                pane.id().name() != fixed.instance.name() || pane.type().name() != descriptor.type.name();
            const bool mounted = pane.parent() || pane.attachedRoot();
            const bool wrong_dispatcher = pane.dispatcherRef() != impl_->dispatcher;
            if (wrong_output_identity || mounted || wrong_dispatcher)
            {
                result = reject(EUiError::INVALID_OUTPUT);
                return {};
            }
            auto deleter =
                object::ObjectDeleter::create<lux::ui::Pane>(std::default_delete<lux::ui::Pane>{}, entry->code());
            result = Owner{created->release(), std::move(deleter)};
            return {};
        };
        // The open foreign factory boundary is the only exception containment, outside UI hot paths.
        auto invoke = [&](services::ServiceResolver& resolver) -> services::ServiceResult<void>
        {
            try
            {
                return factory(resolver);
            }
            catch (const std::bad_alloc&)
            {
                std::terminate();
            }
            catch (...)
            {
                result = reject(EUiError::FACTORY_FAILURE, "UI factory threw");
                return {};
            }
        };
        auto resolved = impl_->services.withDependencies(scope, entry->descriptor().dependencies, invoke);
        if (!resolved)
        {
            return cxx::unexpected(serviceFailure(std::move(resolved.error())));
        }
        return result;
    }
} // namespace lux::editor::desktop
