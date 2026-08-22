#include <lux/engine/ecs/render/systems/RenderSystem.hpp>

#include <lux/engine/function/render/client/RenderControlSession.hpp>

#include <cstdlib>
#include <utility>
#include <vector>

namespace lux::ecs
{
    struct RenderSystem::Impl
    {
        Impl(
            SceneRenderBinding& binding_value,
            ActiveRenderView& active_view_value,
            lux::render::RenderSceneLease scene,
            std::vector<std::unique_ptr<RenderStage>> stage_values)
            : scene_lease(std::move(scene)),
              binding(binding_value),
              active_view(active_view_value)
        {
            stages.reserve(stage_values.size());
            for (auto& stage : stage_values)
            {
                if (!stage)
                    std::abort();
                stages.push_back(std::move(stage));
            }
        }

        void activate(Registry& value)
        {
            if (active)
                return;
            registry = &value;
            active = true;
        }

        void extract(float dt, std::uint64_t tick)
        {
            if (!registry || closing)
                return;
            if (!binding.get().applyPendingSceneOriginRebase())
                return;
            for (auto& stage : stages)
            {
                RenderExtractContext context{
                    *registry,
                    binding.get(),
                    active_view.get(),
                    dt,
                    tick};
                stage->extract(context);
            }
        }

        void detach(Registry&) noexcept
        {
            if (!active)
                return;
            active = false;
            registry = nullptr;
        }

        void advanceClose() noexcept
        {
            if (closed)
                return;
            if (!binding.get().control().flushDeferredReleases())
                return;
            binding.get().control().pumpReplies();
            const auto status = scene_lease.close();
            if (status != lux::render::ERenderLeaseCloseStatus::Released &&
                status != lux::render::ERenderLeaseCloseStatus::AlreadyClosed)
            {
                return;
            }
            closed = true;
            close_progress.notify();
        }

        std::vector<std::unique_ptr<RenderStage>> stages;
        lux::render::RenderSceneLease scene_lease;
        std::reference_wrapper<SceneRenderBinding> binding;
        std::reference_wrapper<ActiveRenderView> active_view;
        Registry* registry{nullptr};
        std::uint64_t tick_index{0u};
        SystemCloseProgressSink close_progress{};
        bool active{false};
        bool closing{false};
        bool closed{false};
    };

    RenderSystem::RenderSystem(
        SceneRenderBinding& binding,
        ActiveRenderView& active_view,
        lux::render::RenderSceneLease scene,
        std::vector<std::unique_ptr<RenderStage>> stages)
        : impl_(std::make_unique<Impl>(
              binding,
              active_view,
              std::move(scene),
              std::move(stages)))
    {
    }

    RenderSystem::~RenderSystem()
    {
        requestClose();
        if (impl_ && impl_->registry)
            impl_->detach(*impl_->registry);
    }

    void RenderSystem::onAdded(const SystemSetupContext& setup)
    {
        impl_->activate(setup.registry());
    }

    void RenderSystem::onRemoved(const SystemRemovalContext& removal)
    {
        impl_->detach(removal.registry());
    }

    void RenderSystem::update(const SystemUpdateContext& context)
    {
        if (impl_->closing)
        {
            impl_->advanceClose();
            return;
        }
        impl_->tick_index = context.tickIndex();
        impl_->extract(context.dt(), context.tickIndex());
    }

    void RenderSystem::requestClose() noexcept
    {
        requestClose({});
    }

    void RenderSystem::requestClose(
        SystemCloseProgressSink progress) noexcept
    {
        if (progress)
            impl_->close_progress = progress;
        impl_->closing = true;
        impl_->advanceClose();
    }

    bool RenderSystem::closeComplete() const noexcept
    {
        return impl_->closed;
    }

    bool RenderSystem::closeNeedsOwnerTick() const noexcept
    {
        return impl_->closing && !impl_->closed;
    }

}
