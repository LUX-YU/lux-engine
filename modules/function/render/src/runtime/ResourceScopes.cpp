#include <lux/engine/render/runtime/RenderServerImpl.hpp>

namespace lux::render
{
    RenderResult<ResourceSet*>
    RenderServer::Impl::resourceScope(const VRenderScope& scope, ResourceTypeId type) noexcept
    {
        const auto* factory = resources_->factory(type);
        if (!factory)
        {
            return cxx::unexpected(RenderError{kStorageFactory});
        }
        if (std::holds_alternative<AttachmentId>(scope))
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        auto actual = findScope(scope);
        if (!actual)
        {
            return cxx::unexpected(actual.error());
        }
        switch (factory->descriptor->domain)
        {
        case EResourceDomain::DEVICE:
            return &*global_storages_;
        case EResourceDomain::SCENE:
            if (const auto* view = std::get_if<ViewRequest>(&scope))
            {
                return &scenes_.find(view->scene)->storages;
            }
            if (std::holds_alternative<SceneId>(scope))
            {
                return *actual;
            }
            break;
        case EResourceDomain::VIEW:
            if (std::holds_alternative<ViewRequest>(scope))
            {
                return *actual;
            }
            break;
        }
        return cxx::unexpected(RenderError{kStorageMismatch});
    }

    void RenderServer::Impl::queryResource(const ResourceRequest& request, ResourceResponse& response) noexcept
    {
        const auto type = resources_->typeId(request.declaration);
        auto owner = resourceScope(request.scope, type);
        if (!owner)
        {
            response.error = owner.error();
            return;
        }
        auto context = resourceCreateContext();
        const auto id = (*owner)->query(context, type);
        if (!id)
        {
            response.error = id.error();
            return;
        }
        response = {{}, type, *id};
    }

    void RenderServer::Impl::getOrCreateResource(const ResourceRequest& request, ResourceResponse& response) noexcept
    {
        const auto type = resources_->typeId(request.declaration);
        auto owner = resourceScope(request.scope, type);
        if (!owner)
        {
            response.error = owner.error();
            return;
        }
        auto context = resourceCreateContext();
        auto instance = (*owner)->getOrCreate(context, type);
        if (!instance)
        {
            response.error = instance.error();
            return;
        }
        storages_created_.store(resources_->creations(), std::memory_order_relaxed);
        response = {{}, type, *instance};
    }

    RenderResult<ResourceSet*> RenderServer::Impl::findScope(const VRenderScope& scope) noexcept
    {
        return std::visit(
            [&](const auto& location) noexcept -> RenderResult<ResourceSet*>
            {
                using T = std::remove_cvref_t<decltype(location)>;
                if constexpr (std::same_as<T, GlobalRenderScope>)
                {
                    return &*global_storages_;
                }
                else if constexpr (std::same_as<T, SceneId>)
                {
                    if (auto* scene = scenes_.find(location))
                    {
                        return &scene->storages;
                    }
                }
                else if constexpr (std::same_as<T, ViewRequest>)
                {
                    auto* scene = scenes_.find(location.scene);
                    auto* view = scene ? scene->views.find(location.view) : nullptr;
                    if (view)
                    {
                        return &view->storages;
                    }
                }
                else
                {
                    if (auto* attachment = attachments_.find(location))
                    {
                        auto* scene = scenes_.find(attachment->scene);
                        auto* view = scene ? scene->views.find(attachment->view) : nullptr;
                        if (view)
                        {
                            return &view->storages;
                        }
                    }
                }
                return cxx::unexpected(RenderError{kRuntimeIdentity});
            },
            scope
        );
    }

    RenderResult<ResourceInstanceId> RenderServer::Impl::ensureResource(ResourceSet& set, ResourceTypeId type) noexcept
    {
        auto dependencies = resourceCreateContext();
        return set.getOrCreate(dependencies, type);
    }

    RenderResult<ResourceInstanceId>
    RenderServer::Impl::resolveEffectResource(
        const RenderEffect& effect,
        std::uint32_t capability,
        std::uint32_t dependency,
        const VRenderScope& scope
    ) noexcept
    {
        if (const auto* id = std::get_if<AttachmentId>(&scope))
        {
            const auto* attachment = attachments_.find(*id);
            if (!attachment || effects_.find(attachment->effect) != &effect)
            {
                return cxx::unexpected(RenderError{kRuntimeIdentity});
            }
        }
        const auto& indices = effect.implementations[capability].resource_dependencies;
        if (dependency >= indices.size())
        {
            return cxx::unexpected(RenderError{kStorageMismatch});
        }
        const auto& declared = effect.resource_dependencies[indices[dependency]];
        const auto type = declared.declaration;
        auto actual_scope = scope;
        if (const auto* id = std::get_if<AttachmentId>(&scope))
        {
            const auto* attachment = attachments_.find(*id);
            actual_scope = ViewRequest{attachment->scene, attachment->view};
        }
        auto set = resourceScope(actual_scope, type);
        if (!set)
        {
            return cxx::unexpected(set.error());
        }
        const auto previous_size = (*set)->size();
        // getOrCreate constructs the actual Storage before publishing it. A failed factory
        // leaves this scope unchanged; no separate public activation transaction is required.
        auto dependencies = resourceCreateContext();
        auto created = (*set)->getOrCreate(dependencies, type);
        if (created && (*set)->size() != previous_size)
        {
            active_frame_inputs_dirty_ = true;
            storages_created_.store(resources_->creations(), std::memory_order_relaxed);
        }
        return created;
    }
} // namespace lux::render
