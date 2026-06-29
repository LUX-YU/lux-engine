#pragma once
/**
 * @file SceneDescriptorArena.hpp
 * @brief Per-scene GROWABLE descriptor-pool chain.
 *
 * Each RenderScene owns one SceneDescriptorArena that backs all of its
 * PERSISTENT descriptor sets (light/scene/instance/vertex-pool/shadow/
 * skinning/particle). It owns only POOLS — never layouts. Layouts stay global
 * and shared (DescriptorService / GeneralDescriptorSetLayout) so identical
 * VkDescriptorSetLayout handles are reused across scenes and pipelines stay
 * compatible.
 *
 * allocate() draws from the current pool and, on exhaustion
 * (VK_ERROR_OUT_OF_POOL_MEMORY / VK_ERROR_FRAGMENTED_POOL), spins up a new pool
 * and retries — so multi-scene allocation auto-scales without a hand-tuned
 * global cap. destroy() tears down the whole chain at scene teardown, which
 * also frees every set the scene allocated (the per-scene resources never call
 * vkFreeDescriptorSets themselves).
 */

#include <lux/engine/function/visibility.h>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <vector>

namespace lux::render
{

class LUX_FUNCTION_PUBLIC SceneDescriptorArena
{
public:
    /// Per-pool capacity template. Sized so a fully-featured scene fits in ONE
    /// pool with headroom; growth is the safety net for heavy scenes.
    struct PoolSizeTemplate
    {
        uint32_t max_sets               = 64;
        uint32_t storage_buffer         = 64;
        uint32_t combined_image_sampler = 16;
        uint32_t uniform_buffer         = 16;
        uint32_t sampled_image          = 0;
        uint32_t storage_image          = 0;
        uint32_t sampler                = 0;
    };

    SceneDescriptorArena() = default;
    ~SceneDescriptorArena() { destroy(); }

    SceneDescriptorArena(const SceneDescriptorArena&)            = delete;
    SceneDescriptorArena& operator=(const SceneDescriptorArena&) = delete;
    // Owns raw VkDescriptorPool handles — a default move would double-destroy.
    SceneDescriptorArena(SceneDescriptorArena&&)                 = delete;
    SceneDescriptorArena& operator=(SceneDescriptorArena&&)      = delete;

    /// Store device + template. No pool is created until the first allocate().
    void init(VkDevice device, const PoolSizeTemplate& tmpl) noexcept
    {
        device_ = device;
        tmpl_   = tmpl;
    }

    /// Allocate one descriptor set of `layout` from the current pool, growing
    /// the chain on exhaustion. `variable_count` > 0 sizes the last
    /// VARIABLE_DESCRIPTOR_COUNT binding (bindless arrays). Throws if a fresh
    /// pool still cannot satisfy a single allocation (template too small).
    [[nodiscard]] VkDescriptorSet allocate(VkDescriptorSetLayout layout,
                                           uint32_t variable_count = 0);

    /// Destroy every pool in the chain (frees all sets allocated from them).
    /// Idempotent — safe to call from shutdownFull() and again from the dtor.
    void destroy() noexcept;

    [[nodiscard]] std::size_t poolCount() const noexcept { return pools_.size(); }

private:
    [[nodiscard]] VkDescriptorPool createPool() const;
    [[nodiscard]] VkResult        tryAllocate(VkDescriptorPool pool,
                                              VkDescriptorSetLayout layout,
                                              uint32_t variable_count,
                                              VkDescriptorSet& out) const noexcept;

    VkDevice                      device_{VK_NULL_HANDLE};
    PoolSizeTemplate              tmpl_{};
    std::vector<VkDescriptorPool> pools_{};   ///< last element = current pool
};

} // namespace lux::render
