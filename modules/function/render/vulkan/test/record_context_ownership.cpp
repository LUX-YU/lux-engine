#include <lux/engine/gapi/vk/vk.hpp>

#define VMA_IMPLEMENTATION
#include <vk_mem_alloc.h>

#include <cassert>
#include <map>
#include <type_traits>

namespace
{
    enum class EKind
    {
        VIEW,
        QUERY,
        SEMAPHORE,
        COMMAND_POOL,
        DESCRIPTOR_POOL
    };

    struct NativeRecord
    {
        VkDevice device{};
        const VkAllocationCallbacks* allocator{};
    };

    std::map<std::pair<EKind, std::uintptr_t>, NativeRecord> live;
    unsigned attempts{}, fail_at{};
    bool reject_query{};
    bool instrument_pools{};

    template <auto Create, class Info, class Handle>
    VkResult acquire(EKind kind, VkDevice device, const Info* info, const VkAllocationCallbacks* allocator, Handle* out)
    {
        const bool is_pool = kind == EKind::COMMAND_POOL || kind == EKind::DESCRIPTOR_POOL;
        if (is_pool && !instrument_pools)
        {
            return Create(device, info, allocator, out);
        }
        *out = VK_NULL_HANDLE;
        if (kind == EKind::QUERY)
        {
            if (reject_query)
            {
                return VK_ERROR_OUT_OF_DEVICE_MEMORY;
            }
        }
        else if (++attempts == fail_at)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto result = Create(device, info, allocator, out);
        if (result == VK_SUCCESS)
        {
            assert(
                live.emplace(std::pair{kind, reinterpret_cast<std::uintptr_t>(*out)}, NativeRecord{device, allocator})
                    .second
            );
        }
        return result;
    }

    template <auto Destroy, class Handle>
    void release(EKind kind, VkDevice device, Handle handle, const VkAllocationCallbacks* allocator)
    {
        const bool is_pool = kind == EKind::COMMAND_POOL || kind == EKind::DESCRIPTOR_POOL;
        if (is_pool && !instrument_pools)
        {
            Destroy(device, handle, allocator);
            return;
        }
        const auto found = live.find({kind, reinterpret_cast<std::uintptr_t>(handle)});
        assert(found != live.end());
        assert(found->second.device == device && found->second.allocator == allocator);
        live.erase(found);
        Destroy(device, handle, allocator);
    }

    // Typed wrappers alter only the native failure boundary; successful calls use real Vulkan objects.
#define TRACK_NATIVE(Name, Kind)                                                                                       \
    VkResult create##Name(VkDevice d, const Vk##Name##CreateInfo* i, const VkAllocationCallbacks* a, Vk##Name* o)      \
    {                                                                                                                  \
        return acquire<vkCreate##Name>(EKind::Kind, d, i, a, o);                                                       \
    }                                                                                                                  \
    void destroy##Name(VkDevice d, Vk##Name o, const VkAllocationCallbacks* a)                                         \
    {                                                                                                                  \
        release<vkDestroy##Name>(EKind::Kind, d, o, a);                                                                \
    }

    TRACK_NATIVE(ImageView, VIEW)
    TRACK_NATIVE(QueryPool, QUERY)
    TRACK_NATIVE(Semaphore, SEMAPHORE)
    TRACK_NATIVE(CommandPool, COMMAND_POOL)
    TRACK_NATIVE(DescriptorPool, DESCRIPTOR_POOL)
#undef TRACK_NATIVE

