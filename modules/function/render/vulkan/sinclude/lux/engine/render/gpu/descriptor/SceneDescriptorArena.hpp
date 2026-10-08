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
 * and retries once. Only a successful candidate joins the chain. The arena
 * owns every pool until the original generation or scene safe point; its
 * destruction frees all remaining sets. Resource users borrow those sets.
 */

#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/render/gpu/lifecycle/DeviceObject.hpp>

#include <vulkan/vulkan.h>

#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace lux::render
{

    class LUX_FUNCTION_PUBLIC SceneDescriptorArena final
    {
    public:
        /// Per-pool capacity template. Sized so a fully-featured scene fits in ONE
        /// pool with headroom; growth is the safety net for heavy scenes.
        struct PoolSizeTemplate
        {
            uint32_t max_sets = 64;
            uint32_t storage_buffer = 64;
            uint32_t combined_image_sampler = 16;
            uint32_t uniform_buffer = 16;
            uint32_t sampled_image = 0;
            uint32_t storage_image = 0;
            uint32_t sampler = 0;
        };

        using CreateResult = Expected<std::unique_ptr<SceneDescriptorArena>>;

        /// Establish the mandatory device/template binding. Native pools remain lazy.
        [[nodiscard]] static CreateResult create(VkDevice device, const PoolSizeTemplate& sizes) noexcept;
        ~SceneDescriptorArena() noexcept = default;

        SceneDescriptorArena(const SceneDescriptorArena&) = delete;
        SceneDescriptorArena& operator=(const SceneDescriptorArena&) = delete;
        // Fixed address: scene resources borrow this arena across generations.
        SceneDescriptorArena(SceneDescriptorArena&&) = delete;
        SceneDescriptorArena& operator=(SceneDescriptorArena&&) = delete;

        /// Allocate from the current pool, or prepare one fresh pool after capacity exhaustion.
        /// Publish that pool only after its first allocation succeeds. A failed candidate
        /// is reclaimed immediately, preserving all existing sets and the exact native error.
        [[nodiscard]] Expected<VkDescriptorSet>
        allocate(VkDescriptorSetLayout layout, uint32_t variable_count = 0) noexcept;

        /// One native allocation for a complete fixed-count resource family. No prefix
        /// survives rejection; the successful sets retain this arena's generation lifetime.
        [[nodiscard]] Expected<std::vector<VkDescriptorSet>> allocateBatch(
            std::span<const VkDescriptorSetLayout> layouts
        ) noexcept;

        [[nodiscard]] std::size_t poolCount() const noexcept
        {
            return pools_.size();
        }

        // ── Generations — rebuilding descriptor sets per LayoutPlan ─────────────
        //
        // A layout change means the **entire batch** of sets is invalidated (the
        // shape changed), not a scattered handful — so this uses "retire the whole
        // generation" instead of freeing sets one at a time: beginGeneration()
        // moves the entire current pool chain into a retired list and starts fresh
        // from empty pools; once the GPU no longer references any of a retired
        // pool's sets, releaseRetired() destroys it (the caller is responsible for
        // delaying that call by some number of frames-in-flight, the same pattern
        // used for every other FIF-deferred destroy in the engine).
        //
        // Why not free sets one at a time: under a layout change every set needs
        // rebuilding anyway, so freeing them individually just breaks the same
        // work into pieces and fragments the pool; retiring a whole generation is
        // O(pool count) and produces no fragmentation.

        /// Starts a new generation: the current pool chain is retired in its
        /// entirety, and subsequent allocate() calls draw from a fresh pool.
        /// Returns the number of pools retired this call (0 means the current
        /// generation had not allocated any sets yet).
        std::size_t beginGeneration() noexcept;

        /// Destroys every retired pool (and every set within it). **Must only be
        /// called once the GPU no longer references those sets** — the caller is
        /// responsible for the FIF delay.
        void releaseRetired() noexcept;

        [[nodiscard]] std::size_t retiredPoolCount() const noexcept
        {
            return retired_pools_.size();
        }

        /// The current generation number (incremented on every beginGeneration
        /// call). Resource objects can use it to tell whether the set they're
        /// holding has gone stale and needs to be rebuilt against the new layout.
        [[nodiscard]] uint32_t generation() const noexcept
        {
            return generation_;
        }

    private:
        SceneDescriptorArena(VkDevice device, const PoolSizeTemplate& sizes) noexcept;
        [[nodiscard]] DescriptorPoolOwner::CreateResult createPool() const noexcept;
        [[nodiscard]] Expected<void> allocateInto(
            std::span<const VkDescriptorSetLayout> layouts,
            std::span<VkDescriptorSet> sets,
            std::span<const uint32_t> variable_counts = {}
        ) noexcept;
        [[nodiscard]] VkResult tryAllocate(
            VkDescriptorPool pool,
            std::span<const VkDescriptorSetLayout> layouts,
            std::span<VkDescriptorSet> sets,
            std::span<const uint32_t> variable_counts
        ) const noexcept;

        VkDevice device_;
        PoolSizeTemplate tmpl_;
        std::vector<DescriptorPoolOwner> pools_{};         ///< last element = current pool
        std::vector<DescriptorPoolOwner> retired_pools_{}; ///< earlier generations awaiting FIF-deferred destruction
        uint32_t generation_{0};
    };

} // namespace lux::render
