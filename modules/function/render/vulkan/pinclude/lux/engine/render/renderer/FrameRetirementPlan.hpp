#pragma once

#include <lux/engine/render/graph/RGRecorder.hpp>

#include <algorithm>
#include <span>
#include <vector>

namespace lux::render
{
    struct FrameRetirementPlan final
    {
        bool fence_on_last_submission{false};
        bool wait_compute_idle{false};
        bool wait_transfer_idle{false};
    };

    /// Derive the frame-retirement join from queue identity and signals. The
    /// host array order is intentionally irrelevant: a compute entry appearing
    /// last must never receive a fence that is meant to retire graphics/present.
    [[nodiscard]] inline FrameRetirementPlan analyzeFrameRetirement(
        std::span<const RGQueueSubmission* const> submissions,
        bool presenting,
        std::vector<VkSemaphoreSubmitInfo>& retire_waits
    )
    {
        FrameRetirementPlan result{};
        retire_waits.clear();

        bool has_signal_values = false;
        bool has_non_graphics = false;
        for (const auto* submission : submissions)
        {
            if (submission == nullptr || submission->cmd == VK_NULL_HANDLE)
                continue;
            has_signal_values |= !submission->signal_semaphores.empty();
            has_non_graphics |= submission->queue_type != ERGQueueType::GRAPHICS;
            if (submission->signal_semaphores.empty())
            {
                result.wait_compute_idle |= submission->queue_type == ERGQueueType::COMPUTE;
                result.wait_transfer_idle |= submission->queue_type == ERGQueueType::TRANSFER;
            }

            for (const auto& signal : submission->signal_semaphores)
            {
                const auto found =
                    std::find_if(retire_waits.begin(), retire_waits.end(), [&](const VkSemaphoreSubmitInfo& wait) {
                        return wait.semaphore == signal.semaphore;
                    }
                    );
                if (found == retire_waits.end())
                {
                    retire_waits.push_back(signal);
                }
                else
                {
                    found->value = std::max(found->value, signal.value);
                    found->stageMask |= signal.stageMask;
                }
            }
        }

        result.fence_on_last_submission = !has_signal_values && !presenting && !has_non_graphics;
        return result;
    }
} // namespace lux::render
