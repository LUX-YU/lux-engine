#pragma once

#include <NRI.h>
#include <lux/engine/render/core/Target.hpp>
#include <lux/engine/render/resources/texture/Data.hpp>
#include <span>

namespace lux::render
{
    struct SampledImageReference final
    {
        TextureId texture;
        TargetId target;
    };

    struct IndexedDraw final
    {
        nri::DrawIndexedDesc draw{};
        nri::Rect scissor{};
        std::uint32_t image = 0;
    };

    // CPU spans are copied into the writable FIF before this call returns.
    struct IndexedDrawData final
    {
        std::span<const std::byte> vertices, indices, parameters;
        std::span<const IndexedDraw> draws;
        std::span<const SampledImageReference> images;
        std::uint32_t vertex_stride = 0, index_stride = 0;
    };

    class InstanceAccess
    {
    public:
        virtual ~InstanceAccess() noexcept = default;
        [[nodiscard]] virtual const TargetDesc& target() const noexcept = 0;
        [[nodiscard]] virtual RenderResult<void>
        setIndexedDrawData(GraphPassId, std::uint32_t slot, const IndexedDrawData&) noexcept = 0;
    };
} // namespace lux::render
