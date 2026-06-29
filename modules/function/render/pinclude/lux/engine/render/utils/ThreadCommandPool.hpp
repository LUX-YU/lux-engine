#pragma once

#include <lux/engine/render/core/VulkanContext.hpp>
#include <vulkan/vulkan.h>

#include <cassert>
#include <cstdint>
#include <vector>

namespace lux::render
{
    /**
     * @brief Per-thread Vulkan command pool manager for multi-threaded
     *        secondary command buffer recording.
     *
     * Vulkan requires that VkCommandPool objects are only used from one thread
     * at a time. This class manages a pool-per-thread and supports allocating
     * secondary command buffers for parallel render pass recording.
     *
     * Usage pattern (per frame):
     *   1. resetAll()          — vkResetCommandPool for every thread pool
     *   2. allocateSecondary() — get a secondary CB for the calling thread
     *   3. record into it      — vkBeginCommandBuffer / draw / vkEndCommandBuffer
     *   4. (at frame end, pools survive until the next resetAll())
     */
    class ThreadCommandPoolManager
    {
    public:
        /**
         * @brief Construct command pools for `thread_count` threads.
         * @param device_ctx  Device context (provides VkDevice + queue family).
         * @param thread_count Maximum number of recording threads.
         * @param queue_family_index Queue family for the pools
         *        (defaults to graphics family from device_ctx).
         */
        ThreadCommandPoolManager(DeviceContext& device_ctx, uint32_t thread_count,
                                  uint32_t queue_family_index = UINT32_MAX)
            : device_(device_ctx.logicalDevice())
            , pools_(thread_count)
        {
            const uint32_t family = (queue_family_index == UINT32_MAX)
                ? device_ctx.graphicsQueueFamilyIndex()
                : queue_family_index;

            for (uint32_t i = 0; i < thread_count; ++i)
            {
                VkCommandPoolCreateInfo ci{ VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
                ci.flags            = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT; // short-lived CBs
                ci.queueFamilyIndex = family;

                VkResult res = vkCreateCommandPool(device_, &ci, nullptr, &pools_[i].pool);
                assert(res == VK_SUCCESS && "ThreadCommandPoolManager: vkCreateCommandPool failed");
                (void)res;
            }
        }

        ~ThreadCommandPoolManager()
        {
            for (auto& tp : pools_)
            {
                if (!tp.allocated.empty())
                {
                    vkFreeCommandBuffers(device_, tp.pool,
                        static_cast<uint32_t>(tp.allocated.size()), tp.allocated.data());
                    tp.allocated.clear();
                }
                if (tp.pool != VK_NULL_HANDLE)
                {
                    vkDestroyCommandPool(device_, tp.pool, nullptr);
                    tp.pool = VK_NULL_HANDLE;
                }
            }
        }

        // Non-copyable
        ThreadCommandPoolManager(const ThreadCommandPoolManager&)            = delete;
        ThreadCommandPoolManager& operator=(const ThreadCommandPoolManager&) = delete;

        /// @brief Reset all command pools. Call once per frame before recording.
        void resetAll()
        {
            for (auto& tp : pools_)
            {
                vkResetCommandPool(device_, tp.pool, 0);
                tp.next_index = 0;
                // Note: we do NOT free the VkCommandBuffers — reset + reuse is faster.
                // next_index tracks how many are in use this frame.
            }
        }

        /**
         * @brief Allocate (or reuse) a secondary command buffer for the given thread.
         * @param thread_index Must be in [0, thread_count).
         * @return A secondary VkCommandBuffer ready to begin recording.
         */
        VkCommandBuffer allocateSecondary(uint32_t thread_index)
        {
            assert(thread_index < pools_.size());
            auto& tp = pools_[thread_index];

            // Reuse an existing CB if available
            if (tp.next_index < tp.allocated.size())
                return tp.allocated[tp.next_index++];

            // Need a new allocation
            VkCommandBufferAllocateInfo ai{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
            ai.commandPool        = tp.pool;
            ai.level              = VK_COMMAND_BUFFER_LEVEL_SECONDARY;
            ai.commandBufferCount = 1;

            VkCommandBuffer cb = VK_NULL_HANDLE;
            VkResult res = vkAllocateCommandBuffers(device_, &ai, &cb);
            assert(res == VK_SUCCESS);
            (void)res;

            tp.allocated.push_back(cb);
            tp.next_index = static_cast<uint32_t>(tp.allocated.size());
            return cb;
        }

        /// @brief Get the number of threads (pools) managed
        uint32_t threadCount() const { return static_cast<uint32_t>(pools_.size()); }

        /// @brief Get the underlying VkCommandPool for a given thread
        VkCommandPool getPool(uint32_t thread_index) const
        {
            assert(thread_index < pools_.size());
            return pools_[thread_index].pool;
        }

    private:
        struct ThreadPool
        {
            VkCommandPool                pool{ VK_NULL_HANDLE };
            std::vector<VkCommandBuffer> allocated;   ///< All CBs ever allocated from this pool
            uint32_t                     next_index{ 0 }; ///< Next reuse index (reset each frame)
        };

        VkDevice                  device_{ VK_NULL_HANDLE };
        std::vector<ThreadPool>   pools_;
    };

} // namespace lux::render
