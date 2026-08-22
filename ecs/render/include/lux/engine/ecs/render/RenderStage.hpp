#pragma once

#include <lux/engine/ecs/EcsCommandBuffer.hpp>
#include <lux/engine/ecs/SystemUpdateContext.hpp>
#include <lux/engine/ecs/render/RenderSubsystemContext.hpp>
#include <lux/engine/function/visibility.h>

#include <span>
#include <string_view>

namespace lux::ecs
{
    /// One immutable ECS-to-render extraction stage owned by RenderSystem.
    ///
    /// The stage sequence is assembled only while the World is unpublished.
    /// It has no dependency declarations and no runtime add/remove protocol;
    /// ordering belongs to the product's direct composition function. The
    /// lifecycle hooks below are restricted to World observer/resource
    /// ownership while those responsibilities are split into ordinary Systems.
    class LUX_FUNCTION_PUBLIC RenderStage : public IEcsCommandProducer
    {
    public:
        virtual ~RenderStage() = default;

        RenderStage(const RenderStage&) = delete;
        RenderStage& operator=(const RenderStage&) = delete;

        [[nodiscard]] virtual std::span<const std::string_view>
        requiredFeatures() const noexcept
        {
            return {};
        }

        virtual void onAdded(const SystemSetupContext&) {}
        virtual void onRemoved(const SystemRemovalContext&) {}
        virtual void prepare(RenderSubsystemContext&) noexcept {}
        virtual void extract(RenderSubsystemContext& context) = 0;
        virtual void settle(RenderSubsystemContext&) {}
        virtual void close(RenderSubsystemContext&) noexcept {}
        virtual void closeScene(RenderSubsystemContext& context) noexcept
        {
            close(context);
        }

    protected:
        RenderStage() = default;
    };
}
