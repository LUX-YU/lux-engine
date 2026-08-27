#include <lux/engine/render/gpu/RenderContext.hpp>
#include <lux/engine/render/gpu/ExternalInterop.hpp> // ExportableBuffer / ExportableTimelineSemaphore
#include <lux/engine/render/gpu/VulkanContext.hpp>   // DeviceContext, ResourceContext
#include <lux/engine/render/gpu/pipeline/PipelineManager.hpp>
#include <lux/engine/render/gpu/pipeline/GeneralDescriptorSetLayout.hpp>
#include <lux/engine/render/gpu/pipeline/PipelineLayoutService.hpp>
#include <lux/engine/render/gpu/lifecycle/ResourceRegistry.hpp>
#include <lux/engine/render/gpu/descriptor/DescriptorService.hpp>
#include <lux/engine/function/render/client/core/RenderTypes.hpp> // kMaxFramesInFlight

#include <lux/engine/gapi/vk/Semaphore.hpp>      // Semaphore::exportHandle
#include <lux/engine/gapi/vk/ExternalHandle.hpp> // kOpaqueExternal*Type / ExternalHandle

#include <cstring>
#include <span>
#include <utility>

namespace lux::render
{
    Expected<std::shared_ptr<RenderContext>> RenderContext::create(ResourceContext& res_ctx, CreateInfo info)
    {
        if (info.frames_in_flight < 1 || info.frames_in_flight > kMaxFramesInFlight)
        {
            return renderFailure<err::device::InvalidFramesInFlight>(info.frames_in_flight, kMaxFramesInFlight);
        }
        if (!info.pipeline_mgr || !info.descriptor_layouts || !info.global_resources)
        {
            return renderFailure<err::internal::InvalidArgument>();
        }

        // Project allocation policy: exhaustion of the process heap is fatal,
        // matching std::vector/string throughout the renderer. Expected covers
        // configuration and Vulkan/domain failures; it is not an OOM recovery
        // mechanism. No exception is used as renderer control flow here.
        return std::make_shared<RenderContext>(ConstructionKey{}, res_ctx, std::move(info));
    }

    RenderContext::RenderContext(ConstructionKey, ResourceContext& res_ctx, CreateInfo info)
        : resource_ctx_(res_ctx), descriptor_layouts_(std::move(info.descriptor_layouts)),
          pipeline_mgr_(std::move(info.pipeline_mgr)), global_registry_(std::move(info.global_resources)),
          frames_in_flight_(info.frames_in_flight), capacity_plan_(info.capacity_plan),
          texture_sampling_catalog_(builtinTextureSamplingRepresentationCatalog())
    {
        // create() is the sole construction boundary and has already validated
        // the frame ring and all three required owning services.

        // (原先这里有一句 find<TextureResources>() + "未解析到内建 GPU 资源" 断言。
        //  已删除,理由有三 —— 它既越层又无效:
        //   1. 越层:RenderContext 是 L1 合成根,不该认识 L3 的纹理域。
        //   2. 不可能触发:emplace<TextureResources>() 就在同一个 RenderServer::init
        //      里、构造本对象之前 48 行,不存在"未 emplace"的路径。
        //   3. 无效:资源注册表没有 erase(失败 init 的对象照样留在表里),所以
        //      init() 失败时该指针依然非空、断言照样通过 —— 它检测不到它声称要
        //      检测的东西。真正的缺口是 TextureResources::init() 的 bool 返回值
        //      被忽略,已在 RenderServer 的 emplace 处补上检查。
        //  其余全局资源(Mesh/Material/ShadingModel)本就是惰性建的,Light 是每场景
        //  的 —— 合成根一个都不该在这里解析。)

        vk_device_ = res_ctx.deviceContext().logicalDevice();
        descriptor_service_ = std::make_unique<DescriptorService>(vk_device_, res_ctx.descriptorPool());
        pipeline_layout_service_ = std::make_unique<PipelineLayoutService>(
            vk_device_,
            res_ctx.deviceContext().physicalDevice().properties().properties.limits.maxBoundDescriptorSets
        );

        // Reflected-layout environment injection: when a template arrives
        // without a pipeline_layout, registerGraphicsTemplate builds one by
        // routing on the contract's frequency (shared domains resolve to a
        // singleton; the FEATURE domain is generated from reflection +
        // contract flags via DescriptorService).
        pipeline_mgr_->setReflectedLayoutEnv(*descriptor_layouts_, *descriptor_service_, *pipeline_layout_service_);

        deferred_destroy_queue_.init(res_ctx.deviceContext().vmaAllocator(), vk_device_);
    }

    RenderContext::~RenderContext()
    {
        global_transfer_scheduler_.shutdown();
        retire_scheduler_.flushAll();
        deferred_destroy_queue_.flushAll();
    }

