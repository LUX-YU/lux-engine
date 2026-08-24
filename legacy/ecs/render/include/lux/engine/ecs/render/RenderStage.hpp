#pragma once

#include <lux/engine/ecs/render/RenderExtractContext.hpp>
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
    /// Observer/cache implementation details are allowed only when their
    /// lifetime is exactly this owning RenderSystem. A Stage has no lifecycle
    /// graph, command producer identity, or dynamic topology API.
    class LUX_FUNCTION_PUBLIC RenderStage
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

        virtual void extract(RenderExtractContext& context) = 0;

    protected:
        RenderStage() = default;
    };
}
