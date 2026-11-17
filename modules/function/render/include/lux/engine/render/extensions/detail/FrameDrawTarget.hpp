#pragma once

#include <lux/engine/render/core/Error.hpp>
#include <lux/engine/render/core/Ids.hpp>

namespace nri
{
    struct Texture;
}

namespace lux::render::detail
{
    class RenderGraphInstance;
    class ResourceUploader;
} // namespace lux::render::detail

namespace lux::render
{
    // These are actual borrowed destinations, prepared at binding/Attach, not a Server facade.
    struct FrameSampledImage final
    {
        const nri::Texture* image;
        std::uint32_t descriptor;
    };

    struct FrameDrawTarget final
    {
        detail::RenderGraphInstance& instance;
        GraphPassId pass;
        detail::ResourceUploader& uploads;
        void* target_context = nullptr;
        RenderResult<FrameSampledImage> (*resolve_target)(void*, TargetId, std::uint32_t) noexcept = nullptr;
    };

} // namespace lux::render
