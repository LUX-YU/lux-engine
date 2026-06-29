// ============================================================================
//  shader_dedup_test.cpp
//
//  Focused unit test for ShaderResources SPIR-V deduplication + refcounting
//  (the Material/Material-Instance "share one PSO" foundation). Brings up a
//  minimal HEADLESS Vulkan device (no window / surface) and verifies:
//    1. add() of byte-identical SPIR-V returns the SAME ShaderHandle
//       (one shared VkShaderModule) — so downstream shader_key (= handle bits)
//       and the PSO are shared;
//    2. add() of different SPIR-V returns a distinct slot;
//    3. the shared module is REFCOUNTED — releasing one owner keeps it alive;
//       only the last remove() destroys it;
//    4. after full removal the cache entry is evicted, so a later identical
//       add() creates a FRESH module rather than aliasing the freed slot.
//
//  The SPIR-V blobs are minimal valid module headers (magic + version words);
//  vkCreateShaderModule accepts them at module-creation time (no pipeline is
//  built, so no full validation is needed). The test only varies their bytes
//  to exercise the hash / exact-compare / refcount bookkeeping.
//
//  Self-checking: 0 = PASS (or skip if no Vulkan device), 1 = FAIL.
// ============================================================================

#include <lux/engine/render/resources/ShaderResources.hpp>
#include <lux/engine/description/ShaderInfo.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <cstdio>
#include <span>
#include <vector>

using namespace lux::render;

static VkInstance g_instance = VK_NULL_HANDLE;
static VkDevice   g_device   = VK_NULL_HANDLE;

// Minimal headless instance + logical device (one queue, no extensions/layers).
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

// Minimal SPIR-V module headers (magic, version 1.0, generator, bound, schema).
// Byte-distinct so they hash differently and exercise distinct-slot behaviour.
static const uint32_t kBlobA[] = { 0x07230203u, 0x00010000u, 0u, 1u, 0u };
static const uint32_t kBlobB[] = { 0x07230203u, 0x00010000u, 0u, 2u, 0u, 0u };

static std::span<const std::byte> asBytes(const uint32_t* p, std::size_t words)
{
    return std::span<const std::byte>(reinterpret_cast<const std::byte*>(p), words * 4);
}

int main()
{
    std::setbuf(stdout, nullptr);
    std::printf("=== shader_dedup_test ===\n");

    if (!initVulkan())
    {
        std::printf("No Vulkan device. Skipping.\n");
        return 0;
    }

    int fails = 0;
    auto check = [&](bool cond, const char* name)
    { std::printf(cond ? "  [PASS] %s\n" : "  [FAIL] %s\n", name); if (!cond) ++fails; };

    {
        ShaderResources sr;
        ShaderResources::InitInfo ii{};
        ii.device = g_device;
        sr.init(ii);

        const lux::rdesc::ShaderInfo info{};

        const ShaderHandle a1 = sr.add(asBytes(kBlobA, 5), info);
        const ShaderHandle a2 = sr.add(asBytes(kBlobA, 5), info); // byte-identical
        const ShaderHandle b  = sr.add(asBytes(kBlobB, 6), info); // different bytes

        check(!a1.is_null(),                              "blob A compiles to a module");
        check(!b.is_null(),                               "blob B compiles to a module");
        check(a1.index == a2.index && a1.gen == a2.gen,   "identical SPIR-V -> identical handle (dedup)");
        check(b.index != a1.index,                        "different SPIR-V -> distinct slot");
        check(sr.get(a1) != nullptr && sr.get(b) != nullptr, "both handles resolve to live modules");
        check(sr.get(a1)->module == sr.get(a2)->module,   "deduped handles share one VkShaderModule");

        // a1/a2 are one slot with refcount 2; b is its own slot (refcount 1).
        sr.remove(a1);
        check(sr.get(a1) != nullptr,                      "after 1 release, shared module still alive (refcount)");
        sr.remove(a2);
        check(sr.get(a1) == nullptr,                      "after final release, module destroyed");
        check(sr.get(b) != nullptr,                       "unrelated shader unaffected by the release");

        // Cache eviction: re-adding identical A must mint a FRESH module, not
        // alias the now-freed slot.
        const ShaderHandle a3 = sr.add(asBytes(kBlobA, 5), info);
        check(!a3.is_null() && sr.get(a3) != nullptr,     "re-add after eviction creates a fresh live module");
        check(a3.gen != a1.gen || a3.index != a1.index,   "fresh handle distinct from the freed one");

        sr.remove(b);
        sr.remove(a3);
        sr.shutdown();
    }

    vkDestroyDevice(g_device, nullptr);
    vkDestroyInstance(g_instance, nullptr);

    std::printf("=== shader_dedup_test %s (fails=%d) ===\n",
                fails == 0 ? "PASSED" : "FAILED", fails);
    return fails == 0 ? 0 : 1;
}
