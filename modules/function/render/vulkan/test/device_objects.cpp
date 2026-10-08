#include <vulkan/vulkan.h>

#include <cassert>
#include <cstdint>
#include <type_traits>
#include <unordered_map>

namespace
{
    enum class EKind
    {
        SAMPLER,
        LAYOUT,
        PIPELINE_LAYOUT,
        POOL,
        SEMAPHORE,
        COMMAND_POOL
    };

    struct NativeObject
    {
        VkDevice device;
        EKind kind;
        uint32_t capacity{};
        uint32_t used{};
        const VkAllocationCallbacks* allocator{};
    };

    std::unordered_map<std::uintptr_t, NativeObject> live;
    std::uintptr_t next_handle{100};
    unsigned attempts{}, fail_at{}, destroyed{};
    EKind last_destroyed{};
    const auto first_device = reinterpret_cast<VkDevice>(1);
    const auto second_device = reinterpret_cast<VkDevice>(2);

    template <class T> VkResult acquire(VkDevice device, EKind kind, T* output, uint32_t capacity = 0)
    {
        ++attempts;
        assert(device == first_device || device == second_device);
        *output = VK_NULL_HANDLE;
        if (attempts == fail_at)
        {
            return VK_ERROR_OUT_OF_DEVICE_MEMORY;
        }
        const auto handle = ++next_handle;
        live.emplace(handle, NativeObject{device, kind, capacity});
        *output = reinterpret_cast<T>(handle);
        return VK_SUCCESS;
    }

    template <class T> void release(VkDevice device, EKind kind, T handle)
    {
        const auto it = live.find(reinterpret_cast<std::uintptr_t>(handle));
        assert(it != live.end() && it->second.device == device && it->second.kind == kind);
        live.erase(it);
        ++destroyed;
        last_destroyed = kind;
    }

    VkResult createSampler(VkDevice device, const VkSamplerCreateInfo*, const VkAllocationCallbacks*, VkSampler* out)
    {
        return acquire(device, EKind::SAMPLER, out);
    }

    void destroySampler(VkDevice device, VkSampler value, const VkAllocationCallbacks*)
    {
        release(device, EKind::SAMPLER, value);
    }

    VkResult createLayout(
        VkDevice device,
        const VkDescriptorSetLayoutCreateInfo*,
        const VkAllocationCallbacks*,
        VkDescriptorSetLayout* out
    )
    {
        return acquire(device, EKind::LAYOUT, out);
    }

    void destroyLayout(VkDevice device, VkDescriptorSetLayout value, const VkAllocationCallbacks*)
    {
        release(device, EKind::LAYOUT, value);
    }

    VkResult createPipelineLayout(
        VkDevice device,
        const VkPipelineLayoutCreateInfo*,
        const VkAllocationCallbacks*,
        VkPipelineLayout* out
    )
    {
        return acquire(device, EKind::PIPELINE_LAYOUT, out);
    }

    void destroyPipelineLayout(VkDevice device, VkPipelineLayout value, const VkAllocationCallbacks*)
    {
        release(device, EKind::PIPELINE_LAYOUT, value);
    }

    VkResult createPool(
        VkDevice device,
        const VkDescriptorPoolCreateInfo* info,
        const VkAllocationCallbacks*,
        VkDescriptorPool* out
    )
    {
        return acquire(device, EKind::POOL, out, info->maxSets);
    }

    void destroyPool(VkDevice device, VkDescriptorPool value, const VkAllocationCallbacks*)
    {
        release(device, EKind::POOL, value);
    }

    VkResult createSemaphore(
        VkDevice device,
        const VkSemaphoreCreateInfo*,
        const VkAllocationCallbacks* allocator,
        VkSemaphore* out
    )
    {
        const auto result = acquire(device, EKind::SEMAPHORE, out);
        if (result == VK_SUCCESS)
        {
            live.at(reinterpret_cast<std::uintptr_t>(*out)).allocator = allocator;
        }
        return result;
    }

    void destroySemaphore(VkDevice device, VkSemaphore value, const VkAllocationCallbacks* allocator)
    {
        assert(live.at(reinterpret_cast<std::uintptr_t>(value)).allocator == allocator);
        release(device, EKind::SEMAPHORE, value);
    }

    VkResult createCommandPool(
        VkDevice device,
        const VkCommandPoolCreateInfo*,
        const VkAllocationCallbacks*,
        VkCommandPool* out
    )
    {
        return acquire(device, EKind::COMMAND_POOL, out);
    }

    void destroyCommandPool(VkDevice device, VkCommandPool value, const VkAllocationCallbacks*)
    {
        release(device, EKind::COMMAND_POOL, value);
    }

    VkResult allocateSets(VkDevice device, const VkDescriptorSetAllocateInfo* info, VkDescriptorSet* out)
    {
        auto& pool = live.at(reinterpret_cast<std::uintptr_t>(info->descriptorPool));
        assert(pool.device == device && pool.kind == EKind::POOL);
        if (pool.used == pool.capacity)
        {
            return VK_ERROR_OUT_OF_POOL_MEMORY;
        }
        ++pool.used;
        *out = reinterpret_cast<VkDescriptorSet>(++next_handle);
        return VK_SUCCESS;
    }
} // namespace

