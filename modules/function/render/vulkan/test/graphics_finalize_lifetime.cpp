#define VMA_IMPLEMENTATION
#include <lux/engine/render/gpu/VmaFwd.hpp>
#include <vk_mem_alloc.h>

#include <cassert>
#include <cstdio>
#include <set>

namespace
{
    enum class EFault
    {
        NONE,
        ALLOCATE,
        BEGIN,
        END
    };

    EFault fault{};
    VkBuffer watched_staging{};
    bool staging_destroyed{};
    unsigned stale_copies{};
    VkEvent recording_gate{};
    std::set<VkCommandBuffer> live_commands;
    unsigned image_copies{};

    void freeCommands(VkDevice device, VkCommandPool pool, uint32_t count, const VkCommandBuffer* commands)
    {
        for (uint32_t i = 0; i < count; ++i)
        {
            assert(live_commands.erase(commands[i]) == 1);
        }
        vkFreeCommandBuffers(device, pool, count, commands);
    }

    void copyImage(
        VkCommandBuffer command,
        VkBuffer source,
        VkImage destination,
        VkImageLayout layout,
        uint32_t count,
        const VkBufferImageCopy* copies
    )
    {
        ++image_copies;
        const bool is_stale = source == watched_staging && staging_destroyed;
        if (is_stale)
        {
            ++stale_copies;
            return;
        }
        vkCmdCopyBufferToImage(command, source, destination, layout, count, copies);
    }

    VkResult allocateCommands(VkDevice device, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* output)
    {
        if (fault == EFault::ALLOCATE)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vkAllocateCommandBuffers(device, info, output);
        if (result == VK_SUCCESS)
        {
            for (uint32_t i = 0; i < info->commandBufferCount; ++i)
            {
                assert(live_commands.insert(output[i]).second);
            }
        }
        return result;
    }

    VkResult beginCommand(VkCommandBuffer command, const VkCommandBufferBeginInfo* info)
    {
        if (fault == EFault::BEGIN)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = vkBeginCommandBuffer(command, info);
        const bool has_recording_gate = result == VK_SUCCESS && recording_gate;
        if (has_recording_gate)
        {
            vkCmdWaitEvents(
                command,
                1,
                &recording_gate,
                VK_PIPELINE_STAGE_HOST_BIT,
                VK_PIPELINE_STAGE_TRANSFER_BIT,
                0,
                nullptr,
                0,
                nullptr,
                0,
                nullptr
            );
        }
        return result;
    }

    VkResult endCommand(VkCommandBuffer command)
    {
        return fault == EFault::END ? VK_ERROR_OUT_OF_DEVICE_MEMORY : vkEndCommandBuffer(command);
    }

    void copyBuffer(
        VkCommandBuffer command,
        VkBuffer source,
        VkBuffer destination,
        uint32_t count,
        const VkBufferCopy* copies
    )
    {
        const bool is_stale = source == watched_staging && staging_destroyed;
        if (is_stale)
        {
            ++stale_copies;
            return; // Observe the actual provider's stale borrow without invoking native undefined behavior.
        }
        vkCmdCopyBuffer(command, source, destination, count, copies);
    }

    void destroyBuffer(VmaAllocator allocator, VkBuffer buffer, VmaAllocation allocation)
    {
        if (buffer == watched_staging)
        {
            staging_destroyed = true;
        }
        vmaDestroyBuffer(allocator, buffer, allocation);
    }
} // namespace

// Complete production providers, real device/allocator/queues. Only the selected
// native failure and stale-copy observation are interposed.
#define vkAllocateCommandBuffers allocateCommands
#define vkBeginCommandBuffer beginCommand
#define vkEndCommandBuffer endCommand
#define vkCmdCopyBuffer copyBuffer
#define vkCmdCopyBufferToImage copyImage
#define vkFreeCommandBuffers freeCommands
#define vmaDestroyBuffer destroyBuffer
#include "../src/comm/RenderServer.cpp"
#include "../src/comm/RenderServerBootstrap.cpp"
#include "../src/gpu/memory/StagingBuffer.cpp"
#include "../src/resources/descriptor/BindlessCombinedSet.cpp"
#include "../src/resources/mesh/MeshResources.cpp"
#undef vmaDestroyBuffer
#undef vkFreeCommandBuffers
#undef vkCmdCopyBufferToImage
#undef vkCmdCopyBuffer
#undef vkEndCommandBuffer
#undef vkBeginCommandBuffer
#undef vkAllocateCommandBuffers

