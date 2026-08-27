#pragma once
/**
 * @file vk_fwd.hpp
 * @brief Vulkan HANDLE forward declarations — without pulling <vulkan/vulkan.h>.
 *
 * Part of the RenderFeature SDK "three-tier" surface policy:
 *   - neutral types (ETextureFormat, ...) where cheap. NOTE: this tier covers
 *     RESOURCE description (formats travel the comm protocol) but NOT pipeline
 *     STATE — the neutral pipeline-state enums + GraphicsPipelineDesc + their
 *     Vk conversion table were deleted as never-wired: registerGraphics() takes
 *     the internal GraphicsPipelineTemplate directly, and the surface below is
 *     Vulkan-typed throughout. Re-introduce them only alongside a real RHI
 *     abstraction that the whole view surface adopts;
 *   - forward-declared Vulkan HANDLES here, so SDK authoring headers that only
 *     traffic handles (record context, IRenderContextView returns, shader/layout
 *     handles) do NOT force <vulkan/vulkan.h> onto a consumer that
 *     does not otherwise need it;
 *   - raw Vulkan (the full header) only inside feature .cpp recorders (vkCmd*) and
 *     the interop boundary (VkImage/VkSemaphore export), where it is unavoidable.
 *
 * Vulkan handles are opaque pointers (dispatchable) or pointer/uint64 (non-
 * dispatchable). We replicate Vulkan's OWN handle macros verbatim and guard them
 * with `#if !defined(...)`, so this header is order-independent with respect to
 * <vulkan/vulkan.h>:
 *   - included BEFORE vulkan.h: vulkan.h sees the macros already defined, skips them,
 *     and re-`typedef`s the same handles (a same-type typedef redefinition is legal C++);
 *   - included AFTER  vulkan.h: the macros are already defined, we skip defining them,
 *     and our typedefs are identical to the ones vulkan.h already emitted.
 * Either way the SAME concrete type is seen across all TUs (no ODR / type-mismatch).
 */

#ifndef VK_DEFINE_HANDLE
#define VK_DEFINE_HANDLE(object) typedef struct object##_T* object;
#endif

#if !defined(VK_DEFINE_NON_DISPATCHABLE_HANDLE)
#if defined(__LP64__) || defined(_WIN64) || (defined(__x86_64__) && !defined(__ILP32__)) || defined(_M_X64) ||         \
    defined(__ia64) || defined(_M_IA64) || defined(__aarch64__) || defined(__powerpc64__)
#define VK_DEFINE_NON_DISPATCHABLE_HANDLE(object) typedef struct object##_T* object;
#else
#define VK_DEFINE_NON_DISPATCHABLE_HANDLE(object) typedef uint64_t object;
#endif
#endif

#include <cstdint> // uint64_t for the 32-bit non-dispatchable handle definition

// -- Dispatchable handles (always pointers) --
VK_DEFINE_HANDLE(VkInstance)
VK_DEFINE_HANDLE(VkPhysicalDevice)
VK_DEFINE_HANDLE(VkDevice)
VK_DEFINE_HANDLE(VkQueue)
VK_DEFINE_HANDLE(VkCommandBuffer)

// -- Non-dispatchable handles (pointer on 64-bit, uint64 on 32-bit) --
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkBuffer)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkImage)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkImageView)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDeviceMemory)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSampler)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkShaderModule)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkPipelineLayout)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDescriptorSetLayout)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDescriptorSet)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkDescriptorPool)
VK_DEFINE_NON_DISPATCHABLE_HANDLE(VkSemaphore)
