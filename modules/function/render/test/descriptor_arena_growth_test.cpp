// ============================================================================
//  descriptor_arena_growth_test.cpp
//
//  Focused unit test for SceneDescriptorArena (the per-scene growable
//  descriptor-pool chain). Brings up a minimal HEADLESS Vulkan device (no
//  window / surface) and verifies:
//    1. allocate() grows the pool chain when the current pool is exhausted
//       (allocate past max_sets → poolCount() climbs, every set is non-null);
//    2. destroy() tears the whole chain down (poolCount() → 0).
//
//  (The allocate() throw-on-impossible-template guard exists in the code as a
//  no-infinite-grow safety net, but isn't asserted here: a fresh pool always
//  holds >=1 set for maxSets>=1, and per-type pool limits are not portably
//  enforced for UPDATE_AFTER_BIND pools, so it can't be triggered reliably.)
//
//  The per-scene isolation / multi-scene behaviour is covered by
//  two_scene_shadow_isolation_test; this test pins the arena mechanics.
//
//  Self-checking: 0 = PASS (or skip if no Vulkan device), 1 = FAIL.
// ============================================================================

#include <lux/engine/render/resources/descriptor/SceneDescriptorArena.hpp>

#include <vulkan/vulkan.h>

#include <cstdio>
#include <exception>
#include <vector>

using namespace lux::render;

static VkInstance g_instance = VK_NULL_HANDLE;
static VkDevice   g_device   = VK_NULL_HANDLE;

// Minimal headless instance + logical device (one queue, no extensions).
static bool initVulkan()
{
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.apiVersion = VK_API_VERSION_1_2;

    VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    ici.pApplicationInfo = &app;
    if (vkCreateInstance(&ici, nullptr, &g_instance) != VK_SUCCESS)
        return false;

    uint32_t phys_count = 0;
    vkEnumeratePhysicalDevices(g_instance, &phys_count, nullptr);
    if (phys_count == 0)
        return false;
    std::vector<VkPhysicalDevice> phys(phys_count);
    vkEnumeratePhysicalDevices(g_instance, &phys_count, phys.data());
    VkPhysicalDevice pd = phys[0];

    uint32_t qf_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, nullptr);
    std::vector<VkQueueFamilyProperties> qf(qf_count);
    vkGetPhysicalDeviceQueueFamilyProperties(pd, &qf_count, qf.data());
    uint32_t qfi = 0;
    for (uint32_t i = 0; i < qf_count; ++i)
        if (qf[i].queueFlags & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) { qfi = i; break; }

    const float pri = 1.f;
    VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    q.queueFamilyIndex = qfi;
    q.queueCount       = 1;
    q.pQueuePriorities = &pri;

    VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    dci.queueCreateInfoCount = 1;
    dci.pQueueCreateInfos    = &q;
    return vkCreateDevice(pd, &dci, nullptr, &g_device) == VK_SUCCESS;
}

// A plain N-storage-buffer descriptor set layout (no UPDATE_AFTER_BIND — a
// non-UAB layout allocates fine from the arena's UAB-capable pools).
static VkDescriptorSetLayout makeStorageLayout(uint32_t count)
{
    std::vector<VkDescriptorSetLayoutBinding> b(count);
    for (uint32_t i = 0; i < count; ++i)
    {
        b[i].binding         = i;
        b[i].descriptorType  = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        b[i].descriptorCount = 1;
        b[i].stageFlags      = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    VkDescriptorSetLayoutCreateInfo ci{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ci.bindingCount = count;
    ci.pBindings    = b.data();
    VkDescriptorSetLayout layout = VK_NULL_HANDLE;
    vkCreateDescriptorSetLayout(g_device, &ci, nullptr, &layout);
    return layout;
}

int main()
{
    std::setbuf(stdout, nullptr);
    std::printf("=== descriptor_arena_growth_test ===\n");

    if (!initVulkan())
    {
        std::printf("No Vulkan device. Skipping.\n");
        return 0;
    }

    int fails = 0;
    auto check = [&](bool cond, const char* name)
    { std::printf(cond ? "  [PASS] %s\n" : "  [FAIL] %s\n", name); if (!cond) ++fails; };

    // ── 1+2. Growth + teardown ──────────────────────────────────────────
    {
        SceneDescriptorArena arena;
        SceneDescriptorArena::PoolSizeTemplate t{};
        t.max_sets               = 2;   // tiny → forces growth
        t.storage_buffer         = 8;   // plenty of descriptors; max_sets is the limit
        t.combined_image_sampler = 0;
        t.uniform_buffer         = 0;
        arena.init(g_device, t);

        VkDescriptorSetLayout layout = makeStorageLayout(1);

        bool all_nonnull = true;
        std::vector<VkDescriptorSet> sets;
        for (int i = 0; i < 5; ++i)
        {
            VkDescriptorSet s = arena.allocate(layout);
            if (s == VK_NULL_HANDLE) all_nonnull = false;
            sets.push_back(s);
        }
        std::printf("  allocated 5 sets; poolCount=%zu\n", arena.poolCount());
        check(all_nonnull, "5 allocations from a max_sets=2 arena all succeed");
        check(arena.poolCount() == 3, "arena grew to 3 pools (ceil(5/2))");

        arena.destroy();
        check(arena.poolCount() == 0, "destroy() tears down the whole chain");

        vkDestroyDescriptorSetLayout(g_device, layout, nullptr);
    }

    vkDestroyDevice(g_device, nullptr);
    vkDestroyInstance(g_instance, nullptr);

    std::printf("=== descriptor_arena_growth_test %s (fails=%d) ===\n",
                fails == 0 ? "PASSED" : "FAILED", fails);
    return fails == 0 ? 0 : 1;
}
