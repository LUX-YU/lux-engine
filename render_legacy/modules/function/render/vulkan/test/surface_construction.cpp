#define VK_USE_PLATFORM_WIN32_KHR
#define NOMINMAX
#include <cassert>
#include <cstdio>
#include <lux/engine/gapi/vk/Instance.hpp>
#include <malloc.h>
#include <map>
#include <type_traits>
#include <utility>
#include <vulkan/vulkan.h>

namespace
{
    bool reject_surface{};
    unsigned created{}, destroyed{};

    struct SurfaceInfo
    {
        VkInstance instance;
        const VkAllocationCallbacks* allocator;
    };

    std::map<VkSurfaceKHR, SurfaceInfo> surfaces;
    unsigned native_calls{};

    void* VKAPI_CALL allocate(void*, size_t size, size_t alignment, VkSystemAllocationScope)
    {
        return _aligned_malloc(size, alignment);
    }

    void* VKAPI_CALL reallocate(void*, void* original, size_t size, size_t alignment, VkSystemAllocationScope)
    {
        return _aligned_realloc(original, size, alignment);
    }

    void VKAPI_CALL freeAllocation(void*, void* memory)
    {
        _aligned_free(memory);
    }

    VkResult VKAPI_CALL createSurface(
        VkInstance instance,
        const VkWin32SurfaceCreateInfoKHR* info,
        const VkAllocationCallbacks* allocator,
        VkSurfaceKHR* output
    )
    {
        ++native_calls;
        if (reject_surface)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vkCreateWin32SurfaceKHR(instance, info, allocator, output);
        if (result == VK_SUCCESS)
        {
            assert(surfaces.emplace(*output, SurfaceInfo{instance, allocator}).second);
            ++created;
        }
        return result;
    }

    void VKAPI_CALL destroySurface(VkInstance instance, VkSurfaceKHR surface, const VkAllocationCallbacks* allocator)
    {
        if (surface)
        {
            assert(
                surfaces.contains(surface) && surfaces.at(surface).instance == instance &&
                surfaces.at(surface).allocator == allocator
            );
            surfaces.erase(surface);
            ++destroyed;
        }
        vkDestroySurfaceKHR(instance, surface, allocator);
    }
} // namespace

#define vkCreateWin32SurfaceKHR createSurface
#define vkDestroySurfaceKHR destroySurface
#include "../src/gpu/RenderSurface.cpp"
#undef vkCreateWin32SurfaceKHR
#undef vkDestroySurfaceKHR

int main()
{
    using lux::render::RenderSurface;
    const char* extensions[]{VK_KHR_SURFACE_EXTENSION_NAME, VK_KHR_WIN32_SURFACE_EXTENSION_NAME};
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.enabledExtensionCount = 2;
    info.ppEnabledExtensionNames = extensions;
    auto instance = lux::gapi::vk::Instance::create(info);
    assert(instance);
    const auto window = CreateWindowExW(
        0,
        L"STATIC",
        L"Surface ownership",
        WS_OVERLAPPEDWINDOW,
        0,
        0,
        64,
        64,
        nullptr,
        nullptr,
        GetModuleHandleW(nullptr),
        nullptr
    );
    assert(window);
    const auto native = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(window));
    static_assert(!std::is_copy_constructible_v<RenderSurface>);
    static_assert(!std::is_copy_assignable_v<RenderSurface>);
    static_assert(std::is_nothrow_move_constructible_v<RenderSurface>);
    static_assert(std::is_nothrow_move_assignable_v<RenderSurface>);
    VkAllocationCallbacks allocator{};
    allocator.pfnAllocation = allocate;
    allocator.pfnReallocation = reallocate;
    allocator.pfnFree = freeAllocation;
    {
        const auto missing_window = RenderSurface::create(0, {64, 64}, *instance);
        assert(
            !missing_window && lux::render::isError<lux::render::err::internal::InvalidArgument>(missing_window.error())
        );
        const lux::gapi::vk::Instance empty;
        const auto missing_instance = RenderSurface::create(native, {64, 64}, empty);
        assert(
            !missing_instance &&
            lux::render::isError<lux::render::err::internal::InvalidArgument>(missing_instance.error())
        );
        assert(native_calls == 0 && surfaces.empty());
    }
    {
        auto accepted = RenderSurface::create(native, {64, 64}, *instance, &allocator);
        assert(accepted && surfaces.size() == 1);
        const VkSurfaceKHR original = *accepted;
        reject_surface = true;
        const auto rejected = RenderSurface::create(native, {128, 128}, *instance, &allocator);
        assert(!rejected && lux::render::isError<lux::render::err::device::VulkanCallFailed>(rejected.error()));
        assert(rejected.error().args[0] == lux::render::encodeVkResult(VK_ERROR_OUT_OF_DEVICE_MEMORY));
        assert(surfaces.contains(original) && static_cast<VkSurfaceKHR>(*accepted) == original);
        assert(accepted->extent().width == 64 && accepted->extent().height == 64);
        reject_surface = false;
        auto candidate = RenderSurface::create(native, {0, 128}, *instance);
        assert(candidate && surfaces.size() == 2);
        assert(candidate->extent().width == 1 && candidate->extent().height == 128);
        const VkSurfaceKHR replacement = *candidate;
        *accepted = std::move(*candidate);
        assert(!surfaces.contains(original) && surfaces.contains(replacement) && surfaces.size() == 1);
        assert(static_cast<VkSurfaceKHR>(*candidate) == VK_NULL_HANDLE);
        *accepted = std::move(*accepted);
        RenderSurface moved(std::move(*accepted));
        assert(static_cast<VkSurfaceKHR>(moved) == replacement);
        assert(static_cast<VkSurfaceKHR>(*accepted) == VK_NULL_HANDLE);
        moved.reset();
        moved.reset();
        assert(surfaces.empty());
        for (unsigned cycle = 0; cycle < 30; ++cycle)
        {
            auto owner = RenderSurface::create(native, {64, 64}, *instance, &allocator);
            assert(owner && surfaces.size() == 1);
        }
        assert(surfaces.empty());
    }
    assert(created == destroyed);
    assert(DestroyWindow(window));
    std::puts("Surface construction: native failure preserves accepted owner, exact errors, retry, custom allocators, "
              "moves and release PASS");
}
