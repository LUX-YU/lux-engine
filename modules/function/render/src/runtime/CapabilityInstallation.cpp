#include <lux/engine/render/runtime/RenderServerImpl.hpp>
#include <ranges>

namespace lux::render
{
    RenderResult<const ConfigValue*> RenderServer::Impl::qualifyConfiguration(
        const FeatureEntry& feature,
        const VCapabilityConfiguration& input,
        ConfigValue& decoded
    ) const noexcept
    {
        const auto* value = std::get_if<ConfigValue>(&input);
        if (!value)
        {
            const auto& serialized = std::get<SerializedRenderConfiguration>(input);
            const auto& contract = feature.capabilities->configuration;
            const bool invalid_schema = serialized.schema != contract.schema || serialized.version != contract.version;
            const bool invalid_size = serialized.bytes.size() > config.max_config_bytes;
            if (invalid_schema || invalid_size)
            {
                return cxx::unexpected(RenderError{kInvalidConfig});
            }
            if (feature.graph.decode_configuration)
            {
                auto result = feature.graph.decode_configuration(serialized.bytes);
                if (!result)
                {
                    return cxx::unexpected(result.error());
                }
                decoded = std::move(*result);
            }
            else if (feature.config_type.isValid() || !serialized.bytes.empty())
            {
                return cxx::unexpected(RenderError{kInvalidConfig});
            }
            value = &decoded;
        }
        const bool invalid_type = feature.config_type != value->type();
        const bool invalid_size = value->retainedBytes() > config.max_config_bytes;
        if (invalid_type || invalid_size)
        {
            return cxx::unexpected(RenderError{kInvalidConfig});
        }
        return value;
    }

    RenderResult<StorageSet*> RenderServer::Impl::findScope(const VRenderScope& scope) noexcept
    {
        return std::visit(
            [&](const auto& location) noexcept -> RenderResult<StorageSet*>
            {
                using T = std::remove_cvref_t<decltype(location)>;
                if constexpr (std::same_as<T, GlobalRenderScope>)
                {
                    return &*global_storages;
                }
                else if constexpr (std::same_as<T, SceneId>)
                {
                    if (auto* scene = scenes.find(location))
                    {
                        return &scene->storages;
                    }
                }
                else if constexpr (std::same_as<T, ViewRequest>)
                {
                    auto* scene = scenes.find(location.scene);
                    auto* view = scene ? scene->views.find(location.view) : nullptr;
                    if (view)
                    {
                        return &view->storages;
                    }
                }
                else
                {
                    if (auto* attachment = attachments.find(location))
                    {
                        return attachment->storages.get();
                    }
                }
                return cxx::unexpected(RenderError{kRuntimeIdentity});
            },
            scope
        );
    }

    RenderResult<StorageId> RenderServer::Impl::ensureStorage(
        StorageSet& set,
        StorageTypeId type,
        std::string_view key,
        const ConfigValue& configuration,
        std::vector<std::pair<StorageSet*, StorageId>>& created
    ) noexcept
    {
        const auto previous = set.find(type, key);
        auto dependencies = storageContext();
        auto instance = set.getOrCreate(dependencies, type, key, configuration);
        if (instance && previous.isNull())
        {
            created.emplace_back(&set, *instance);
        }
        return instance;
    }

    void RenderServer::Impl::listCapabilities(
        const ListCapabilities::Request& input,
        ListCapabilities::Response& result
    ) noexcept
    {
        for (const auto& entry : features)
        {
            if (!entry.capabilities || (!input.implementation.empty() && input.implementation != entry.name))
            {
                continue;
            }
            const auto& support = *entry.capabilities;
            const bool matches =
                input.capability.empty() ||
                std::ranges::any_of(
                    support.capabilities,
                    [&](const auto& capability) noexcept
                    { return capability.name == input.capability && capability.version == input.version; }
                );
            if (!matches)
            {
                continue;
            }
            auto& published = result.implementations.emplace_back(support);
            published.selection = CapabilitySelection{entry.id};
            for (const auto& declaration : entry.graph.storage_inputs)
            {
                if (!resources->factory(declaration.storage_type))
                {
                    published.unavailable = RenderError{kStorageCapability};
                    break;
                }
            }
            for (const auto& schema : input.input_schemas)
            {
                const bool supported = std::ranges::any_of(
                    support.capabilities,
                    [&](const auto& capability) noexcept
                    {
                        return std::ranges::any_of(
                            capability.inputs,
                            [&](const auto& payload) noexcept { return payload.schema == schema; }
                        );
                    }
                );
                if (!supported)
                {
                    published.unavailable = RenderError{kFeatureResourceMismatch};
                }
            }
            if (input.target)
            {
                const bool supported = std::ranges::any_of(
                    support.capabilities,
                    [&](const auto& capability) noexcept
                    {
                        const bool has_format = std::ranges::find(capability.target_formats, input.target->format) !=
                                                capability.target_formats.end();
                        const bool has_samples = std::ranges::find(capability.target_samples, input.target->samples) !=
                                                 capability.target_samples.end();
                        const bool has_kind = std::ranges::find(capability.target_kinds, input.target->kind) !=
                                              capability.target_kinds.end();
                        return has_format && has_samples && has_kind;
                    }
                );
                auto valid = input.target->validate();
                if (!valid || !supported)
                {
                    published.unavailable = valid ? RenderError{kRuntimeUnsupported} : valid.error();
                }
            }
            for (auto dependency : entry.graph.dependencies)
            {
                if (!features_by_name.contains(dependency.name()))
                {
                    published.unavailable = RenderError{kFeatureMissingProvider};
                    break;
                }
            }
        }
        result.error = {};
    }

