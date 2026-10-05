#include <algorithm>
#include <limits>
#include <lux/engine/editor/desktop/ContentRouting.hpp>
#include <lux/engine/editor/desktop/UiRegistry.hpp>
#include <lux/engine/editor/workspace/LayoutPlan.hpp>
#include <lux/engine/editor/workbench/DockLayoutMapping.hpp>
#include <lux/engine/ui/Root.hpp>
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
        auto attachmentFailure(lux::ui::EAttachmentError cause) noexcept
        {
            const auto code = cause == lux::ui::EAttachmentError::BUSY           ? EUiError::BUSY
                              : cause == lux::ui::EAttachmentError::WRONG_THREAD ? EUiError::WRONG_THREAD
                                                                                : EUiError::ATTACHMENT;
            return cxx::unexpected(UiFailure{code, "ui.attachment", static_cast<std::uint64_t>(cause), {}});
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
        std::vector<sessions::SessionKindIdView> content_kinds;
        UiDescriptor descriptor;
        explicit Storage(const UiDescriptor& input) : descriptor(input)
        {
            names.reserve(2 + input.dependencies.size() * 4 + input.content_kinds.size());
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
            content_kinds.reserve(input.content_kinds.size());
            for (const auto kind : input.content_kinds)
            {
                content_kinds.emplace_back(name(kind.name()));
            }
            descriptor.content_kinds = content_kinds;
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
        std::unordered_map<std::string, std::vector<std::size_t>> content;
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
            std::unordered_set<std::string_view> kinds;
            for (const auto kind : descriptor.content_kinds)
            {
                const bool invalid_kind = !kind.isValid() || kind.hash() != cxx::Fnv1a64::hash(kind.name()) ||
                                          !kinds.insert(kind.name()).second;
                if (invalid_kind)
                {
                    return reject(EUiError::INVALID_DESCRIPTOR, "Invalid or repeated content kind");
                }
                data->content[std::string{kind.name()}].push_back(i);
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
    UiResult<UiHandle> UiCatalog::selectContent(
        const sessions::SessionKindId& kind,
        std::optional<views::ViewTypeId> preferred
    ) const
    {
        if (!data_)
        {
            return reject(EUiError::NOT_FOUND);
        }
        const auto found = data_->content.find(kind.name);
        if (found == data_->content.end())
        {
            return reject(EUiError::NOT_FOUND);
        }
        auto selected = detail::selectContent(entries(), found->second, preferred);
        if (!selected)
        {
            return cxx::unexpected(UiFailure{
                selected.error().code == detail::EContentSelectionError::AMBIGUOUS ? EUiError::AMBIGUOUS
                                                                                  : EUiError::NOT_FOUND,
                "view.content",
                0,
                std::move(selected.error().candidates)
            });
        }
        return at(*selected);
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
        struct Output final
        {
            std::shared_ptr<const UiEntry> declaration;
            object::ObjectIdentity identity;
            views::ViewRestoreKey restore_key;
        };
        // Only the standard candidate deleter retains this metadata. The registry observes it weakly;
        // it never owns a Pane, retains a model, or creates another instance ID/retirement queue.
        std::vector<std::weak_ptr<const Output>> outputs;
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
        return createImpl(handle, scope, input);
    }
    UiResult<std::unique_ptr<lux::ui::Pane, object::ObjectDeleter>> UiRegistry::createImpl(
        const UiHandle& handle,
        services::ServiceScope& scope,
        const UiCreateInfo& input
    ) noexcept
    {
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
        const bool wrong_restore_key = fixed.restore_key && !fixed.restore_key->isValid();
        if (wrong_identity || wrong_configuration || wrong_restore_key)
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
            std::erase_if(impl_->outputs, [](const auto& output) { return output.expired(); });
            auto output = std::make_shared<const Impl::Output>(
                entry, pane.identity(), fixed.restore_key.value_or(views::ViewRestoreKey{pane.id().name()})
            );
            auto destroy = [output](lux::ui::Pane* pane) noexcept { delete pane; };
            auto deleter = object::ObjectDeleter::create<lux::ui::Pane>(std::move(destroy), entry->code());
            impl_->outputs.push_back(output);
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
    UiResult<void> UiRegistry::visit(
        lux::ui::Root& root,
        const lux::ui::PaneHandle& handle,
        cxx::function_ref<void(const std::shared_ptr<const UiEntry>&, lux::ui::Pane&)> callback
    ) noexcept
    {
        if (auto admitted = impl_->admission(); !admitted)
        {
            return cxx::unexpected(std::move(admitted.error()));
        }
        Impl::Guard guard{impl_->active};
        return visitAdmitted(root, handle, callback);
    }
    UiResult<void> UiRegistry::visitAdmitted(
        lux::ui::Root& root,
        const lux::ui::PaneHandle& handle,
        cxx::function_ref<void(const std::shared_ptr<const UiEntry>&, lux::ui::Pane&)> callback
    ) noexcept
    {
        UiResult<void> result = reject(EUiError::NOT_FOUND, "Pane was not created by this registry");
        auto borrow = [&](lux::ui::Pane& pane)
        {
            const auto identity = pane.identity();
            for (const auto& weak : impl_->outputs)
            {
                auto output = weak.lock();
                if (output && output->identity == identity)
                {
                    // Registered extension callbacks are a foreign boundary, never the draw/update hot path.
                    try
                    {
                        callback(output->declaration, pane);
                        result = {};
                    }
                    catch (const std::bad_alloc&)
                    {
                        std::terminate();
                    }
                    catch (...)
                    {
                        result = reject(EUiError::FACTORY_FAILURE, "UI operation threw");
                    }
                    return;
                }
            }
        };
        auto visited = root.withPane(handle, borrow);
        if (!visited)
        {
            return attachmentFailure(visited.error());
        }
        return result;
    }
    UiResult<std::vector<WindowInfo>> UiRegistry::describe(lux::ui::Root& root) noexcept
    {
        if (auto admitted = impl_->admission(); !admitted)
            return cxx::unexpected(std::move(admitted.error()));
        Impl::Guard guard{impl_->active};
        return describeAdmitted(root);
    }
    UiResult<std::vector<WindowInfo>> UiRegistry::describeAdmitted(lux::ui::Root& root) noexcept
    {
        if (root.dispatcherRef() != impl_->dispatcher)
            return reject(EUiError::WRONG_THREAD);
        const auto revision = root.windowRevision();
        // Fix original handles before any extension content callback; no raw pointer crosses a callback.
        std::vector<lux::ui::PaneHandle> handles;
        for (auto* pane : root.panes())
        {
            if (!pane)
                continue;
            const auto identity = pane->identity();
            const bool registered = std::ranges::any_of(impl_->outputs, [&](const auto& weak)
            {
                const auto output = weak.lock();
                return output && output->identity == identity;
            });
            if (!registered)
                continue;
            auto handle = root.identify(*pane);
            if (!handle)
                return attachmentFailure(handle.error());
            handles.push_back(std::move(*handle));
        }
        std::vector<WindowInfo> result;
        result.reserve(handles.size());
        for (const auto& handle : handles)
        {
            auto capture = [&](const std::shared_ptr<const UiEntry>& entry, lux::ui::Pane& pane)
            {
                const auto identity = pane.identity();
                // visitAdmitted established this exact output; no callback intervenes in the lookup.
                const auto found = std::ranges::find_if(impl_->outputs, [&](const auto& weak)
                {
                    const auto output = weak.lock();
                    return output && output->identity == identity;
                });
                const auto output = found->lock();
                const auto& descriptor = entry->descriptor();
                result.push_back({
                    handle, pane.id(), pane.type(), output->restore_key, std::string{pane.title()},
                    pane.visible(), pane.focused(), descriptor.content ? descriptor.content(pane) : views::ViewContent{}
                });
            };
            auto visited = visitAdmitted(root, handle, capture);
            if (!visited)
                return cxx::unexpected(std::move(visited.error()));
            if (root.windowRevision() != revision)
                return reject(EUiError::STALE_ROOT, "Window set changed during description");
        }
        return result;
    }
    UiResult<lux::ui::AttachmentCommit> UiRegistry::applyLayout(
        lux::ui::Root& root, services::ServiceScope& scope, workspace::DockLayout input
    ) noexcept
    {
        if (auto admitted = impl_->admission(); !admitted)
            return cxx::unexpected(std::move(admitted.error()));
        Impl::Guard guard{impl_->active};
        const auto revision = root.windowRevision();
        auto layout = workspace::ValidatedLayout::validate(std::move(input));
        if (!layout)
            return cxx::unexpected(UiFailure{
                EUiError::INVALID_CONFIGURATION, "layout", static_cast<std::uint64_t>(layout.error().code),
                std::move(layout.error().detail)
            });
        auto services = impl_->services.readScope();
        if (!services)
            return cxx::unexpected(serviceFailure(std::move(services.error())));
        auto live = describeAdmitted(root);
        if (!live)
            return cxx::unexpected(std::move(live.error()));
        std::vector<workspace::LayoutTarget> targets;
        targets.reserve(live->size());
        for (const auto& window : *live)
            targets.push_back({window.restore_key, window.type});
        std::vector<workspace::ViewProviderInfo> providers;
        const auto catalog = snapshot();
        for (const auto& entry : catalog.entries())
        {
            const auto& descriptor = entry->descriptor();
            providers.push_back({views::ViewTypeId{descriptor.type.name()}, descriptor.schema, descriptor.schema});
        }
        auto plan = workspace::LayoutPlanner::resolve(*layout, targets, providers);
        if (!plan)
            return cxx::unexpected(UiFailure{
                EUiError::INVALID_CONFIGURATION, "layout", static_cast<std::uint64_t>(plan.error().code),
                std::move(plan.error().detail)
            });
        std::vector<UiMountRequest> candidates;
        std::vector<UiStateRequest> states;
        std::map<std::uint32_t, std::string> windows;
        for (const auto& planned : plan->views)
        {
            const auto& slot = planned.slot;
            if (planned.existing)
            {
                const auto& window = (*live)[*planned.existing];
                states.push_back({window.handle, slot.state, slot.visible});
                windows.emplace(slot.id.value, std::string{window.instance.name()});
                continue;
            }
            if (planned.resolution != workspace::ELayoutResolution::CREATE_UNBOUND)
                return reject(EUiError::INVALID_CONFIGURATION, "Required window provider or schema unavailable");
            auto factory = catalog.find(slot.type.view());
            if (!factory)
                return cxx::unexpected(std::move(factory.error()));
            auto name = std::string{"layout/"} + std::string{slot.type.name()} + "/" +
                        std::string{slot.restore_key.name()};
            windows.emplace(slot.id.value, name);
            candidates.push_back({
                std::move(*factory),
                {impl_->dispatcher, lux::ui::PaneId{name}, {}, slot.state, slot.restore_key}, slot.visible
            });
        }
        auto docking = workbench::detail::makeDockTree(plan->layout.dock, windows);
        if (root.windowRevision() != revision)
            return reject(EUiError::STALE_ROOT, "Window set changed during layout preparation");
        return mountAdmitted(root, scope, std::move(candidates), std::move(docking), std::move(states));
    }

    UiResult<views::ViewContent> UiRegistry::content(lux::ui::Root& root, const lux::ui::PaneHandle& handle) noexcept
    {
        views::ViewContent result;
        auto capture = [&](const std::shared_ptr<const UiEntry>& entry, lux::ui::Pane& pane)
        {
            const auto& descriptor = entry->descriptor();
            if (descriptor.content)
            {
                result = descriptor.content(pane);
            }
        };
        auto visited = visit(root, handle, capture);
        if (!visited)
        {
            return cxx::unexpected(std::move(visited.error()));
        }
        return result;
    }
    UiResult<void> UiRegistry::rebind(
        lux::ui::Root& root, const lux::ui::PaneHandle& handle, const views::ViewContent& content
    ) noexcept
    {
        // Capture before an extension callback; a caller may mutate its own input in that callback.
        const auto candidate = content;
        UiResult<void> result;
        auto bind = [&](const std::shared_ptr<const UiEntry>& entry, lux::ui::Pane& pane)
        {
            const auto& descriptor = entry->descriptor();
            if (!candidate.valid())
            {
                result = reject(EUiError::INVALID_CONFIGURATION, "Invalid content association");
                return;
            }
            if (descriptor.rebind)
            {
                result = descriptor.rebind(pane, candidate);
            }
            else
            {
                const auto current = descriptor.content ? descriptor.content(pane) : views::ViewContent{};
                if (candidate != current)
                {
                    result = reject(EUiError::OPERATION_FAILURE, "This factory does not support content binding");
                }
            }
        };
        auto visited = visit(root, handle, bind);
        if (!visited)
        {
            return cxx::unexpected(std::move(visited.error()));
        }
        return result;
    }
    UiResult<lux::ui::PreparedAttachment>
    UiRegistry::prepareClose(lux::ui::Root& root, std::span<const lux::ui::PaneHandle> input) noexcept
    {
        if (auto admitted = impl_->admission(); !admitted)
        {
            return cxx::unexpected(std::move(admitted.error()));
        }
        Impl::Guard guard{impl_->active};
        // Callback code can modify the caller's container. Keep the original identities, not raw
        // pointers across callbacks. Root prepares all topology/capacity checks before domain cleanup.
        const std::vector handles(input.begin(), input.end());
        std::vector<lux::ui::Pane*> panes;
        panes.reserve(handles.size());
        for (const auto& handle : handles)
        {
            auto pane = root.findPane(handle);
            if (!pane)
            {
                return attachmentFailure(pane.error());
            }
            const auto identity = (*pane)->identity();
            const bool is_known = std::ranges::any_of(impl_->outputs, [&](const auto& weak)
            {
                const auto output = weak.lock();
                return output && output->identity == identity;
            });
            if (!is_known)
            {
                return reject(EUiError::NOT_FOUND, "Pane was not created by this registry");
            }
            panes.push_back(*pane);
        }
        auto prepared = root.prepareDetach(panes);
        if (!prepared)
        {
            return attachmentFailure(prepared.error());
        }
        const auto revision = root.windowRevision();
        UiResult<void> result;
        auto prepare = [&](const std::shared_ptr<const UiEntry>& entry, lux::ui::Pane& pane)
        {
            const auto& descriptor = entry->descriptor();
            if (descriptor.prepare_close)
            {
                result = descriptor.prepare_close(pane);
            }
        };
        for (const auto& handle : handles)
        {
            auto visited = visitAdmitted(root, handle, prepare);
            if (!visited)
            {
                return cxx::unexpected(std::move(visited.error()));
            }
            if (!result)
            {
                return cxx::unexpected(std::move(result.error()));
            }
            // An external owner can disappear in another window's callback. Do not pass its raw
            // address to a later callback or report a valid preparation after that structural change.
            if (root.windowRevision() != revision)
            {
                return attachmentFailure(lux::ui::EAttachmentError::STALE_PREPARATION);
            }
        }
        return std::move(*prepared);
    }
    UiResult<workspace::VersionedViewState>
    UiRegistry::captureState(lux::ui::Root& root, const lux::ui::PaneHandle& handle) noexcept
    {
        UiResult<workspace::VersionedViewState> result{workspace::VersionedViewState{}};
        auto capture = [&](const std::shared_ptr<const UiEntry>& entry, lux::ui::Pane& pane)
        {
            const auto& descriptor = entry->descriptor();
            if (descriptor.capture_state)
            {
                result = descriptor.capture_state(pane);
            }
        };
        auto visited = visit(root, handle, capture);
        if (!visited)
        {
            return cxx::unexpected(std::move(visited.error()));
        }
        return result;
    }
    UiResult<lux::ui::AttachmentCommit> UiRegistry::mount(
        lux::ui::Root& root,
        services::ServiceScope& scope,
        std::vector<UiMountRequest> input,
        std::optional<lux::ui::DockTree> docking,
        std::vector<UiStateRequest> state_input
    ) noexcept
    {
        if (auto admission = impl_->admission(); !admission)
        {
            return cxx::unexpected(std::move(admission.error()));
        }
        Impl::Guard guard{impl_->active};
        // Input cleanup stays under UI admission even when service admission fails. On success,
        // declaration order also keeps the service read alive until every input owner is gone.
        std::optional<services::ServiceRegistry::ReadScope> services;
        auto requests = std::move(input);
        auto state_requests = std::move(state_input);
        auto acquired = impl_->services.readScope();
        if (!acquired)
        {
            return cxx::unexpected(serviceFailure(std::move(acquired.error())));
        }
        services.emplace(std::move(*acquired));
        return mountAdmitted(root, scope, std::move(requests), std::move(docking), std::move(state_requests));
    }
    UiResult<lux::ui::AttachmentCommit> UiRegistry::mountAdmitted(
        lux::ui::Root& root,
        services::ServiceScope& scope,
        std::vector<UiMountRequest> requests,
        std::optional<lux::ui::DockTree> docking,
        std::vector<UiStateRequest> state_requests
    ) noexcept
    {
        // All foreign input/candidate cleanup precedes both guards. Never borrow a mutable
        // caller vector or fill a new source stamp after a callback.
        if (root.dispatcherRef() != impl_->dispatcher)
        {
            return reject(EUiError::WRONG_THREAD);
        }
        const auto revision = root.windowRevision();
        std::unordered_set<std::string_view> names;
        names.reserve(requests.size());
        for (const auto& request : requests)
        {
            const bool duplicate =
                !names.insert(request.input.instance.name()).second || root.findPane(request.input.instance.view());
            if (duplicate)
            {
                return reject(EUiError::DUPLICATE, std::string{request.input.instance.name()});
            }
        }
        std::optional<lux::ui::PreparedDockTree> prepared_docking;
        if (docking)
        {
            auto prepared = root.prepareDockTree(std::move(*docking));
            if (!prepared)
            {
                return cxx::unexpected(UiFailure{
                    EUiError::INVALID_CONFIGURATION,
                    "ui.docking",
                    static_cast<std::uint64_t>(prepared.error()),
                    {}
                });
            }
            prepared_docking.emplace(std::move(*prepared));
        }
        std::vector<std::unique_ptr<lux::ui::Pane, object::ObjectDeleter>> owners;
        std::vector<lux::ui::WindowVisibility> visibility;
        struct State final
        {
            std::shared_ptr<const UiEntry> declaration;
            cxx::move_only_function<void()> apply;
        };
        std::vector<State> states;
        states.reserve(state_requests.size());
        owners.reserve(requests.size());
        visibility.reserve(requests.size() + state_requests.size());
        // Reject the whole input topology before any provider prepares or cancels an interaction.
        std::vector<lux::ui::Pane*> existing;
        existing.reserve(state_requests.size());
        for (const auto& request : state_requests)
        {
            auto found = root.findPane(request.target);
            if (!found)
            {
                return attachmentFailure(found.error());
            }
            if (std::ranges::find(existing, *found) != existing.end())
            {
                return reject(EUiError::DUPLICATE, "Repeated state target");
            }
            existing.push_back(*found);
        }
        for (const auto& request : state_requests)
        {
            UiResult<void> result;
            auto prepare = [&](const std::shared_ptr<const UiEntry>& entry, lux::ui::Pane& pane)
            {
                const auto& descriptor = entry->descriptor();
                const auto& configuration = request.configuration;
                if (configuration.schema != descriptor.schema)
                {
                    result = reject(EUiError::INVALID_CONFIGURATION, "Unsupported state schema");
                    return;
                }
                if (descriptor.validate)
                {
                    result = descriptor.validate(configuration.bytes);
                    if (!result)
                    {
                        return;
                    }
                }
                cxx::move_only_function<void()> apply;
                if (descriptor.prepare_state)
                {
                    auto prepared = descriptor.prepare_state(pane, configuration);
                    if (!prepared)
                    {
                        result = cxx::unexpected(std::move(prepared.error()));
                        return;
                    }
                    apply = std::move(*prepared);
                }
                else if (!configuration.bytes.empty())
                {
                    result = reject(EUiError::INVALID_CONFIGURATION, "This factory has no configurable state");
                    return;
                }
                states.push_back({entry, std::move(apply)});
                if (request.visible)
                {
                    visibility.push_back({&pane, *request.visible});
                }
            };
            auto visited = visitAdmitted(root, request.target, prepare);
            if (!visited)
            {
                return cxx::unexpected(std::move(visited.error()));
            }
            if (!result)
            {
                return cxx::unexpected(std::move(result.error()));
            }
        }
        for (const auto& request : requests)
        {
            auto created = createImpl(request.factory, scope, request.input);
            if (!created)
            {
                return cxx::unexpected(std::move(created.error()));
            }
            visibility.push_back({created->get(), request.visible});
            owners.push_back(std::move(*created));
        }
        if (root.windowRevision() != revision)
        {
            // Preserve any legitimate callback change; never replace its state with an older layout.
            return reject(EUiError::STALE_ROOT);
        }
        std::vector<lux::ui::Pane*> panes;
        panes.reserve(owners.size());
        for (const auto& owner : owners)
        {
            panes.push_back(owner.get());
        }
        auto prepared = root.prepareMount(panes, visibility);
        if (!prepared)
        {
            return attachmentFailure(prepared.error());
        }
        // Capacity, providers and all value preparations have succeeded. End overlays using the
        // original domain gates; no source, binding, or persisted configuration is applied here.
        for (const auto& request : state_requests)
        {
            UiResult<void> result;
            auto cancel = [&](const std::shared_ptr<const UiEntry>& entry, lux::ui::Pane& pane)
            {
                if (entry->descriptor().cancel_preview)
                {
                    result = entry->descriptor().cancel_preview(pane);
                }
            };
            auto visited = visitAdmitted(root, request.target, cancel);
            if (!visited)
            {
                return cxx::unexpected(std::move(visited.error()));
            }
            if (!result)
            {
                return cxx::unexpected(std::move(result.error()));
            }
        }
        auto apply = [&]() noexcept
        {
            for (auto& state : states)
            {
                if (state.apply)
                {
                    state.apply();
                }
            }
            if (prepared_docking)
            {
                root.commitDockTree(std::move(*prepared_docking));
            }
        };
        auto committed = root.commit(*prepared, owners, apply);
        if (!committed)
        {
            return attachmentFailure(committed.error());
        }
        return std::move(*committed);
    }
} // namespace lux::editor::desktop
