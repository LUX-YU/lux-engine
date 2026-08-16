#pragma once
#include <vulkan/vulkan.h>
#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>

namespace lux::render
{
    inline void vk_check(
        VkResult result,
        const char* expression = nullptr
    )
    {
        if (result == VK_SUCCESS)
            return;
        renderFatal(expression ? expression : "Vulkan call failed");
    }
} // namespace lux::render

/**
 * @brief Convenience macro that captures the expression text for diagnostics.
 */
#ifndef VK_CHECK
#define VK_CHECK(x) ::lux::render::vk_check((x), #x)
#endif

/**
 * @brief Non-throwing variant: evaluate a Vulkan call and return Expected<void>
 *        on failure.  Use in hot-path functions that return Expected<void>.
 *
 * 失败时把那个 VkResult 一起带上 —— 调用点已经拿到了它,丢掉等于让上层只知道
 * 「某个 Vulkan 调用失败了」。
 */
#ifndef VK_EXPECT
#define VK_EXPECT(x)                                                              \
    do {                                                                          \
        const VkResult _vk_r = (x);                                               \
        if (_vk_r != VK_SUCCESS)                                                  \
            return ::lux::render::renderFailure<                                  \
                ::lux::render::err::device::VulkanCallFailed>(                    \
                    ::lux::render::encodeVkResult(_vk_r));                        \
    } while (0)
#endif
