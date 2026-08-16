#pragma once
#include <lux/engine/gapi/vk/vk.hpp>
#include <lux/engine/description/ShaderInfo.hpp>

namespace lux::render
{
    /// Compiled shader module + reflection metadata.
    /// Managed by ShaderResources.
    struct ShaderObject
    {
        VkShaderModule module{VK_NULL_HANDLE};
        lux::rdesc::ShaderInfo info{};
    };
} // namespace lux::render
