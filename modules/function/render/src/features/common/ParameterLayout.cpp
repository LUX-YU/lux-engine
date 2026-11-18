#include <lux/engine/render/core/EffectParameters.hpp>
#include <lux/engine/render/features/common/ParameterLayout.hpp>

#include <cstddef>

namespace lux::render::detail
{
    rdesc::ShaderBlock effectParameterLayout() noexcept
    {
        return {
            sizeof(EffectParameters),
            alignof(EffectParameters),
            rdesc::EDataLayout::STD430,
            {{"exposure", rdesc::EDataScalar::FLOAT, offsetof(EffectParameters, exposure), 1, 1, 0, 0}}
        };
    }

} // namespace lux::render::detail
