#include <lux/engine/render/RendererContext.hpp>
#include <lux/engine/render/RenderContextView.hpp>          // ExportableBuffer / ExportableTimelineSemaphore (return types)
#include <lux/engine/render/core/VulkanContext.hpp> // DeviceContext, ResourceContext
#include <lux/engine/render/pipeline/PipelineManager.hpp>
#include <lux/engine/render/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/pipeline/ShaderPermutationCompiler.hpp>
#include <lux/engine/render/resources/lifecycle/ResourceRegistry.hpp>
#include <lux/engine/render/resources/TextureResources.hpp> // find<TextureResources>() — builtin bindless set
#include <lux/engine/render/resources/descriptor/DescriptorService.hpp>
#include <lux/engine/render/core/RenderTypes.hpp>            // kMaxFramesInFlight

#include <lux/engine/gapi/vk/DeviceMemory.hpp>   // DeviceMemory::exportHandle (no dtor — adopt-out pattern)
#include <lux/engine/gapi/vk/Semaphore.hpp>      // Semaphore::exportHandle
#include <lux/engine/gapi/vk/ExternalHandle.hpp> // kOpaqueExternal*Type / ExternalHandle

#include <cassert>
#include <cstring>
#include <span>
#include <stdexcept>
#include <string>

namespace lux::render
{
    RenderContext::RenderContext(ResourceContext &res_ctx, CreateInfo info)
        : resource_ctx_(res_ctx)
        , descriptor_layouts_(std::move(info.descriptor_layouts))
        , permutation_compiler_(std::move(info.permutation_compiler))
        , pipeline_mgr_(std::move(info.pipeline_mgr))
        , global_registry_(std::move(info.global_resources))
        , frames_in_flight_(info.frames_in_flight)
    {
        assert(pipeline_mgr_ && "RenderContext: pipeline_mgr must not be null");
        assert(descriptor_layouts_ && "RenderContext: descriptor_layouts must not be null");
        assert(global_registry_ && "RenderContext: global_resources must not be null");

        // Authoritative frames-in-flight gate (release-safe). Per-frame GPU rings
        // are sized to kMaxFramesInFlight and indexed by serial % k; a fif beyond
        // the cap would collide / index out of bounds. Every RenderContext — built
        // by the server OR directly by an embedder/test — passes through here, so
        // this is the single guarantee that no out-of-range fif reaches the rings.
        if (frames_in_flight_ < 1 || frames_in_flight_ > kMaxFramesInFlight)
            throw std::runtime_error(
                "RenderContext: frames_in_flight must be in [1, kMaxFramesInFlight].");

        auto *texture_res  = global_registry_->find<TextureResources>();
        // MeshResources + MaterialResources + ShadingModelRegistry are built LAZILY
        // now (StandardMeshStack / StandardMaterial attach, or first upload — see
        // ensureGlobalMesh|MaterialResources), so they are intentionally NOT required
        // here: a pure-2D / headless server resolves no mesh arena / material stack.
        // LightResources is per-scene now (M1, Plan A) — owned by each RenderScene,
        // not resolved from the global registry here.
        if (!texture_res)
            throw std::runtime_error("RenderContext: failed to resolve builtin GPU resources.");

        vk_device_ = res_ctx.deviceContext().logicalDevice();
        descriptor_service_ = std::make_unique<DescriptorService>(
            vk_device_, res_ctx.descriptorPool());
        pipeline_layout_service_ = std::make_unique<PipelineLayoutService>(vk_device_);

        deferred_destroy_queue_.init(
            res_ctx.deviceContext().vmaAllocator(), vk_device_);
    }

    RenderContext::~RenderContext()
    {
        retire_scheduler_.flushAll();
        deferred_destroy_queue_.flushAll();
    }

    VkDevice RenderContext::device() const noexcept
    {
        return vk_device_;
    }

    DeviceContext &RenderContext::deviceContext() noexcept
    {
        return resource_ctx_.deviceContext();
    }

    VmaAllocator RenderContext::vmaAllocator() const noexcept
    {
        return resource_ctx_.deviceContext().vmaAllocator();
    }

    // ── External-memory interop (CUDA<->Vulkan zero-copy) ────────────────
    // Domain-neutral: allocate dedicated, exportable Vulkan buffers / timeline
    // semaphores. The engine never knows what the buffer holds — a downstream
    // feature owns the handles and hands them to an external producer (e.g. CUDA).
    //
    // resource_ctx_ is a reference member, so calling its non-const accessors from
    // a const method is well-formed (constness of *this does not propagate through
    // a reference member's referent).
    namespace
    {
        /// Widen a platform external handle to uint64_t for the vulkan.h-free facade.
        inline uint64_t toU64(lux::gapi::vk::ExternalHandle h) noexcept
        {
#if defined(VK_USE_PLATFORM_WIN32_KHR)
            return reinterpret_cast<uint64_t>(h);   // Win32 HANDLE (void*)
#else
            return static_cast<uint64_t>(h);        // POSIX fd (int)
#endif
        }
    } // namespace

    bool RenderContext::supportsExternalMemory() const noexcept
    {
        return resource_ctx_.deviceContext().supportsExternalMemory();
    }

    void RenderContext::deviceUUID(uint8_t out[16]) const noexcept
    {
        const auto uuid = resource_ctx_.deviceContext().deviceUUID(); // std::array<uint8_t, VK_UUID_SIZE>
        std::memcpy(out, uuid.data(), VK_UUID_SIZE);
    }