    void RenderContext::setErrorSink(RenderErrorSink* sink) noexcept
    {
        error_sink_ = sink;
        // 管线管理器自己也要上报(创建失败、变体预算耗尽),而它比本上下文更早
        // 构造出来 —— 在这里转交,调用方只需接一次线。
        if (pipeline_mgr_)
            pipeline_mgr_->setErrorSink(sink);
    }

    VkDevice RenderContext::device() const noexcept
    {
        return vk_device_;
    }

    DeviceContext& RenderContext::deviceContext() noexcept
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
        class ExportableBufferCandidate final
        {
        public:
            explicit ExportableBufferCandidate(VkDevice device) noexcept : device_(device)
            {
            }

            ~ExportableBufferCandidate() noexcept
            {
                if (buffer != VK_NULL_HANDLE)
                    vkDestroyBuffer(device_, buffer, nullptr);
                if (memory != VK_NULL_HANDLE)
                    vkFreeMemory(device_, memory, nullptr);
            }

            ExportableBufferCandidate(const ExportableBufferCandidate&) = delete;
            ExportableBufferCandidate& operator=(const ExportableBufferCandidate&) = delete;

            [[nodiscard]] ExportableBuffer publish(std::uint64_t external_handle, std::uint64_t actual_size) noexcept
            {
                ExportableBuffer result{
                    .buffer = std::exchange(buffer, VkBuffer{}),
                    .memory = std::exchange(memory, VkDeviceMemory{}),
                    .external_handle = external_handle,
                    .actual_size = actual_size,
                };
                return result;
            }

            VkBuffer buffer{VK_NULL_HANDLE};
            VkDeviceMemory memory{VK_NULL_HANDLE};

        private:
            VkDevice device_{VK_NULL_HANDLE};
        };

        /// Widen a platform external handle to uint64_t for the vulkan.h-free facade.
        inline uint64_t toU64(lux::gapi::vk::ExternalHandle h) noexcept
        {
#if defined(VK_USE_PLATFORM_WIN32_KHR)
            return reinterpret_cast<uint64_t>(h); // Win32 HANDLE (void*)
#else
            return static_cast<uint64_t>(h); // POSIX fd (int)
#endif
        }

