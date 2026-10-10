#include <lux/engine/gapi/vk/vk.hpp>
#include <lux/engine/render/gpu/memory/StagingBuffer.hpp>
#include <lux/engine/render/gpu/memory/VmaTypes.hpp>

#include <vulkan/vulkan.h>

#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <system_error>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

namespace
{
#if defined(LUX_TRANSFER_THREAD_FAILURE_TEST)
    bool reject_thread{};

    template <class F> std::thread makeTransferThread(F&& function)
    {
        if (reject_thread)
        {
            throw std::system_error(std::make_error_code(std::errc::resource_unavailable_try_again));
        }
        return std::thread(std::forward<F>(function));
    }
#endif

    std::mutex staging_mutex;
    std::unordered_map<VkBuffer, VmaAllocation> staging_allocations;
    std::atomic<bool> reject_mapping{};

    VkResult createStaging(
        VmaAllocator allocator,
        const VkBufferCreateInfo* buffer_info,
        const VmaAllocationCreateInfo* allocation_info,
        VkBuffer* buffer,
        VmaAllocation* allocation,
        VmaAllocationInfo* info
    )
    {
        const auto result = vmaCreateBuffer(allocator, buffer_info, allocation_info, buffer, allocation, info);
        if (result == VK_SUCCESS)
        {
            const std::lock_guard lock(staging_mutex);
            assert(staging_allocations.emplace(*buffer, *allocation).second);
            if (reject_mapping.load())
            {
                info->pMappedData = nullptr;
            }
        }
        return result;
    }

    void destroyStaging(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation)
    {
        const std::lock_guard lock(staging_mutex);
        const auto it = staging_allocations.find(buffer);
        assert(it != staging_allocations.end() && it->second == allocation);
        staging_allocations.erase(it);
        vmaDestroyBuffer(allocator, buffer, allocation);
    }

    std::atomic<unsigned> texture_failure{};
    std::atomic<unsigned> texture_rejections{};
    std::unordered_map<VkImage, VmaAllocation> images;
    std::unordered_map<VkImageView, VkImage> views;
    std::unordered_map<VkSampler, VkDevice> samplers;