    uint32_t RenderContext::findMemoryTypeIndex(uint32_t type_filter, uint32_t property_flags) const noexcept
    {
        return resource_ctx_.deviceContext().physicalDevice().findMemoryTypeIndex(
            type_filter, static_cast<VkMemoryPropertyFlags>(property_flags));
    }

    ExportableBuffer RenderContext::createExportableBuffer(uint64_t size, uint32_t usage_flags)
    {
        ExportableBuffer result{};
        if (size == 0 || !supportsExternalMemory())
            return result;

        const VkDevice dev = device();

        // 1. Buffer flagged as exportable. STORAGE_BUFFER|TRANSFER_DST are always on
        //    (an external producer writes it, the engine may also copy into it).
        VkExternalMemoryBufferCreateInfo ext_buffer_info{ VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO };
        ext_buffer_info.handleTypes = lux::gapi::vk::kOpaqueExternalMemoryType;

        VkBufferCreateInfo buffer_info{ VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        buffer_info.pNext       = &ext_buffer_info;
        buffer_info.size        = static_cast<VkDeviceSize>(size);
        buffer_info.usage       = static_cast<VkBufferUsageFlags>(usage_flags)
                                | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT
                                | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        VkBuffer buffer = VK_NULL_HANDLE;
        if (vkCreateBuffer(dev, &buffer_info, nullptr, &buffer) != VK_SUCCESS)
            return result;

        VkMemoryRequirements mem_req{};
        vkGetBufferMemoryRequirements(dev, buffer, &mem_req);

        // 2. DEVICE_LOCAL memory type is mandatory for opaque external import.
        const uint32_t type_index =
            findMemoryTypeIndex(mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type_index == UINT32_MAX)
        {
            vkDestroyBuffer(dev, buffer, nullptr);
            return result;
        }

        // 3. Dedicated, exportable allocation — bypasses VMA entirely. pNext chain:
        //    VkMemoryAllocateInfo -> Export -> Dedicated(->Win32 on Windows).
        VkMemoryDedicatedAllocateInfo dedicated_info{ VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO };
        dedicated_info.buffer = buffer;

        VkExportMemoryAllocateInfo export_info{ VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO };
        export_info.pNext       = &dedicated_info;
        export_info.handleTypes = lux::gapi::vk::kOpaqueExternalMemoryType;

#if defined(VK_USE_PLATFORM_WIN32_KHR)
        VkExportMemoryWin32HandleInfoKHR win32_info{ VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR };
        win32_info.pAttributes = nullptr;
        win32_info.dwAccess    = GENERIC_ALL;
        win32_info.name        = nullptr;
        dedicated_info.pNext   = &win32_info;
#endif

        VkMemoryAllocateInfo alloc_info{ VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc_info.pNext           = &export_info;
        alloc_info.allocationSize  = mem_req.size;
        alloc_info.memoryTypeIndex = type_index;

        // gapi::vk::DeviceMemory has NO destructor, so a local wrapper does not free
        // on scope exit — we capture its handle into the result (caller owns it) and
        // only release() it on an error path.
        lux::gapi::vk::DeviceMemory memory;
        try
        {
            memory = lux::gapi::vk::DeviceMemory(dev, alloc_info); // throws on alloc failure
        }
        catch (...)
        {
            vkDestroyBuffer(dev, buffer, nullptr);
            return result;
        }

        if (vkBindBufferMemory(dev, buffer, memory.handle(), 0) != VK_SUCCESS)
        {
            memory.release(dev);
            vkDestroyBuffer(dev, buffer, nullptr);
            return result;
        }

        const lux::gapi::vk::ExternalHandle handle = memory.exportHandle(dev);
        if (handle == lux::gapi::vk::kInvalidExternalHandle)
        {
            memory.release(dev);
            vkDestroyBuffer(dev, buffer, nullptr);
            return result;
        }

        result.buffer          = buffer;
        result.memory          = memory.handle();   // adopt out of the (dtor-less) wrapper
        result.external_handle = toU64(handle);
        result.actual_size     = static_cast<uint64_t>(mem_req.size);
        return result;
    }

    ExportableTimelineSemaphore RenderContext::createExportableTimelineSemaphore()
    {
        ExportableTimelineSemaphore result{};
        if (!supportsExternalMemory())
            return result;

        const VkDevice dev = device();

        VkSemaphoreTypeCreateInfo type_info{ VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO };
        type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type_info.initialValue  = 0;

        VkExportSemaphoreCreateInfo export_info{ VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO };
        export_info.pNext       = &type_info;
        export_info.handleTypes = lux::gapi::vk::kOpaqueExternalSemaphoreType;

#if defined(VK_USE_PLATFORM_WIN32_KHR)
        VkExportSemaphoreWin32HandleInfoKHR win32_info{ VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR };
        win32_info.pAttributes = nullptr;
        win32_info.dwAccess    = GENERIC_ALL;
        win32_info.name        = nullptr;
        type_info.pNext        = &win32_info;
#endif

        VkSemaphoreCreateInfo sem_info{ VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        sem_info.pNext = &export_info;

        // gapi::vk::Semaphore is likewise destructor-less — adopt-out / release-on-error.
        lux::gapi::vk::Semaphore semaphore;
        try
        {
            semaphore = lux::gapi::vk::Semaphore(dev, sem_info);
        }
        catch (...)
        {
            return result;
        }

        const lux::gapi::vk::ExternalHandle handle = semaphore.exportHandle(dev);
        if (handle == lux::gapi::vk::kInvalidExternalHandle)
        {
            semaphore.release(dev);
            return result;
        }

        result.semaphore       = semaphore.handle();
        result.external_handle = toU64(handle);
        return result;
    }
} // namespace lux::render