        [[nodiscard]] VkResult
        exportMemoryHandle(VkDevice device, VkDeviceMemory memory, lux::gapi::vk::ExternalHandle& out_handle) noexcept
        {
            out_handle = lux::gapi::vk::kInvalidExternalHandle;
#if defined(VK_USE_PLATFORM_WIN32_KHR)
            auto fn = reinterpret_cast<PFN_vkGetMemoryWin32HandleKHR>(
                vkGetDeviceProcAddr(device, "vkGetMemoryWin32HandleKHR")
            );
            if (fn == nullptr)
                return VK_ERROR_EXTENSION_NOT_PRESENT;

            const VkMemoryGetWin32HandleInfoKHR info{
                .sType = VK_STRUCTURE_TYPE_MEMORY_GET_WIN32_HANDLE_INFO_KHR,
                .pNext = nullptr,
                .memory = memory,
                .handleType = lux::gapi::vk::kOpaqueExternalMemoryType,
            };
            HANDLE handle = nullptr;
            const VkResult result = fn(device, &info, &handle);
            if (result == VK_SUCCESS)
                out_handle = handle;
            return result;
#else
            auto fn = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR"));
            if (fn == nullptr)
                return VK_ERROR_EXTENSION_NOT_PRESENT;

            const VkMemoryGetFdInfoKHR info{
                .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
                .pNext = nullptr,
                .memory = memory,
                .handleType = lux::gapi::vk::kOpaqueExternalMemoryType,
            };
            int fd = -1;
            const VkResult result = fn(device, &info, &fd);
            if (result == VK_SUCCESS)
                out_handle = fd;
            return result;
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
            type_filter,
            static_cast<VkMemoryPropertyFlags>(property_flags)
        );
    }

    Expected<ExportableBuffer> RenderContext::createExportableBuffer(uint64_t size, uint32_t usage_flags)
    {
        ExportableBuffer result{};
        if (size == 0)
            return renderFailure<err::internal::InvalidArgument>();
        if (!supportsExternalMemory())
            return result;

        const VkDevice dev = device();
        ExportableBufferCandidate candidate(dev);

        // 1. Buffer flagged as exportable. STORAGE_BUFFER|TRANSFER_DST are always on
        //    (an external producer writes it, the engine may also copy into it).
        VkExternalMemoryBufferCreateInfo ext_buffer_info{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO};
        ext_buffer_info.handleTypes = lux::gapi::vk::kOpaqueExternalMemoryType;

        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.pNext = &ext_buffer_info;
        buffer_info.size = static_cast<VkDeviceSize>(size);
        buffer_info.usage = static_cast<VkBufferUsageFlags>(usage_flags) | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                            VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        buffer_info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

        const VkResult buffer_created = vkCreateBuffer(dev, &buffer_info, nullptr, &candidate.buffer);
        if (buffer_created != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(buffer_created));
        }
        if (candidate.buffer == VK_NULL_HANDLE)
            return renderFailure<err::device::VulkanObjectCreationFailed>();

        VkMemoryRequirements mem_req{};
        vkGetBufferMemoryRequirements(dev, candidate.buffer, &mem_req);

        // 2. DEVICE_LOCAL memory type is mandatory for opaque external import.
        const uint32_t type_index = findMemoryTypeIndex(mem_req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
        if (type_index == UINT32_MAX)
            return renderFailure<err::memory::GpuAllocationFailed>();

        // 3. Dedicated, exportable allocation — bypasses VMA entirely. pNext chain:
        //    VkMemoryAllocateInfo -> Export -> Dedicated(->Win32 on Windows).
        VkMemoryDedicatedAllocateInfo dedicated_info{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
        dedicated_info.buffer = candidate.buffer;

        VkExportMemoryAllocateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
        export_info.pNext = &dedicated_info;
        export_info.handleTypes = lux::gapi::vk::kOpaqueExternalMemoryType;

#if defined(VK_USE_PLATFORM_WIN32_KHR)
        VkExportMemoryWin32HandleInfoKHR win32_info{VK_STRUCTURE_TYPE_EXPORT_MEMORY_WIN32_HANDLE_INFO_KHR};
        win32_info.pAttributes = nullptr;
        win32_info.dwAccess = GENERIC_ALL;
        win32_info.name = nullptr;
        dedicated_info.pNext = &win32_info;
#endif

        VkMemoryAllocateInfo alloc_info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc_info.pNext = &export_info;
        alloc_info.allocationSize = mem_req.size;
        alloc_info.memoryTypeIndex = type_index;

        const VkResult allocated = vkAllocateMemory(dev, &alloc_info, nullptr, &candidate.memory);
        if (allocated != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(allocated));
        }
        if (candidate.memory == VK_NULL_HANDLE)
            return renderFailure<err::device::VulkanObjectCreationFailed>();

        const VkResult bound = vkBindBufferMemory(dev, candidate.buffer, candidate.memory, 0);
        if (bound != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(bound));
        }

        lux::gapi::vk::ExternalHandle handle = lux::gapi::vk::kInvalidExternalHandle;
        const VkResult exported = exportMemoryHandle(dev, candidate.memory, handle);
        if (exported != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(exported));
        }
        if (handle == lux::gapi::vk::kInvalidExternalHandle)
            return renderFailure<err::device::VulkanObjectCreationFailed>();

        return candidate.publish(toU64(handle), static_cast<uint64_t>(mem_req.size));
    }

    Expected<ExportableTimelineSemaphore> RenderContext::createExportableTimelineSemaphore()
    {
        ExportableTimelineSemaphore result{};
        if (!supportsExternalMemory())
            return result;

        const VkDevice dev = device();

        VkSemaphoreTypeCreateInfo type_info{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
        type_info.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
        type_info.initialValue = 0;

        VkExportSemaphoreCreateInfo export_info{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
        export_info.pNext = &type_info;
        export_info.handleTypes = lux::gapi::vk::kOpaqueExternalSemaphoreType;

#if defined(VK_USE_PLATFORM_WIN32_KHR)
        VkExportSemaphoreWin32HandleInfoKHR win32_info{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
        win32_info.pAttributes = nullptr;
        win32_info.dwAccess = GENERIC_ALL;
        win32_info.name = nullptr;
        type_info.pNext = &win32_info;
#endif

        VkSemaphoreCreateInfo sem_info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        sem_info.pNext = &export_info;

        VkSemaphore raw_semaphore = VK_NULL_HANDLE;
        const VkResult created = vkCreateSemaphore(dev, &sem_info, nullptr, &raw_semaphore);
        if (created != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(created));
        }
        if (raw_semaphore == VK_NULL_HANDLE)
            return renderFailure<err::device::VulkanObjectCreationFailed>();

        // Thin carrier; this function remains the owning transaction until the
        // raw handle is published in the result below.
        auto semaphore = lux::gapi::vk::Semaphore::adopt(dev, raw_semaphore);

        lux::gapi::vk::ExternalHandle handle = lux::gapi::vk::kInvalidExternalHandle;
        const VkResult exported = semaphore.exportHandle(handle);
        if (exported != VK_SUCCESS)
        {
            return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(exported));
        }
        if (handle == lux::gapi::vk::kInvalidExternalHandle)
            return renderFailure<err::device::VulkanObjectCreationFailed>();

        result.semaphore = semaphore.release();
        result.external_handle = toU64(handle);
        return result;
    }
} // namespace lux::render
