#pragma once

#include <lux/engine/render/vulkan/Error.hpp>

// Only instrumented test libraries call this seam. The production expansion is
// the native expression itself: no function table, test symbols or dispatch.
#ifdef LUX_VULKAN_TEST_SEAM
namespace lux::render::vulkan::test
{
    VkResult before(const char *operation) noexcept;
    VkResult after(const char *operation, VkResult result) noexcept;
    void destroyed(const char *operation) noexcept;

    template <typename F> VkResult invoke(const char *operation, F &&function) noexcept
    {
        const auto injected = before(operation);
        if (injected != VK_SUCCESS)
        {
            return injected;
        }
        return after(operation, function());
    }
} // namespace lux::render::vulkan::test

#define LUX_NATIVE(name, expression) ::lux::render::vulkan::test::invoke(name, [&] { return (expression); })
#define LUX_DESTROY(name, expression)                                                                                  \
    do                                                                                                                 \
    {                                                                                                                  \
        (expression);                                                                                                  \
        ::lux::render::vulkan::test::destroyed(name);                                                                  \
    } while (false)
#else
#define LUX_NATIVE(name, expression) (expression)
#define LUX_DESTROY(name, expression) (expression)
#endif