// The actual cache/retirement algorithms are compiled with deterministic native boundaries.
// clang-format off
#define vkCreateSampler createSampler
#define vkDestroySampler destroySampler
#define vkCreateDescriptorSetLayout createLayout
#define vkDestroyDescriptorSetLayout destroyLayout
#define vkCreatePipelineLayout createPipelineLayout
#define vkDestroyPipelineLayout destroyPipelineLayout
#define vkCreateDescriptorPool createPool
#define vkDestroyDescriptorPool destroyPool
#define vkAllocateDescriptorSets allocateSets
#define vkCreateSemaphore createSemaphore
#define vkDestroySemaphore destroySemaphore
#define vkCreateCommandPool createCommandPool
#define vkDestroyCommandPool destroyCommandPool
#include "../src/gpu/descriptor/DescriptorService.cpp"
#include "../src/gpu/pipeline/PipelineLayoutService.cpp"
#include "../src/gpu/descriptor/SceneDescriptorArena.cpp"
#undef vkDestroyCommandPool
#undef vkCreateCommandPool
#undef vkDestroySemaphore
#undef vkCreateSemaphore
#undef vkAllocateDescriptorSets
#undef vkDestroyDescriptorPool
#undef vkCreateDescriptorPool
#undef vkDestroyPipelineLayout
#undef vkCreatePipelineLayout
#undef vkDestroyDescriptorSetLayout
#undef vkCreateDescriptorSetLayout
#undef vkDestroySampler
#undef vkCreateSampler
// clang-format on

namespace
{
    using namespace lux::render;

    template <class Owner, class Info> void ownerContract(Info info)
    {
        static_assert(!std::is_copy_constructible_v<Owner>);
        static_assert(!std::is_copy_assignable_v<Owner>);
        static_assert(std::is_nothrow_move_constructible_v<Owner>);
        static_assert(std::is_nothrow_move_assignable_v<Owner>);
        static_assert(std::is_nothrow_destructible_v<Owner>);
        fail_at = attempts + 1;
        auto failed = Owner::create(first_device, info);
        assert(!failed && failed.error() == VK_ERROR_OUT_OF_DEVICE_MEMORY && live.empty());
        fail_at = 0;
        auto first = Owner::create(first_device, info);
        auto second = Owner::create(second_device, info);
        assert(first && second && live.size() == 2);
        const auto original = first->get();
        const auto before = destroyed;
        *second = std::move(*first);
        assert(destroyed == before + 1 && live.size() == 1);
        assert(!*first && second->get() == original);
        Owner moved(std::move(*second));
        assert(!*second && moved.get() == original);
        moved = std::move(moved);
        moved.reset();
        moved.reset();
        assert(live.empty() && destroyed == before + 2);
    }
} // namespace

int main()
{
    using namespace lux::render;
    ownerContract<SamplerOwner>(VkSamplerCreateInfo{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO});
    ownerContract<DescriptorSetLayoutOwner>(
        VkDescriptorSetLayoutCreateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO}
    );
    ownerContract<PipelineLayoutOwner>(VkPipelineLayoutCreateInfo{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO});
    ownerContract<DescriptorPoolOwner>(VkDescriptorPoolCreateInfo{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO});
    ownerContract<SemaphoreOwner>(VkSemaphoreCreateInfo{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO});
    ownerContract<CommandPoolOwner>(VkCommandPoolCreateInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO});

    {
        VkAllocationCallbacks first_callbacks{}, second_callbacks{};
        const VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        auto first = SemaphoreOwner::create(first_device, info, &first_callbacks);
        auto second = SemaphoreOwner::create(second_device, info, &second_callbacks);
        assert(first && second && live.size() == 2);
        *second = std::move(*first);
        assert(!*first && live.size() == 1);
        SemaphoreOwner moved(std::move(*second));
        assert(!*second);
        moved.reset();
        assert(live.empty());
    }

    {
        DescriptorService cache(first_device, VK_NULL_HANDLE);
        const SamplerDesc descriptor{};
        fail_at = attempts + 1;
        assert(cache.sampler(descriptor) == VK_NULL_HANDLE && live.empty());
        fail_at = 0;
        const auto sampler = cache.sampler(descriptor);
        assert(sampler != VK_NULL_HANDLE && cache.sampler(descriptor) == sampler && live.size() == 1);
        const auto layout = cache.registerLayout({});
        assert(cache.layout(layout) != VK_NULL_HANDLE && cache.registerLayout({}) == layout && live.size() == 2);
    }
    assert(live.empty());
    assert(last_destroyed == EKind::SAMPLER);
    {
        PipelineLayoutService cache(second_device, 4);
        fail_at = attempts + 1;
        auto rejected = cache.getOrCreate({});
        assert(!rejected && isError<err::device::VulkanObjectCreationFailed>(rejected.error()) && live.empty());
        fail_at = 0;
        auto layout = cache.getOrCreate({});
        auto same = cache.getOrCreate({});
        assert(layout && same && *layout == *same && live.size() == 1);
    }
    assert(live.empty());
    {
        SceneDescriptorArena arena;
        SceneDescriptorArena::PoolSizeTemplate size{};
        size.max_sets = 1;
        arena.init(first_device, size);
        const auto layout = reinterpret_cast<VkDescriptorSetLayout>(3);
        assert(arena.allocate(layout) != VK_NULL_HANDLE);
        assert(arena.allocate(layout) != VK_NULL_HANDLE);
        assert(arena.poolCount() == 2 && live.size() == 2);
        const auto before = destroyed;
        assert(arena.beginGeneration() == 2 && arena.poolCount() == 0 && arena.retiredPoolCount() == 2);
        assert(destroyed == before && live.size() == 2);
        assert(arena.allocate(layout) != VK_NULL_HANDLE && live.size() == 3);
        arena.releaseRetired();
        assert(destroyed == before + 2 && live.size() == 1);
    }
    assert(live.empty());
}
