#pragma once

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <lux/engine/render/vulkan/device/Device.hpp>

namespace foundation_test
{
    using namespace lux::render;
    using namespace lux::render::vulkan;

    inline void require(bool condition, int line)
    {
        if (!condition)
        {
            std::fprintf(stderr, "FAILED foundation assertion line %d\n", line);
            std::abort();
        }
    }

#define CHECK(condition) ::foundation_test::require(static_cast<bool>(condition), __LINE__)

    template <typename T> T take(RenderResult<T> result)
    {
        if (!result)
        {
            std::fprintf(
                stderr,
                "Native result error=%llu args=%llu,%llu,%llu\n",
                static_cast<unsigned long long>(result.error().type),
                static_cast<unsigned long long>(result.error().args[0]),
                static_cast<unsigned long long>(result.error().args[1]),
                static_cast<unsigned long long>(result.error().args[2])
            );
            std::abort();
        }
        return std::move(*result);
    }

    struct Validation
    {
        std::atomic<unsigned> errors{};
        std::atomic<unsigned> warnings{};
    };

    inline VKAPI_ATTR VkBool32 VKAPI_CALL diagnostic(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT,
        const VkDebugUtilsMessengerCallbackDataEXT *data,
        void *user
    )
    {
        auto &validation = *static_cast<Validation *>(user);
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            ++validation.errors;
        else
            ++validation.warnings;
        std::fprintf(stderr, "VALIDATION %u %s\n", severity, data->pMessage);
        return VK_FALSE;
    }

    inline VulkanInstance instance(Validation &validation)
    {
        return take(VulkanInstance::create({{}, true, diagnostic, &validation}));
    }
} // namespace foundation_test
