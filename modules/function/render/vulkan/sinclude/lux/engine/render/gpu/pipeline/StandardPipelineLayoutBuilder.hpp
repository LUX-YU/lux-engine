#pragma once
#include <lux/engine/function/visibility.h>
#include <lux/engine/function/render/client/core/Errors.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <span>
#include <string_view>

namespace lux::rdesc
{
struct ShaderInfo;
}

namespace lux::render
{

class RenderContext;

/// Build or fetch a standard graphics pipeline layout that uses descriptor
/// set layouts [0, descriptor_set_count) from GeneralDescriptorSetLayout and
/// merged push-constant ranges inferred from shader reflection metadata.
[[nodiscard]] LUX_FUNCTION_PUBLIC Expected<VkPipelineLayout> buildStandardGraphicsPipelineLayout(
    RenderContext& ctx,
    uint32_t descriptor_set_count,
    std::span<const lux::rdesc::ShaderInfo* const> shader_infos,
    std::string_view debug_name);

} // namespace lux::render