int main()
{
    using namespace lux::render;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    RenderChannelSync sync;
    ServerConfig config;
    config.enable_validation = true;
    std::atomic<int> validation_errors{};
    config.validation_error_counter = &validation_errors;
    auto prepared = GeneralRenderServer::Impl::create(config, sync);
    assert(prepared);
    auto& im = **prepared;
    assert(ensureGlobalMeshResources(*im.render_ctx_));
    auto& mesh = im.render_ctx_->globalRegistry().must<MeshResources>();
    const auto device = im.dev_ctx_->logicalDevice().handle();
    const auto pool = im.res_ctx_->commandPool();
    const auto allocator = im.dev_ctx_->vmaAllocator();

    for (auto failure : {EFault::ALLOCATE, EFault::BEGIN, EFault::END, EFault::NONE})
    {
        const std::vector<std::byte> bytes(64);
        MeshCreateInfo mesh_info{};
        mesh_info.layout_id = 1;
        mesh_info.vertex_stride = 16;
        mesh_info.vertex_buffer = bytes;
        auto allocated = mesh.allocateOnly(mesh_info);
        assert(allocated);
        const auto* record = mesh.getGpuRecord(allocated->handle);
        assert(record);
        VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        buffer_info.size = bytes.size();
        buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
        VmaAllocationCreateInfo allocation_info{};
        allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
        allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
        VkBuffer buffer{};
        VmaAllocation allocation{};
        assert(vmaCreateBuffer(allocator, &buffer_info, &allocation_info, &buffer, &allocation, nullptr) == VK_SUCCESS);
        watched_staging = buffer;
        staging_destroyed = false;
        const auto before = stale_copies;
        TransferCompletion completion{};
        completion.kind = TransferCompletion::EKind::MESH_BUFFER;
        completion.request_id = 71;
        completion.resource_handle = allocated->handle.index;
        completion.resource_gen = allocated->handle.gen;
        completion.staging = StagingBuffer(allocator, buffer, allocation);
        completion.mesh.mesh_index = allocated->handle.index;
        completion.mesh.vbo_buf = mesh.vertexBuffer(record->vbo_segment);
        completion.mesh.vbo_offset = record->vertex_buffer_range.offset;
        completion.mesh.vbo_size = bytes.size();
        im.pending_completions_.push_back(std::move(completion));
        assert(!completion.staging.valid());
        fault = failure;
        VkEvent gate{};
        if (failure == EFault::NONE)
        {
            VkEventCreateInfo event_info{VK_STRUCTURE_TYPE_EVENT_CREATE_INFO};
            assert(vkCreateEvent(device, &event_info, nullptr, &gate) == VK_SUCCESS);
            recording_gate = gate;
        }
        const auto commands_before = live_commands.size();
        assert(im.processUploadCompletions());
        recording_gate = VK_NULL_HANDLE;
        fault = EFault::NONE;
        if (failure == EFault::NONE)
        {
            assert(!staging_destroyed && live_commands.size() == commands_before + 1);
            assert(im.pending_graphics_finalizes_.size() == 1 && im.pending_deferred_replies_.empty());
            const auto timeline_value = im.pending_graphics_finalizes_[0].timeline_value;
            (void)im.processUploadCompletions();
            assert(!staging_destroyed && im.pending_graphics_finalizes_.size() == 1);
            assert(vkSetEvent(device, gate) == VK_SUCCESS);
            const auto semaphore = im.transfer_pipeline_->timelineSemaphore();
            VkSemaphoreWaitInfo wait{VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO};
            wait.semaphoreCount = 1;
            wait.pSemaphores = &semaphore;
            wait.pValues = &timeline_value;
            assert(vkWaitSemaphores(device, &wait, 5'000'000'000ull) == VK_SUCCESS);
            (void)im.processUploadCompletions();
            assert(staging_destroyed && live_commands.size() == commands_before);
            assert(mesh.alive(allocated->handle) && mesh.isReady(allocated->handle.index));
            assert(mesh.destroy(allocated->handle));
            vkDestroyEvent(device, gate, nullptr);
        }
        assert(staging_destroyed);
        assert(live_commands.size() == commands_before);
        assert(!mesh.alive(allocated->handle));
        assert(im.pending_graphics_finalizes_.empty());
        assert(im.pending_deferred_replies_.size() == 1);
        assert(
            im.pending_deferred_replies_[0].request_id == 71 &&
            im.pending_deferred_replies_[0].status == (failure == EFault::NONE ? 0u : 1u)
        );
        im.pending_deferred_replies_.clear();
        // A subsequent recording must not consume the failed batch's released staging.
        auto retry = CommandBufferOwner::create(device, pool);
        assert(retry);
        VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        assert(vkBeginCommandBuffer(retry->get(), &begin) == VK_SUCCESS);
        mesh.recordStagingCopies(retry->get());
        assert(vkEndCommandBuffer(retry->get()) == VK_SUCCESS);
        std::printf(
            "graphics-finalize failure=%u stale copies=%u (expected 0)\n",
            unsigned(failure),
            stale_copies - before
        );
        watched_staging = VK_NULL_HANDLE;
    }
    auto& textures = im.render_ctx_->globalRegistry().must<TextureResources>();
    for (bool cube : {false, true})
    {
        auto& set = cube ? textures.bindlessSetCube() : textures.bindlessSet2D();
        for (auto failure : {EFault::ALLOCATE, EFault::BEGIN, EFault::END, EFault::NONE})
        {
            // This separately owned synchronous upload must survive rejection of
            // the asynchronous graphics-finalize batch.
            auto& independent_set = textures.bindlessSet2D();
            auto independent = independent_set.addPersistentTexture(1, 1, 1, VK_FORMAT_R8G8B8A8_UNORM);
            assert(independent);
            const auto slot = set.allocateSlotDeferred();
            assert(slot.isValid());
            VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
            image_info.flags = cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0;
            image_info.imageType = VK_IMAGE_TYPE_2D;
            image_info.extent = {1, 1, 1};
            image_info.mipLevels = 1;
            image_info.arrayLayers = cube ? 6 : 1;
            image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
            image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            image_info.samples = VK_SAMPLE_COUNT_1_BIT;
            VmaAllocationCreateInfo allocation_info{};
            allocation_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
            auto image = VmaImage::create(allocator, image_info, allocation_info);
            assert(image);
            VkImageViewCreateInfo view_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view_info.image = image->image();
            view_info.viewType = cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
            view_info.format = image_info.format;
            view_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, image_info.arrayLayers};
            auto view = ImageViewOwner::create(device, view_info);
            assert(view);
            VkSamplerCreateInfo sampler_info{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            auto sampler = SamplerOwner::create(device, sampler_info);
            assert(sampler);
            TransferCompletion completion{};
            completion.kind = cube ? TransferCompletion::EKind::TEXTURE_CUBE : TransferCompletion::EKind::TEXTURE_2D;
            completion.request_id = 72;
            completion.resource_handle = completion.texture.slot_index = slot.index;
            completion.resource_gen = slot.gen;
            completion.texture.face_stride = 4;
            completion.texture.uploaded_mip_count = 1;
            completion.texture.uploaded_mips[0].width = completion.texture.uploaded_mips[0].height = 1;
            completion.sampled_image.image = std::move(*image);
            completion.sampled_image.view = std::move(*view);
            completion.sampled_image.sampler = std::move(*sampler);
            completion.sampled_image.format = image_info.format;
            completion.sampled_image.array_layers = image_info.arrayLayers;
            completion.sampled_image.width = completion.sampled_image.height = 1;
            VkBufferCreateInfo buffer_info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
            buffer_info.size = completion.stg_size = 4 * image_info.arrayLayers;
            buffer_info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
            allocation_info.usage = VMA_MEMORY_USAGE_AUTO;
            allocation_info.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT;
            VkBuffer buffer{};
            VmaAllocation allocation{};
            assert(
                vmaCreateBuffer(allocator, &buffer_info, &allocation_info, &buffer, &allocation, nullptr) == VK_SUCCESS
            );
            watched_staging = buffer;
            staging_destroyed = false;
            completion.staging = StagingBuffer(allocator, buffer, allocation);
            im.pending_completions_.push_back(std::move(completion));
            fault = failure;
            const auto commands_before = live_commands.size();
            assert(im.processUploadCompletions());
            fault = EFault::NONE;
            if (failure == EFault::NONE)
            {
                assert(!staging_destroyed && im.pending_graphics_finalizes_.size() == 1);
                assert(vkDeviceWaitIdle(device) == VK_SUCCESS);
                (void)im.processUploadCompletions();
                assert(set.isTextureAlive(slot));
                assert(set.removeTexture(slot));
            }
            assert(staging_destroyed && live_commands.size() == commands_before);
            assert(im.pending_graphics_finalizes_.empty());
            assert(im.pending_deferred_replies_.size() == 1);
            assert(im.pending_deferred_replies_[0].status == (failure == EFault::NONE ? 0u : 1u));
            im.pending_deferred_replies_.clear();
            const auto copied_before = image_copies;
            auto retry = CommandBufferOwner::create(device, pool);
            assert(retry);
            VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
            assert(vkBeginCommandBuffer(retry->get(), &begin) == VK_SUCCESS);
            set.recordStagingTextureCopies(retry->get());
            set.recordAcquireBarriers(retry->get());
            set.recordDeferredMipGens(retry->get());
            assert(vkEndCommandBuffer(retry->get()) == VK_SUCCESS);
            assert(image_copies == copied_before);
            assert(independent_set.flushUploads());
            assert(image_copies == copied_before + 1);
            assert(independent_set.isTextureAlive(*independent));
            assert(independent_set.removeTexture(*independent));
            std::printf("graphics-finalize cube=%u failure=%u cleanup/retry PASS\n", cube, unsigned(failure));
            watched_staging = VK_NULL_HANDLE;
        }
    }
    prepared->reset();
    assert(live_commands.empty());
    assert(validation_errors.load() == 0);
    return stale_copies ? 1 : 0;
}