    VkResult allocateCommands(VkDevice device, const VkCommandBufferAllocateInfo* info, VkCommandBuffer* out)
    {
        if (++attempts == fail_at)
        {
            std::fill_n(out, info->commandBufferCount, VK_NULL_HANDLE);
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        return vkAllocateCommandBuffers(device, info, out);
    }

    VkResult allocateDescriptors(VkDevice device, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* out)
    {
        if (++attempts == fail_at)
        {
            std::fill_n(out, info->descriptorSetCount, VK_NULL_HANDLE);
            return VK_ERROR_OUT_OF_POOL_MEMORY;
        }
        return vkAllocateDescriptorSets(device, info, out);
    }
} // namespace

// Compile the actual allocation/rollback code, leaving graph execution and submission unchanged.
// clang-format off
#include "../src/gpu/memory/VmaTypes.cpp"
#define vkCreateImageView createImageView
#define vkDestroyImageView destroyImageView
#define vkCreateQueryPool createQueryPool
#define vkDestroyQueryPool destroyQueryPool
#define vkCreateSemaphore createSemaphore
#define vkDestroySemaphore destroySemaphore
#define vkCreateCommandPool createCommandPool
#define vkDestroyCommandPool destroyCommandPool
#define vkCreateDescriptorPool createDescriptorPool
#define vkDestroyDescriptorPool destroyDescriptorPool
#define vkAllocateCommandBuffers allocateCommands
#define vkAllocateDescriptorSets allocateDescriptors
#include "../src/gpu/VulkanContext.cpp"
#include "../src/graph/RGVulkanRecorder.cpp"
#include "../src/graph/RGVulkanRecorder.RecordContext.cpp"
#undef vkAllocateDescriptorSets
#undef vkAllocateCommandBuffers
#undef vkDestroyDescriptorPool
#undef vkCreateDescriptorPool
#undef vkDestroyCommandPool
#undef vkCreateCommandPool
#undef vkDestroySemaphore
#undef vkCreateSemaphore
#undef vkDestroyQueryPool
#undef vkCreateQueryPool
#undef vkDestroyImageView
#undef vkCreateImageView
// clang-format on

int main()
{
    using namespace lux::render;
    static_assert(!std::is_copy_constructible_v<RGRecordContext>);
    static_assert(!std::is_copy_assignable_v<RGRecordContext>);
    static_assert(std::is_nothrow_move_constructible_v<RGRecordContext>);
    static_assert(std::is_nothrow_move_assignable_v<RGRecordContext>);
    static_assert(std::is_nothrow_destructible_v<RGRecordContext>);

    auto instance_owner = InstanceContext::create({});
    assert(instance_owner);
    auto& instance = **instance_owner;
    auto device_owner = DeviceContext::create(instance, EPhysicalDeviceSelectionPolicy::DISCRETE_GPU_PREFERRED);
    assert(device_owner);
    auto& device = **device_owner;
    auto resources_owner = ResourceContext::create(device);
    assert(resources_owner);
    auto& resources = **resources_owner;
    instrument_pools = true;
    PipelineManager pipelines(device, true);
    RGVulkanRecorder recorder(resources, pipelines);

    const VkDescriptorSetLayoutCreateInfo layout_info{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    auto layout = DescriptorSetLayoutOwner::create(device.logicalDevice(), layout_info);
    assert(layout);
    VkImageCreateInfo image_info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    image_info.imageType = VK_IMAGE_TYPE_2D;
    image_info.format = VK_FORMAT_R8G8B8A8_UNORM;
    image_info.extent = {8, 8, 1};
    image_info.mipLevels = 3;
    image_info.arrayLayers = 1;
    image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.usage = VK_IMAGE_USAGE_SAMPLED_BIT;
    VmaAllocationCreateInfo allocation_info{};
    allocation_info.usage = VMA_MEMORY_USAGE_GPU_ONLY;
    auto image = VmaImage::create(device.vmaAllocator(), image_info, allocation_info);
    assert(image);

    RGPhysicalResourceTable physical;
    const auto index = physical.emplace();
    assert(index == 0);
    physical[index].index = 0;
    physical[index].type = ERGResourceType::TEXTURE;
    physical[index].lifetime = ERGResourceLifetime::PERSISTENT;
    physical[index].physical_handles.push_back(reinterpret_cast<std::uintptr_t>(image->image()));

    RGCompiledGraph graph;
    graph.valid = true;
    auto& resource = graph.original_graph.resources.emplace_back();
    auto texture = RGTextureDescription::Absolute(8, 8);
    texture.mip_levels = 3;
    resource.desc = texture;
    auto& pass = graph.original_graph.passes.emplace_back();
    pass.textures.push_back({RGResourceHandle{0}, ETextureRole::SAMPLED, ERGResourceUsage::READ, {}});
    graph.compiled_passes.emplace_back().pass = &pass;
    graph.multi_queue_info.has_async_work = true;
    graph.multi_queue_info.compute_order = {0};
    graph.multi_queue_info.transfer_order = {0};
    graph.original_graph.transient_descriptor_sets.push_back({"empty", layout->get(), {}});

    unsigned boundary_count{};
    {
        auto first = recorder.allocateRecordContext(graph, physical, {8, 8}, 2);
        assert(first && first->timestamp_pool && first->image_views.size() == 8);
        boundary_count = attempts;
        const auto count = live.size();
        auto second = recorder.allocateRecordContext(graph, physical, {8, 8}, 2);
        assert(second && live.size() == 2 * count);
        *first = std::move(*second);
        assert(live.size() == count && !second->timestamp_pool && second->image_views.empty());
        RGRecordContext moved(std::move(*first));
        assert(!first->timeline_semaphore && moved.per_frame_views[0][0] == moved.image_views[0].get());

        // Imported frame overrides are borrowed: replacing a lookup must neither lose
        // the originally created view owner nor adopt the external view's responsibility.
        VkImageViewCreateInfo imported_info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        imported_info.image = image->image();
        imported_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
        imported_info.format = image_info.format;
        imported_info.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        auto imported = ImageViewOwner::create(device.logicalDevice(), imported_info);
        assert(imported);
        moved.per_frame_views[0][0] = imported->get();
        assert(recorder.deallocateRecordContext(moved));
        assert(live.size() == 1 && moved.frames_in_flight == 0);
        assert(live.contains({EKind::VIEW, reinterpret_cast<std::uintptr_t>(imported->get())}));
    }
    assert(live.empty());
    assert(boundary_count == 15);

    for (unsigned boundary = 1; boundary <= boundary_count; ++boundary)
    {
        attempts = 0;
        fail_at = boundary;
        const auto rejected = recorder.allocateRecordContext(graph, physical, {8, 8}, 2);
        assert(!rejected && isError<err::device::VulkanObjectCreationFailed>(rejected.error()));
        assert(attempts == boundary && live.empty());
    }

    fail_at = 0;
    reject_query = true;
    {
        auto without_timing = recorder.allocateRecordContext(graph, physical, {8, 8}, 2);
        assert(without_timing && !without_timing->timestamp_pool);
        assert(without_timing->timestamp_pass_capacity == 0);
        assert(without_timing->latest_gpu_timing && !without_timing->latest_gpu_timing->available);
        assert(without_timing->image_views.size() == 8);
    }
    assert(live.empty());
    instrument_pools = false;
}
