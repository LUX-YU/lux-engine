#pragma once
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/render/gpu/lifecycle/DeviceObject.hpp>

namespace lux::render
{
    /// Compiled shader module + reflection metadata.
    /// Managed by ShaderResources.
    struct ShaderObject
    {
        ShaderModuleOwner module;
        lux::rdesc::ShaderInfo info{};
    };
} // namespace lux::render
