#pragma once

#include <lux/engine/render/effect/CpuCallFrame.hpp>

namespace lux::render
{
    class InstanceLinkContext
    {
    public:
        virtual ~InstanceLinkContext() noexcept = default;

        [[nodiscard]] virtual RenderResult<DrawStreamSlot>
        resolveDrawStream(GraphPassId) noexcept = 0;

        template <class Storage>
        [[nodiscard]] RenderResult<DrawResourceSlot<Storage>>
        resolveDrawResource(GraphPassId pass, std::string_view binding) const noexcept
        {
            const auto resolved = resolveDrawSource(pass, binding, cxx::make_basic_type_info<Storage>());
            if (!resolved) return cxx::unexpected(resolved.error());
            return DrawResourceSlot<Storage>{*resolved};
        }

        [[nodiscard]] virtual RenderResult<RasterProgramSlot>

        resolveRasterProgram(GraphPassId, std::uint32_t source_program) const noexcept = 0;

        [[nodiscard]] virtual RenderResult<RasterBufferSlot>
        resolveRasterBuffer(GraphPassId, GraphResourceId) const noexcept = 0;

        [[nodiscard]] virtual RenderResult<RasterImageSlot>
        resolveRasterImage(GraphPassId, GraphResourceId) const noexcept = 0;

        [[nodiscard]] virtual RenderResult<RasterSamplerSlot>
        resolveRasterSampler(GraphPassId, std::uint32_t) const noexcept = 0;

        [[nodiscard]] virtual RenderResult<std::uint32_t>
        resolveRasterArgument(GraphPassId, std::string_view) const noexcept = 0;

    private:
        [[nodiscard]] virtual RenderResult<extension_detail::DrawSourceAddress>
        resolveDrawSource(GraphPassId, std::string_view, cxx::basic_type_info) const noexcept = 0;

    };
} // namespace lux::render
