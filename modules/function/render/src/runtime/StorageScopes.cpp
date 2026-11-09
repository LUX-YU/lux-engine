#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render
{
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

    RenderResult<StorageId> RenderServer::Impl::resolveEffectStorage(
        const RenderEffect& effect,
        std::uint32_t capability,
        std::uint32_t dependency,
        const VRenderScope& scope
    ) noexcept
    {
        if (const auto* id = std::get_if<AttachmentId>(&scope))
        {
            const auto* attachment = attachments.find(*id);
            if (!attachment || effects.find(attachment->effect) != &effect)
            {
                return cxx::unexpected(RenderError{kRuntimeIdentity});
            }
        }
        const auto& indices = effect.implementations[capability].storage_dependencies;
        const auto& declared = effect.storage_dependencies[indices[dependency]];
        if (scope.index() != static_cast<std::size_t>(declared.scope))
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        auto set = findScope(scope);
        if (!set)
        {
            return cxx::unexpected(set.error());
        }
        const auto& requirement = declared.requirement;
        const auto type = resources->typeId(requirement.type.view());
        const auto previous_size = (*set)->size();
        // getOrCreate constructs the actual Storage before publishing it. A failed factory
        // leaves this scope unchanged; no separate public activation transaction is required.
        auto dependencies = storageContext();
        auto created = (*set)->getOrCreate(dependencies, type, requirement.key, requirement.config);
        if (created && (*set)->size() != previous_size)
        {
            active_frame_inputs_dirty = true;
            storages_created.store(resources->creations());
        }
        return created;
    }
} // namespace lux::render
