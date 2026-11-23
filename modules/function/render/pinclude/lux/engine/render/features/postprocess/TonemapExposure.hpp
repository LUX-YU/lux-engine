#pragma once

#include <lux/engine/render/frame/FrameError.hpp>
#include <array>
#include <cmath>
#include <lux/engine/render/graph/ConstantBlock.hpp>

namespace lux::render::detail
{
    struct TonemapConstants final
    {
        float exposure;
    };

    struct TonemapExposure final
    {
        TonemapConstants constants;
        std::uint64_t revision = 1;

        [[nodiscard]] RenderResult<void> setExposure(float exposure) noexcept
        {
            if (!std::isfinite(exposure)) return cxx::unexpected(RenderError{kFrameContract});
            if (revision == UINT64_MAX) return cxx::unexpected(RenderError{kRuntimeCapacity});
            constants.exposure = exposure;
            ++revision;
            return {};
        }
    };
} // namespace lux::render::detail

namespace lux::render
{
    template <> struct ConstantBlockTraits<detail::TonemapConstants> final
    {
        static ConstantBlockLayout layout(rdesc::EShaderTarget target) noexcept
        {
            static constexpr std::array members{
                ConstantMemberLayout{"exposure", offsetof(detail::TonemapConstants, exposure), rdesc::EDataScalar::FLOAT}
            };
            static_assert(sizeof(detail::TonemapConstants) == 4);
            // DXIL's constant buffer alignment is distinct from the packed root DWORD payload.
            const unsigned alignment = target == rdesc::EShaderTarget::DXIL ? 16u : 4u;
            return {sizeof(detail::TonemapConstants), alignment, members};
        }
    };
} // namespace lux::render
