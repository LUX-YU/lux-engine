#pragma once

#include <lux/engine/render/effect/CpuCallFrame.hpp>

namespace lux::render
{
    namespace extension_detail
    {
        enum class ECpuPort : std::uint8_t { INPUT, OUTPUT };
    }

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

        template <CpuPreparationValue T>
        [[nodiscard]] RenderResult<CpuInputSlot<T>> resolveCpuInput(GraphCpuDataId source) const noexcept
        {
            auto slot = resolveCpuSlot(
                source, cxx::make_basic_type_info<T>(), sizeof(T), extension_detail::ECpuPort::INPUT
            );
            if (!slot) return cxx::unexpected(slot.error());
            return CpuInputSlot<T>{*slot};
        }

        template <CpuPreparationValue T>
        [[nodiscard]] RenderResult<CpuOutputSlot<T>> resolveCpuOutput(GraphCpuDataId source) const noexcept
        {
            auto slot = resolveCpuSlot(
                source, cxx::make_basic_type_info<T>(), sizeof(T), extension_detail::ECpuPort::OUTPUT
            );
            if (!slot) return cxx::unexpected(slot.error());
            return CpuOutputSlot<T>{*slot};
        }

        // Grants a value snapshot of this producer's owning EffectGraph node.
        template <CpuPreparationValue T>
        [[nodiscard]] RenderResult<CpuInputSlot<T>> resolveEffectInput() noexcept
        {
            auto slot = claimEffectInput(cxx::make_basic_type_info<T>(), sizeof(T));
            if (!slot) return cxx::unexpected(slot.error());
            return CpuInputSlot<T>{*slot};
        }

        template <ShaderConstantBlock T>
        [[nodiscard]] RenderResult<InlineConstantWriter<T>> resolveInlineConstants(GraphPassId pass) noexcept
        {
            auto slot = claimInlineBlock(pass, cxx::make_basic_type_info<T>(), sizeof(T));
            if (!slot) return cxx::unexpected(slot.error());
            return InlineConstantWriter<T>{*slot};
        }

    private:
        [[nodiscard]] virtual RenderResult<extension_detail::DrawSourceAddress>
        resolveDrawSource(GraphPassId, std::string_view, cxx::basic_type_info) const noexcept = 0;

        [[nodiscard]] virtual RenderResult<std::uint32_t>
        claimEffectInput(cxx::basic_type_info, std::size_t) noexcept = 0;

        [[nodiscard]] virtual RenderResult<std::uint32_t>
        resolveCpuSlot(
            GraphCpuDataId, cxx::basic_type_info, std::size_t, extension_detail::ECpuPort
        ) const noexcept = 0;

        [[nodiscard]] virtual RenderResult<std::uint32_t>
        claimInlineBlock(GraphPassId, cxx::basic_type_info, std::size_t) noexcept = 0;

    };
} // namespace lux::render
