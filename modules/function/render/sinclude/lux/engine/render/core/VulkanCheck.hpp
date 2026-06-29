#pragma once
#include <vulkan/vulkan.h>
#include <lux/engine/render/core/Errors.hpp>
#include <stdexcept>
#include <string>

namespace lux::render
{
    /**
     * @brief Check a VkResult and throw on failure.
     *
     * In debug builds this also fires an assert so the debugger can catch
     * the exact call-site.  In release builds it throws a runtime_error
     * with the numeric error code.
     */
    inline void vk_check(VkResult result, const char* expr = nullptr)
    {
        if (result == VK_SUCCESS) return;
#ifndef NDEBUG
        assert(false && "VK_CHECK failed");
#endif
        std::string msg = "Vulkan call failed with VkResult = " + std::to_string(static_cast<int>(result));
        if (expr) { msg += " ("; msg += expr; msg += ")"; }
        throw std::runtime_error(msg);
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
 */
#ifndef VK_EXPECT
#define VK_EXPECT(x)                                                         \
    do {                                                                      \
        VkResult _vk_r = (x);                                                \
        if (_vk_r != VK_SUCCESS)                                             \
            return ::lux::cxx::unexpected(                                    \
                ::lux::render::make_error_code(                               \
                    ::lux::render::ERenderError::InternalError));             \
    } while (0)
#endif
