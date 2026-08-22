#include <lux/engine/ecs/render/systems/RenderSystem.hpp>

#include <lux/engine/function/render/client/FeatureCatalog.hpp>
#include <lux/engine/function/render/client/RenderUploadClient.hpp>
#include <utility>

namespace lux::ecs
{
    struct RenderSystem::Impl
    {
        Impl(
            lux::render::RenderFrameSession& session,
            lux::render::RenderControlSession& control,
            lux::render::RenderUploadClient upload,
            lux::render::RenderSceneLease scene,
            RenderSystemStages stages_value
        )
            : stages(std::move(stages_value)),
              scene_lease(std::move(scene)),
              binding(session, control, std::move(upload), scene_lease.id())
        {
        }

        RenderSystemStages            stages;
        lux::render::RenderSceneLease scene_lease;
        SceneRenderBinding           binding;
        ActiveRenderView             active_view;
        lux::ecs::Registry*   registry{nullptr};
        std::uint64_t                tick_index{0};
        bool                         closing{false};
        bool                         closed{false};
    };

    RenderSystem::RenderSystem(
        lux::render::RenderFrameSession& session,
        lux::render::RenderControlSession& control,
        lux::render::RenderUploadClient upload,
        lux::render::RenderSceneLease scene,
        RenderSystemStages stages
    )
        : impl_(std::make_unique<Impl>(
              session,
              control,
              std::move(upload),
              std::move(scene),
              std::move(stages)
          ))
    {
    }

    RenderSystem::~RenderSystem()
    {
        (void)close();
        if (impl_ && impl_->registry)
            impl_->stages.detach(*impl_->registry);
    }

    void RenderSystem::onAdded(const SystemSetupContext& setup)
    {
        impl_->registry = &setup.registry();
        impl_->stages.activate(setup.registry());
    }

    void RenderSystem::onRemoved(const SystemRemovalContext& removal)
    {
        impl_->stages.detach(removal.registry());
        impl_->registry = nullptr;
    }

    void RenderSystem::update(const SystemUpdateContext& context)
    {
        if (impl_->closing)
            return;
        impl_->tick_index = context.tickIndex();
        impl_->stages.extract(
            context.registry(),
            impl_->binding,
            impl_->active_view,
            context.dt(),
            context.tickIndex()
        );
    }

    std::span<const std::string_view>
    RenderSystem::requiredFeatures() const noexcept
    {
        return impl_->stages.requiredFeatures();
    }

    void RenderSystem::setFeatures(
        const lux::render::FeatureCatalog& catalog
    ) noexcept
    {
        impl_->binding.setCatalog(catalog);
    }

    void RenderSystem::bindFeature(
        std::string_view           name,
        lux::render::FeatureHandle handle
    )
    {
        impl_->binding.bindFeature(name, handle);
    }

    void RenderSystem::unbindFeature(
        std::string_view name,
        lux::render::FeatureHandle expected) noexcept
    {
        impl_->binding.unbindFeature(name, expected);
    }

    void RenderSystem::settle()
    {
        if (!impl_->registry || impl_->closing)
            return;
        impl_->stages.settle(
            *impl_->registry,
            impl_->binding,
            impl_->active_view,
            impl_->tick_index
        );
    }

    lux::render::ERenderLeaseCloseStatus RenderSystem::close() noexcept
    {
        if (impl_->closed)
            return lux::render::ERenderLeaseCloseStatus::AlreadyClosed;
        if (!impl_->closing && impl_->registry)
            impl_->stages.close(
                *impl_->registry,
                impl_->binding,
                impl_->active_view,
                impl_->tick_index
            );
        impl_->closing = true;
        const auto status = impl_->scene_lease.close();
        if (status == lux::render::ERenderLeaseCloseStatus::Released ||
            status == lux::render::ERenderLeaseCloseStatus::AlreadyClosed)
            impl_->closed = true;
        return status;
    }

    SceneRenderBinding& RenderSystem::binding() noexcept
    {
        return impl_->binding;
    }

    ActiveRenderView& RenderSystem::activeView() noexcept
    {
        return impl_->active_view;
    }

    std::uint64_t RenderSystem::droppedStaleCommands() const noexcept
    {
        return impl_->stages.droppedStaleCommands();
    }

} // namespace lux::ecs
