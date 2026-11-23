#pragma once

#include <lux/engine/render/runtime/TargetId.hpp>
#include <lux/engine/systems/render/RenderReaction.hpp>
#include <lux/engine/systems/render/view/RenderTargetComponent.hpp>
#include <unordered_map>

namespace lux::engine::render
{
    struct RenderViewComponent;

    [[nodiscard]] LUX_ENGINE_SYSTEM_RENDER_PUBLIC lux::render::TargetId
    findRenderTarget(const ecs::Registry& registry, ecs::Entity entity) noexcept;
    [[nodiscard]] LUX_ENGINE_SYSTEM_RENDER_PUBLIC const lux::render::TargetDesc*
    renderTargetDescription(const ecs::Registry& registry, ecs::Entity entity) noexcept;

    // One per Scene. Locals own candidates; removed_resources_ owns only outputs removed from ECS.
    class LUX_ENGINE_SYSTEM_RENDER_PUBLIC ViewReaction final : public RenderReaction
    {
    public:
        using Failure = std::pair<ecs::Entity, error::Error>;
        object::TSignal<Failure> outputFailed{*this};
        using CreateResult = lux::render::RenderResult<std::unique_ptr<ViewReaction>>;
        [[nodiscard]] static CreateResult create(RenderSystem& system) noexcept;
        ~ViewReaction() noexcept override;

    private:
        explicit ViewReaction(RenderSystem& system) noexcept;
        void inputChanged(ecs::Registry&, ecs::Entity entity) noexcept;
        void removeOutput(ecs::Registry& registry, ecs::Entity entity) noexcept;
        void reportFailure(ecs::Entity entity, error::Error error) noexcept override;
        [[nodiscard]] RenderViewComponent takeOutput(ecs::Entity entity) noexcept;
        [[nodiscard]] exec::task<Result> reconcile(ecs::Entity entity) noexcept override;
        [[nodiscard]] exec::task<Result> createOutput(ecs::Entity entity, RenderTargetComponent desired) noexcept;
        [[nodiscard]] exec::task<Result>
        resizeOutput(ecs::Entity entity, lux::render::TargetId target, lux::render::TargetDesc description) noexcept;
        [[nodiscard]] exec::task<Result> resumeOutput(ecs::Entity entity) noexcept;
        [[nodiscard]] exec::task<Result> releaseOutput(RenderViewComponent& resources) noexcept;
        [[nodiscard]] exec::task<Result>
        discard(ecs::Entity entity, RenderViewComponent resources, error::Error cause = {}) noexcept;
        [[nodiscard]] exec::task<Result> drainRemoved(ecs::Entity entity) noexcept;
        [[nodiscard]] exec::task<Result> releaseResources() noexcept override;
        std::unordered_multimap<ecs::Entity, RenderViewComponent> removed_resources_;
    };
} // namespace lux::engine::render