    void RenderServer::Impl::installCapabilities(
        const InstallCapabilities::Request& input,
        InstallCapabilities::Response& result
    ) noexcept
    {
        const auto* feature = features.find(input.selection.implementation_);
        if (!feature || !feature->capabilities)
        {
            result.error = RenderError{kRuntimeIdentity};
            return;
        }
        ConfigValue decoded;
        auto configuration = qualifyConfiguration(*feature, input.configuration, decoded);
        if (!configuration)
        {
            result.error = configuration.error();
            return;
        }
        if (auto destination = findScope(input.scope); !destination)
        {
            result.error = destination.error();
            return;
        }

        SceneId scene;
        std::optional<ViewRequest> view;
        const auto* attachment_id = std::get_if<AttachmentId>(&input.scope);
        if (const auto* selected_scene = std::get_if<SceneId>(&input.scope))
        {
            scene = *selected_scene;
        }
        else if (const auto* selected_view = std::get_if<ViewRequest>(&input.scope))
        {
            view = *selected_view;
            scene = view->scene;
        }
        else if (attachment_id)
        {
            const auto& attachment = *attachments.find(*attachment_id);
            scene = attachment.scene;
            view = ViewRequest{scene, attachment.view};
        }

        std::vector<std::pair<StorageSet*, StorageId>> created;
        cxx::scope_exit rollback(
            [&]() noexcept
            {
                for (auto [set, id] : created | std::views::reverse)
                {
                    (void)set->remove(id);
                }
            }
        );

        struct Binding final
        {
            StorageSet* set;
            StorageId storage;
            std::uint32_t input;
            VRenderScope scope;
        };

        std::vector<Binding> bindings;
        const auto declarations = feature->graph.storage_inputs;
        for (std::uint32_t i = 0; i < declarations.size(); ++i)
        {
            const auto& declaration = declarations[i];
            VRenderScope scope;
            switch (declaration.scope)
            {
            case ECapabilityInputScope::GLOBAL:
                scope = GlobalRenderScope{};
                break;
            case ECapabilityInputScope::SCENE:
                if (scene.isNull())
                {
                    continue;
                }
                scope = scene;
                break;
            case ECapabilityInputScope::VIEW:
                if (!view)
                {
                    continue;
                }
                scope = *view;
                break;
            case ECapabilityInputScope::ATTACHMENT:
                if (!attachment_id)
                {
                    continue;
                }
                scope = *attachment_id;
                break;
            }
            auto* set = *findScope(scope); // The enclosing scope and all ancestors were qualified above.
            const auto type = resources->typeId(declaration.storage_type);
            auto dependency_config = declaration.configuration ? declaration.configuration((*configuration)->data())
                                                               : RenderResult<ConfigValue>{ConfigValue{}};
            if (!dependency_config)
            {
                result.error = dependency_config.error();
                return;
            }
            auto storage = ensureStorage(*set, type, declaration.storage_key, *dependency_config, created);
            if (!storage)
            {
                result.error = storage.error();
                return;
            }
            bindings.push_back({set, *storage, i, std::move(scope)});
        }
        // No fallible allocation or user factory remains. Publish all input relations together.
        for (const auto& binding : bindings)
        {
            const auto& declaration = declarations[binding.input];
            binding.set->bindInput(input.selection, binding.input, binding.storage);
            result.inputs.push_back(
                {std::string(declaration.schema),
                 declaration.version,
                 CapabilityInput{input.selection, binding.scope, binding.input}}
            );
        }
        rollback.release();
        result.error = {};
        storages_created.store(resources->creations());
    }
} // namespace lux::render
