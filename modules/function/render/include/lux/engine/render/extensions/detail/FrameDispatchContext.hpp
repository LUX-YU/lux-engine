#pragma once

#include <lux/cxx/execution/ManualContext.hpp>
#include <lux/engine/render/core/Ids.hpp>
#include <lux/engine/render/extensions/features/CapabilityLowering.hpp>
#include <lux/engine/render/frame/FrameData.hpp>
#include <variant>

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
    struct FrameParameterTarget final
    {
        detail::RenderGraphInstance& instance;
        ParameterProjection projection;
    };

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

    using VFrameDataTarget = std::variant<void*, FrameParameterTarget, FrameDrawTarget>;
} // namespace lux::render

namespace lux::render::runtime_detail
{
    struct FrameRecord;

    // Borrowed only during invocation. Async methods receive a copy of their narrow destination
    // and data headers; neither the stack context nor a temporary decoded span object is retained.
    struct FrameDispatchContext final
    {
        FrameDataView data;
        const void* root; // Size/alignment/object lifetime were qualified at frame admission.
        const VFrameDataTarget& target;
        FrameRecord& frame;
        std::uint32_t entry;
        stdexec::counting_scope& scope;
        cxx::execution::ManualContext::Scheduler server_delivery;
    };

    LUX_RENDER_PUBLIC void completeFrameData(FrameRecord&, std::uint32_t entry, RenderError) noexcept;
} // namespace lux::render::runtime_detail
