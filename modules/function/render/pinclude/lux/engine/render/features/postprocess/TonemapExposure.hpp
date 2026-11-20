#pragma once

#include <array>
#include <cmath>
#include <lux/engine/render/commands/EffectCommands.hpp>
#include <lux/engine/render/commands/Error.hpp>
#include <lux/engine/render/extensions/graph/ConstantBlock.hpp>

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

        static void setNode(TonemapExposure& state, const SetToneMappingExposure::Request& request, AckResponse& out)
            noexcept
        {
            assign(state, request.exposure, out);
        }

    private:
        static void assign(TonemapExposure& state, float exposure, AckResponse& out) noexcept
        {
            if (!std::isfinite(exposure))
            {
                out.error = {kFrameContract};
                return;
            }
            if (exposure != state.constants.exposure)
            {
                if (state.revision == UINT64_MAX)
                {
                    out.error = {kRuntimeCapacity};
                    return;
                }
                state.constants.exposure = exposure;
                ++state.revision;
            }
            out.error = {};
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
