#pragma once
#include <vulkan/vulkan.h>

namespace lux::gapi::vk
{
    // ─────────────────────────────────────────────────────────────────────────
    //  Platform-neutral external resource handle (CUDA / other-API interop)
    // ─────────────────────────────────────────────────────────────────────────
    //
    // External memory/semaphore handles are inherently platform-typed, but the
    // SHAPE is identical across platforms, so we expose ONE neutral handle type +
    // the matching Vulkan "opaque" handle-type bits, chosen at compile time:
    //
    //   Windows : ExternalHandle = void* (Win32 NT HANDLE), OPAQUE_WIN32 bits.
    //   POSIX   : ExternalHandle = int   (file descriptor),  OPAQUE_FD bits.
    //
    // Callers (and the gapi export helpers) use ExternalHandle /
    // kOpaqueExternalMemoryType / kOpaqueExternalSemaphoreType and never name a
    // platform-specific Vulkan symbol — matching the engine's GLFW-mediated,
    // platform-agnostic style. The importing API (e.g. CUDA) selects its matching
    // handle type (cudaExternalMemoryHandleTypeOpaqueWin32 vs ...OpaqueFd) the same
    // way on its side.
    //
    // Handle ownership: the value returned by an export helper is owned by the
    // CALLER — close it (CloseHandle on Win32 / ::close on POSIX) after the
    // importer has duplicated it (CUDA dup's on import).

#if defined(VK_USE_PLATFORM_WIN32_KHR)
    using ExternalHandle = void*; // Win32 NT HANDLE
    inline constexpr ExternalHandle kInvalidExternalHandle = nullptr;
    inline constexpr VkExternalMemoryHandleTypeFlagBits kOpaqueExternalMemoryType =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_WIN32_BIT;
    inline constexpr VkExternalSemaphoreHandleTypeFlagBits kOpaqueExternalSemaphoreType =
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
    using ExternalHandle = int; // POSIX file descriptor
    inline constexpr ExternalHandle kInvalidExternalHandle = -1;
    inline constexpr VkExternalMemoryHandleTypeFlagBits kOpaqueExternalMemoryType =
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
    inline constexpr VkExternalSemaphoreHandleTypeFlagBits kOpaqueExternalSemaphoreType =
        VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif

    /// True when this build targets a platform with a concrete external-handle path
    /// (Win32 or POSIX-fd). Always true today; guards future exotic targets.
    inline constexpr bool kHasExternalHandleSupport = true;

} // namespace lux::gapi::vk
