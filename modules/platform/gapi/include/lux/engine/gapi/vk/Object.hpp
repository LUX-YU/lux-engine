#pragma once
// ============================================================================
//  Ownership model for the lux::gapi::vk wrappers — read before "fixing" one.
//
//  Most legacy types in this folder are thin, non-owning handle carriers. New
//  wrappers must make ownership explicit: low-cardinality objects may store the
//  parent handle and own their resource (Swapchain/Semaphore); high-cardinality
//  carriers need a consumer-level RAII owner that also stores the parent.
//
//  A type explicitly kept as a carrier contains only the Vulkan handle (plus
//  create-time metadata a caller would otherwise re-query). Destroying it needs
//  its VkDevice/VkInstance, so making every sampler, image view and per-slot
//  object self-owning would grow the hot arrays that hold thousands of them.
//  Those carriers stay handle-sized; their consumer-level owner stores the
//  parent once and applies RAII to the whole collection.
//
//  A non-owning carrier obligates its CALLER to:
//
//    - own the lifetime explicitly. The caller knows the device, so the caller
//      is where RAII belongs — wrap these in a type that does have a destructor
//      (lux::render::RenderSurface and SwapchainImageViews are the shape: store
//      the parent once and release from the owner destructor);
//    - never move-assign onto a live handle. Move assignment overwrites without
//      releasing — it cannot release, it has no device — so the old handle would
//      leak. Reset or release first;
//    - remember that a forgotten release is a SILENT leak here. There is no
//      assert to catch it, by the same size argument.
//
//  A type that stores its parent handle is an owner instead: it must destroy in
//  its destructor and make move-assignment release-before-adopt.
// ============================================================================
#include <vulkan/vulkan.h>
#include <cassert>
#include <cstdio>

// A simple VkResult-to-string conversion; extend as needed
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

/// Legacy Vulkan invocation shim. It never throws: recoverable code must not
/// use this macro at all and must return the exact VkResult through expected.
/// Remaining uses are tracked by the exception-removal work package.
///
/// - `VK_ERROR_DEVICE_LOST` is left to the detecting layer to report once;
/// - other failures print a diagnostic and assert in diagnostic builds.
#define VK_FUNC_INVOKE(func, error_msg, ...)                                     \
do {                                                                             \
    VkResult err = func(__VA_ARGS__);                                            \
    if (err != VK_SUCCESS && err != VK_ERROR_DEVICE_LOST) {                      \
        std::fprintf(stderr,                                                     \
            "[Vulkan] %s failed: %s (%d)\n  at %s:%d  call: %s\n",               \
            #func, vk_result_to_string(err), (int)err,                           \
            __FILE__, __LINE__, #func "(" #__VA_ARGS__ ")");                     \
        assert(false && "Vulkan call failed");                                   \
    }                                                                            \
} while (0)