    VkResult createImage(
        VmaAllocator allocator,
        const VkImageCreateInfo* info,
        const VmaAllocationCreateInfo* allocation_info,
        VkImage* image,
        VmaAllocation* allocation,
        VmaAllocationInfo* mapped
    )
    {
        if (texture_failure.load() == 1)
        {
            ++texture_rejections;
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vmaCreateImage(allocator, info, allocation_info, image, allocation, mapped);
        if (result == VK_SUCCESS)
        {
            const std::lock_guard lock(staging_mutex);
            assert(images.emplace(*image, *allocation).second);
        }
        return result;
    }

    void destroyImage(VmaAllocator allocator, VkImage image, VmaAllocation allocation)
    {
        const std::lock_guard lock(staging_mutex);
        for (const auto& [view, parent] : views)
        {
            assert(parent != image);
        }
        assert(images.at(image) == allocation);
        images.erase(image);
        vmaDestroyImage(allocator, image, allocation);
    }

    VkResult createView(
        VkDevice device,
        const VkImageViewCreateInfo* info,
        const VkAllocationCallbacks* allocator,
        VkImageView* view
    )
    {
        if (texture_failure.load() == 2)
        {
            ++texture_rejections;
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vkCreateImageView(device, info, allocator, view);
        if (result == VK_SUCCESS)
        {
            const std::lock_guard lock(staging_mutex);
            assert(views.emplace(*view, info->image).second);
        }
        return result;
    }

    void destroyView(VkDevice device, VkImageView view, const VkAllocationCallbacks* allocator)
    {
        if (view == VK_NULL_HANDLE)
        {
            return;
        }
        const std::lock_guard lock(staging_mutex);
        assert(views.erase(view) == 1);
        vkDestroyImageView(device, view, allocator);
    }

    VkResult createSampler(
        VkDevice device,
        const VkSamplerCreateInfo* info,
        const VkAllocationCallbacks* allocator,
        VkSampler* sampler
    )
    {
        if (texture_failure.load() == 3)
        {
            ++texture_rejections;
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vkCreateSampler(device, info, allocator, sampler);
        if (result == VK_SUCCESS)
        {
            const std::lock_guard lock(staging_mutex);
            assert(samplers.emplace(*sampler, device).second);
        }
        return result;
    }

    void destroySampler(VkDevice device, VkSampler sampler, const VkAllocationCallbacks* allocator)
    {
        if (sampler == VK_NULL_HANDLE)
        {
            return;
        }
        const std::lock_guard lock(staging_mutex);
        assert(samplers.at(sampler) == device);
        samplers.erase(sampler);
        vkDestroySampler(device, sampler, allocator);
    }

    std::unordered_map<VkSemaphore, VkDevice> semaphores;
    std::unordered_map<VkCommandPool, VkDevice> pools;
    unsigned attempts{}, fail_at{};

    VkResult createSemaphore(
        VkDevice device,
        const VkSemaphoreCreateInfo* info,
        const VkAllocationCallbacks* allocator,
        VkSemaphore* output
    )
    {
        ++attempts;
        *output = VK_NULL_HANDLE;
        if (attempts == fail_at)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vkCreateSemaphore(device, info, allocator, output);
        if (result == VK_SUCCESS)
        {
            assert(semaphores.emplace(*output, device).second);
        }
        return result;
    }

    void destroySemaphore(VkDevice device, VkSemaphore semaphore, const VkAllocationCallbacks* allocator)
    {
        const auto it = semaphores.find(semaphore);
        assert(it != semaphores.end() && it->second == device);
        semaphores.erase(it);
        vkDestroySemaphore(device, semaphore, allocator);
    }

    VkResult createPool(
        VkDevice device,
        const VkCommandPoolCreateInfo* info,
        const VkAllocationCallbacks* allocator,
        VkCommandPool* output
    )
    {
        ++attempts;
        *output = VK_NULL_HANDLE;
        if (attempts == fail_at)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vkCreateCommandPool(device, info, allocator, output);
        if (result == VK_SUCCESS)
        {
            assert(pools.emplace(*output, device).second);
        }
        return result;
    }

    void destroyPool(VkDevice device, VkCommandPool pool, const VkAllocationCallbacks* allocator)
    {
        const auto it = pools.find(pool);
        assert(it != pools.end() && it->second == device);
        pools.erase(it);
        vkDestroyCommandPool(device, pool, allocator);
    }
} // namespace

// Run the actual worker and device path. Only creation faults and destruction accounting are injected.
// clang-format off
#define vmaCreateImage createImage
#define vmaDestroyImage destroyImage
#define vkCreateImageView createView
#define vkDestroyImageView destroyView
#define vkCreateSampler createSampler
#define vkDestroySampler destroySampler
#include "../src/gpu/memory/VmaTypes.cpp"
#define vmaCreateBuffer createStaging
#define vmaDestroyBuffer destroyStaging
#include "../src/gpu/memory/StagingBuffer.cpp"
#define vkCreateSemaphore createSemaphore
#define vkDestroySemaphore destroySemaphore
#define vkCreateCommandPool createPool
#define vkDestroyCommandPool destroyPool
#include <lux/engine/render/gpu/VulkanContext.hpp>
#if defined(LUX_TRANSFER_THREAD_FAILURE_TEST)
#include "GpuTransferPipelineThreadFailure.cpp"
#else
#include "../src/resources/lifecycle/GpuTransferPipeline.cpp"
#endif
#undef vkDestroyCommandPool
#undef vkCreateCommandPool
#undef vkDestroySemaphore
#undef vkCreateSemaphore
#undef vmaDestroyBuffer
#undef vmaCreateBuffer
#undef vmaCreateImage
#undef vmaDestroyImage
#undef vkCreateImageView
#undef vkDestroyImageView
#undef vkCreateSampler
#undef vkDestroySampler
// clang-format on

int main()
{
    using namespace lux::render;
    auto instance_owner = InstanceContext::create({});
    assert(instance_owner);
    auto& instance = **instance_owner;
    auto device_owner = DeviceContext::create(instance, EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED);
    assert(device_owner);
    auto& device = **device_owner;
    static_assert(noexcept(GpuTransferPipeline::create(std::declval<const GpuTransferPipeline::Config&>())));
    static_assert(!std::is_default_constructible_v<GpuTransferPipeline>);
    static_assert(!std::is_move_constructible_v<GpuTransferPipeline>);
    static_assert(!std::is_copy_constructible_v<GpuTransferPipeline>);
    static_assert(!std::is_copy_constructible_v<SampledImage>);
    static_assert(!std::is_copy_assignable_v<SampledImage>);
    static_assert(std::is_nothrow_move_constructible_v<SampledImage>);
    static_assert(std::is_nothrow_move_assignable_v<SampledImage>);
    static_assert(!std::is_copy_constructible_v<TransferCompletion>);
    static_assert(!std::is_copy_assignable_v<TransferCompletion>);
    static_assert(std::is_nothrow_move_constructible_v<TransferCompletion>);
    static_assert(std::is_nothrow_move_assignable_v<TransferCompletion>);
    GpuTransferPipeline::Config config;
    config.device_ctx = &device;
    config.batch_slot_count = 3;
    config.queue_capacity = 4;
    config.result_capacity = 8;
    for (unsigned boundary = 1; boundary <= config.batch_slot_count + 1; ++boundary)
    {
        attempts = 0;
        fail_at = boundary;
        const auto candidate = GpuTransferPipeline::create(config);
        assert(!candidate && isError<err::device::VulkanCallFailed>(candidate.error()));
        assert(attempts == boundary && semaphores.empty() && pools.empty());
    }
    fail_at = 0;
#if defined(LUX_TRANSFER_THREAD_FAILURE_TEST)
    reject_thread = true;
    try
    {
        const auto failure = GpuTransferPipeline::create(config);
        assert(!failure && isError<err::upload::WorkerStartFailed>(failure.error()));
        assert(failure.error().args[0] == static_cast<std::uint32_t>(std::errc::resource_unavailable_try_again));
        assert(failure.error().args[1] == 0u);
        assert(semaphores.empty() && pools.empty());
        std::puts("PASS thread acquisition failure returned after complete native cleanup");
    }
    catch (const std::system_error& error)
    {
        std::fprintf(
            stderr,
            "FAIL thread acquisition escaped factory: code=%d semaphores=%zu pools=%zu\n",
            error.code().value(),
            semaphores.size(),
            pools.size()
        );
        return 1;
    }
    reject_thread = false;
#endif
    bool leaked_staging = false;
    for (unsigned kind = 0; kind < 3; ++kind)
    {
        auto pipeline = GpuTransferPipeline::create(config);
        assert(pipeline);
        reject_mapping.store(true);
        const auto pixels = std::make_shared<std::array<std::byte, 16>>();
        if (kind == 0)
        {
            MeshTransferTask task{};
            task.vbo_bytes = pixels->size();
            task.vbo_data = pixels->data();
            task.data_owner = pixels;
            task.request_id = kind;
            assert((*pipeline)->submitMeshTransfer(std::move(task)));
        }
        else if (kind == 1)
        {
            TextureTransferTask task{};
            task.format = EPixelFormat::RGBA8_UNORM;
            task.total_bytes = pixels->size();
            task.mips[0] = {pixels, pixels->data(), pixels->size(), 2, 2, 0};
            task.request_id = kind;
            assert((*pipeline)->submitTextureTransfer(std::move(task)));
        }
        else
        {
            CubeTransferTask task{};
            task.format = EPixelFormat::RGBA8_UNORM;
            task.face_size = 2;
            task.face_bytes = pixels->size();
            task.request_id = kind;
            for (auto& face : task.faces)
            {
                face = {pixels, pixels->data()};
            }
            assert((*pipeline)->submitCubeTransfer(std::move(task)));
        }
        TransferCompletion completion;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while ((*pipeline)->drainResults(&completion, 1) == 0)
        {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        assert(completion.failed && completion.request_id == kind);
        pipeline->reset();
        reject_mapping.store(false);
        const std::lock_guard lock(staging_mutex);
        std::printf("mapping rejection kind=%u live staging=%zu\n", kind, staging_allocations.size());
        leaked_staging = leaked_staging || !staging_allocations.empty();
        // Clean up only after the original owner/worker has stopped; preserve the observed failure.
        for (const auto& [buffer, allocation] : staging_allocations)
        {
            vmaDestroyBuffer(device.vmaAllocator(), buffer, allocation);
        }
        staging_allocations.clear();
    }
    if (leaked_staging)
    {
        return 1;
    }

    bool leaked_texture = false;
    for (unsigned boundary = 0; boundary < 4; ++boundary)
    {
        for (unsigned kind = 0; kind < 2; ++kind)
        {
            texture_failure.store(boundary);
            texture_rejections.store(0);
            auto pipeline = GpuTransferPipeline::create(config);
            assert(pipeline);
            const auto pixels = std::make_shared<std::array<std::byte, 16>>();
            {
                if (kind == 0)
                {
                    TextureTransferTask task{};
                    task.request_id = 55;
                    task.format = EPixelFormat::RGBA8_UNORM;
                    task.total_bytes = pixels->size();
                    task.mips[0] = {pixels, pixels->data(), pixels->size(), 2, 2, 0};
                    assert((*pipeline)->submitTextureTransfer(std::move(task)));
                }
                else
                {
                    CubeTransferTask task{};
                    task.request_id = 55;
                    task.format = EPixelFormat::RGBA8_UNORM;
                    task.face_size = 2;
                    task.face_bytes = pixels->size();
                    for (auto& face : task.faces)
                    {
                        face = {pixels, pixels->data()};
                    }
                    assert((*pipeline)->submitCubeTransfer(std::move(task)));
                }
                TransferCompletion completion;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                while ((*pipeline)->drainResults(&completion, 1) == 0)
                {
                    assert(std::chrono::steady_clock::now() < deadline);
                    std::this_thread::yield();
                }
                assert(completion.failed == (boundary != 0));
                assert(texture_rejections.load() == (boundary != 0 ? 1u : 0u));
                assert(completion.request_id == 55);
                assert(completion.failed || completion.gpu_copy_recorded);
                assert(device.waitIdle() == VK_SUCCESS);
                if (completion.retained_batch_slot != UINT32_MAX)
                {
                    (*pipeline)->releaseAfterGraphicsAcquire(completion.retained_batch_slot);
                }
                // GPU work is complete: dropping an unadopted result must release its native ownership.
                TransferCompletion result = std::move(completion);
                assert(
                    !completion.sampled_image.image && !completion.sampled_image.view &&
                    !completion.sampled_image.sampler
                );
            }
            pipeline->reset();
            texture_failure.store(0);
            const std::lock_guard lock(staging_mutex);
            std::printf(
                "completed texture boundary=%u kind=%u images=%zu views=%zu samplers=%zu\n",
                boundary,
                kind,
                images.size(),
                views.size(),
                samplers.size()
            );
            leaked_texture = leaked_texture || !images.empty() || !views.empty() || !samplers.empty();
            // Keep the failing original run leak-free at fixture shutdown without hiding the result.
            for (const auto& [view, image] : views)
            {
                vkDestroyImageView(device.logicalDevice(), view, nullptr);
            }
            for (const auto& [sampler, owner] : samplers)
            {
                vkDestroySampler(owner, sampler, nullptr);
            }
            for (const auto& [image, allocation] : images)
            {
                vmaDestroyImage(device.vmaAllocator(), image, allocation);
            }
            views.clear();
            samplers.clear();
            images.clear();
        }
    }
    if (leaked_texture)
    {
        return 1;
    }

    // Receive an actual submitted mesh copy, move its staging owner through result storage,
    // then release it only after the original transfer completion boundary.
    {
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = 16;
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        const std::uint32_t families[]{device.graphicsQueueFamilyIndex(), device.transferQueueFamilyIndex()};
        if (families[0] != families[1])
        {
            buffer_info.sharingMode = VK_SHARING_MODE_CONCURRENT;
            buffer_info.queueFamilyIndexCount = 2;
            buffer_info.pQueueFamilyIndices = families;
        }
        VmaAllocationCreateInfo allocation_info{};
        allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
        allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VkBuffer destination{};
        VmaAllocation allocation{};
        VmaAllocationInfo mapped{};
        assert(
            vmaCreateBuffer(
                device.vmaAllocator(),
                &buffer_info,
                &allocation_info,
                &destination,
                &allocation,
                &mapped
            ) == VK_SUCCESS
        );
        assert(mapped.pMappedData);
        auto pipeline = GpuTransferPipeline::create(config);
        assert(pipeline);
        const auto bytes = std::make_shared<std::array<std::byte, 16>>();
        bytes->fill(std::byte{0x4d});
        MeshTransferTask task{};
        task.vbo_buf = destination;
        task.vbo_bytes = bytes->size();
        task.vbo_data = bytes->data();
        task.data_owner = bytes;
        task.request_id = 31;
        assert((*pipeline)->submitMeshTransfer(std::move(task)));
        TransferCompletion completion;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        while ((*pipeline)->drainResults(&completion, 1) == 0)
        {
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        assert(!completion.failed && completion.gpu_copy_recorded && completion.request_id == 31);
        if (completion.timeline_value != 0)
        {
            const auto semaphore = (*pipeline)->timelineSemaphore();
            VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wait.semaphoreCount = 1;
            wait.pSemaphores = &semaphore;
            wait.pValues = &completion.timeline_value;
            assert(vkWaitSemaphores(device.logicalDevice(), &wait, UINT64_MAX) == VK_SUCCESS);
        }
        assert(vmaInvalidateAllocation(device.vmaAllocator(), allocation, 0, 16) == VK_SUCCESS);
        assert(std::memcmp(mapped.pMappedData, bytes->data(), bytes->size()) == 0);
        const auto staging = completion.staging.buffer();
        {
            std::vector<TransferCompletion> pending;
            pending.push_back(std::move(completion));
            assert(!completion.staging.valid());
            assert(pending.front().staging.buffer() == staging);
            TransferCompletion adopted = std::move(pending.front());
            pending.clear();
            {
                const std::lock_guard lock(staging_mutex);
                assert(staging_allocations.size() == 1 && staging_allocations.contains(staging));
            }
            // This is the same final move used by the graphics-followup retirement vector.
            std::vector<StagingBuffer> retirement;
            retirement.push_back(std::move(adopted.staging));
            assert(!adopted.staging.valid());
            retirement.clear();
        }
        {
            const std::lock_guard lock(staging_mutex);
            assert(staging_allocations.empty());
        }
        pipeline->reset();
        vmaDestroyBuffer(device.vmaAllocator(), destination, allocation);
        std::puts("PASS submitted copy bytes, move-only completion and exact staging retirement");
    }

    {
        auto bounded = config;
        bounded.result_capacity = 1;
        bounded.queue_capacity = 16;
        auto pipeline = GpuTransferPipeline::create(bounded);
        assert(pipeline);
        const auto pixels = std::make_shared<std::array<std::byte, 16>>();
        for (unsigned request = 0; request < 8; ++request)
        {
            TextureTransferTask task{};
            task.request_id = request;
            task.format = EPixelFormat::RGBA8_UNORM;
            task.total_bytes = pixels->size();
            task.mips[0] = {pixels, pixels->data(), pixels->size(), 2, 2, 0};
            assert((*pipeline)->submitTextureTransfer(std::move(task)));
        }
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;)
        {
            {
                const std::lock_guard lock(staging_mutex);
                if (images.size() >= 2)
                {
                    break;
                }
            }
            assert(std::chrono::steady_clock::now() < deadline);
            std::this_thread::yield();
        }
        // The original owner drains its full result ring, joins the worker and waits GPU idle.
        // No external fixture performs per-completion native cleanup.
        pipeline->reset();
        const std::lock_guard lock(staging_mutex);
        assert(images.empty() && views.empty() && samplers.empty() && staging_allocations.empty());
        std::puts("PASS full result ring shutdown releases accepted texture owners at the original idle boundary");
    }

    for (unsigned iteration = 0; iteration < 128; ++iteration)
    {
        {
            auto pipeline = GpuTransferPipeline::create(config);
            assert(pipeline && (*pipeline)->timelineSemaphore() != VK_NULL_HANDLE);
            assert(semaphores.size() == 1 && pools.size() == config.batch_slot_count);
            if (iteration == 0)
            {
                (*pipeline)->stopAndDrain();
                (*pipeline)->stopAndDrain();
                assert(!(*pipeline)->submitMeshTransfer(MeshTransferTask{}));
                TransferCompletion completion{};
                assert((*pipeline)->drainResults(&completion, 1) == 0);
                assert(semaphores.size() == 1 && pools.size() == config.batch_slot_count);
            }
        }
        assert(semaphores.empty() && pools.empty());
    }
}
