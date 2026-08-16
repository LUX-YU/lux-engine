#pragma once

#include <lux/engine/ecs/EcsCommandBuffer.hpp>
#include <lux/engine/ecs/render/RenderExtractionResources.hpp>
#include <lux/engine/ecs/render/SceneRenderBinding.hpp>
#include <lux/engine/meta/LuxObject.hpp>

#include <cstdint>
#include <functional>

namespace lux::ecs
{
    /// Narrow, non-owning view exposed to RenderSystem-internal extraction
    /// stages. It has no frame admission or submission API: the lexical frame
    /// is opened and closed by FrameCoordinator outside the ECS schedule.
    class RenderSubsystemContext final
    {
    public:
        RenderSubsystemContext(
            lux::meta::EntityRegistry& registry,
            EcsCommandWriter           commands,
            SceneRenderBinding&        render,
            ActiveRenderView&          active_view,
            float                      dt,
            std::uint64_t              tick_index
        ) noexcept
            : registry_(registry),
              commands_(commands),
              render_(render),
              active_view_(active_view),
              dt_(dt),
              tick_index_(tick_index)
        {
        }

        [[nodiscard]] lux::meta::EntityRegistry& registry() const noexcept
        {
            return registry_.get();
        }

        [[nodiscard]] const EcsCommandWriter& commands() const noexcept
        {
            return commands_;
        }

        [[nodiscard]] SceneRenderBinding& render() const noexcept
        {
            return render_.get();
        }

        [[nodiscard]] ActiveRenderView& activeView() const noexcept
        {
            return active_view_.get();
        }

        [[nodiscard]] float dt() const noexcept { return dt_; }
        [[nodiscard]] std::uint64_t tickIndex() const noexcept
        {
            return tick_index_;
        }

    private:
        std::reference_wrapper<lux::meta::EntityRegistry> registry_;
        EcsCommandWriter                               commands_{};
        std::reference_wrapper<SceneRenderBinding>     render_;
        std::reference_wrapper<ActiveRenderView>       active_view_;
        float                                          dt_{0.0f};
        std::uint64_t                                  tick_index_{0};
    };

} // namespace lux::ecs
