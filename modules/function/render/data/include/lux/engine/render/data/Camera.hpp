#pragma once

#include <lux/engine/render/core/Camera.hpp>
#include <lux/engine/render/data/ShaderBlock.hpp>

namespace lux::render
{
    // Cold ABI description shared by the Runtime camera source and actual Feature shader declarations.
    [[nodiscard]] inline ShaderBlock cameraParameterBlock() noexcept
    {
        ShaderBlock result{128, 16};
        result.fields = {
            {"view", rdesc::EDataScalar::FLOAT, 0, 4, 4, 0, 16},
            {"projection", rdesc::EDataScalar::FLOAT, 64, 4, 4, 0, 16}
        };
        return result;
    }
}
