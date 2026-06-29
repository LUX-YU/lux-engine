#pragma once
/**
 * @file FrameSubmitMerge.hpp
 * @brief Pure (GPU-free) merge of per-view multi-queue submissions.
 *
 * Each rendered view contributes one RGMultiQueueSubmitInfo per tick. Several
 * views frequently target the same (queue, command buffer); submitting each
 * separately would be O(N^2) and would issue redundant vkQueueSubmit2 calls.
 * SubmissionMerger collapses them into one RGQueueSubmission per
 * (queue_type, cmd), concatenating the wait/signal semaphore lists.
 *
 * Split out of FrameDriver::endFrame on purpose: it performs NO Vulkan calls, so
 * it is unit-testable in isolation (submission_merge_test), and it owns reused
 * scratch storage so the per-frame hot path allocates nothing after warm-up.
 *
 * Defined inline (header-only) so the unit test compiles its own copy without
 * depending on render.dll's exported-symbol set.
 */

#include <lux/engine/render/graph/RGRecorder.hpp> // RGQueueSubmission, RGMultiQueueSubmitInfo, ERGQueueType

#include <vulkan/vulkan.h>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_map>
#include <vector>

namespace lux::render
{
    /// A submission is identified by its (queue, command buffer) pair.
    struct SubmitMergeKey
    {
        ERGQueueType    queue_type{ERGQueueType::GRAPHICS};
        VkCommandBuffer cmd{VK_NULL_HANDLE};

        bool operator==(const SubmitMergeKey& rhs) const noexcept
        {
            return queue_type == rhs.queue_type && cmd == rhs.cmd;
        }
    };

    struct SubmitMergeKeyHasher
    {
        size_t operator()(const SubmitMergeKey& key) const noexcept
        {
            const size_t h_queue = std::hash<uint32_t>{}(static_cast<uint32_t>(key.queue_type));
            const size_t h_cmd   = std::hash<VkCommandBuffer>{}(key.cmd);
            return h_queue ^ (h_cmd + 0x9e3779b9u + (h_queue << 6u) + (h_queue >> 2u));
        }
    };

    /// Collapses per-view multi-queue submissions into one entry per
    /// (queue_type, cmd), concatenating wait/signal semaphores. Pure CPU logic —
    /// no Vulkan calls. Owns reused scratch (one instance per FrameDriver).
    class SubmissionMerger
    {
    public:
        /// Merge @p inputs. Null / non-multi-queue infos and null command buffers
        /// are skipped. Returns a reference to internal storage that stays valid
        /// until the next merge() call on this instance.
        const std::vector<RGQueueSubmission>&
        merge(const std::vector<const RGMultiQueueSubmitInfo*>& inputs)
        {
            merged_.clear();
            index_.clear();

            for (const RGMultiQueueSubmitInfo* info : inputs)
            {
                if (info == nullptr || !info->is_multi_queue)
                    continue;

                for (const auto& sub : info->submissions)
                {
                    if (sub.cmd == VK_NULL_HANDLE)
                        continue;

                    const SubmitMergeKey key{sub.queue_type, sub.cmd};
                    auto [it, inserted] = index_.try_emplace(key, merged_.size());
                    if (inserted)
                    {
                        merged_.push_back(sub);
                    }
                    else
                    {
                        auto& merged = merged_[it->second];
                        merged.wait_semaphores.insert(
                            merged.wait_semaphores.end(),
                            sub.wait_semaphores.begin(),
                            sub.wait_semaphores.end());
                        merged.signal_semaphores.insert(
                            merged.signal_semaphores.end(),
                            sub.signal_semaphores.begin(),
                            sub.signal_semaphores.end());
                    }
                }
            }
            return merged_;
        }

    private:
        std::vector<RGQueueSubmission>                                   merged_;
        std::unordered_map<SubmitMergeKey, size_t, SubmitMergeKeyHasher> index_;
    };

} // namespace lux::render
