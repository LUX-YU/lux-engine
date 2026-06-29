#pragma once
#include <vulkan/vulkan.h>
#include <memory>
#include <cassert>
#include <cstdio>

// 简单的 VkResult 转字符串；根据需要补全
static const char* vk_result_to_string(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_NOT_READY: return "VK_NOT_READY";
        case VK_TIMEOUT: return "VK_TIMEOUT";
        case VK_EVENT_SET: return "VK_EVENT_SET";
        case VK_EVENT_RESET: return "VK_EVENT_RESET";
        case VK_INCOMPLETE: return "VK_INCOMPLETE";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_DEVICE_LOST: return "VK_ERROR_DEVICE_LOST";
        case VK_ERROR_MEMORY_MAP_FAILED: return "VK_ERROR_MEMORY_MAP_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "VK_ERROR_FEATURE_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        case VK_ERROR_TOO_MANY_OBJECTS: return "VK_ERROR_TOO_MANY_OBJECTS";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "VK_ERROR_FORMAT_NOT_SUPPORTED";
        case VK_ERROR_FRAGMENTED_POOL: return "VK_ERROR_FRAGMENTED_POOL";
        default: return "VK_ERROR_UNKNOWN";
    }
}

#if defined LUX_PLATFORM_GAPI_DISABLE_EXCEPTION
    /// Invoke a Vulkan API. Behaviour on a non-`VK_SUCCESS` result:
    ///
    /// - **`VK_ERROR_DEVICE_LOST`** — silently ignored. Once the device is
    ///   lost, *every* subsequent Vulkan call on it returns the same code,
    ///   so logging each one just spams stderr with N copies of the same
    ///   fact, and asserting blocks shutdown without surfacing anything
    ///   new. The intentionally-lost-device signal belongs to the layer
    ///   that detected it (validation callback / debug utils messenger),
    ///   not to a per-call macro. Enable
    ///   `EditorConfig::enable_vulkan_validation = true` when actually
    ///   hunting one of these — the debug callback fires before the
    ///   cascade and gives you the specific spec violation.
    /// - **anything else** — print a structured diagnostic, then assert.
    ///   This still catches genuinely unexpected results (OOM, surface-
    ///   lost, etc.) loudly during development.
    #define VK_FUNC_INVOKE(func, error_msg, ...)                                 \
    do {                                                                         \
        VkResult err = func(__VA_ARGS__);                                        \
        if (err != VK_SUCCESS && err != VK_ERROR_DEVICE_LOST) {                  \
            std::fprintf(stderr,                                                 \
                "[Vulkan] %s failed: %s (%d)\n  at %s:%d  call: %s\n",           \
                #func, vk_result_to_string(err), (int)err,                       \
                __FILE__, __LINE__, #func "(" #__VA_ARGS__ ")");                 \
            /* 打印完再断言；NDEBUG 下可考虑改为 std::abort() */                 \
            assert(false && "Vulkan call failed");                               \
        }                                                                        \
    } while (0);
#else
    #include <stdexcept>
    #include <string>
    #define VK_FUNC_INVOKE(func, error_msg, ...)                                 \
    do {                                                                         \
        VkResult err = func(__VA_ARGS__);                                        \
        if (err != VK_SUCCESS) {                                                 \
            char buf[256];                                                       \
            std::snprintf(buf, sizeof(buf),                                      \
                "%s: %s (%d) at %s:%d",                                          \
                error_msg, vk_result_to_string(err), (int)err,                   \
                __FILE__, __LINE__);                                             \
            throw std::runtime_error(buf);                                       \
        }                                                                        \
    } while (0);
#endif

