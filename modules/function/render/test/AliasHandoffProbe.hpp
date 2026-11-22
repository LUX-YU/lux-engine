#pragma once

#include "NativeDevice.hpp"
#include <algorithm>
#include <cstdio>
#include <mutex>

namespace foundation_test
{
    // Correlate the recorded global barrier with a real command buffer and the accepted wait
    // that submits it. The producer must have signaled that exact fence value on its own queue.
    class AliasHandoffProbe final
    {
    public:
        AliasHandoffProbe(DeviceSession& device, const nri::GlobalBarrierDesc& expected, nri::StageBits wait)
            : core_(const_cast<nri::CoreInterface&>(device.nriCore())), original_barrier_(core_.CmdBarrier),
              original_submit_(core_.QueueSubmit), expected_(expected), wait_(wait),
              producer_(device.queue(EQueueRole::COMPUTE).native()), consumer_(device.queue().native())
        {
            active_ = this;
            core_.CmdBarrier = record;
            core_.QueueSubmit = submit;
        }

        ~AliasHandoffProbe()
        {
            core_.CmdBarrier = original_barrier_;
            core_.QueueSubmit = original_submit_;
            active_ = nullptr;
        }

        [[nodiscard]] bool observed() noexcept
        {
            std::lock_guard lock(mutex_);
            return observed_;
        }

    private:
        static void NRI_CALL record(nri::CommandBuffer& command, const nri::BarrierDesc& barriers)
        {
            auto& self = *active_;
            for (const auto& barrier : std::span{barriers.globals, barriers.globalNum})
            {
                if (barrier.before.access == self.expected_.before.access &&
                    barrier.before.stages == self.expected_.before.stages &&
                    barrier.after.access == self.expected_.after.access &&
                    barrier.after.stages == self.expected_.after.stages)
                {
                    std::lock_guard lock(self.mutex_);
                    self.buffers_.push_back(&command);
                }
            }
            self.original_barrier_(command, barriers);
        }

        static nri::Result NRI_CALL submit(nri::Queue& queue, const nri::QueueSubmitDesc& desc)
        {
            auto& self = *active_;
            const auto result = self.original_submit_(queue, desc);
            if (result != nri::Result::SUCCESS) return result;
            std::lock_guard lock(self.mutex_);
            if (&queue == &self.producer_)
            {
                for (const auto& signal : std::span{desc.signalFences, desc.signalFenceNum})
                    if (signal.stages == nri::StageBits::ALL) self.signals_.push_back(signal);
            }
            if (&queue != &self.consumer_) return result;
            const bool has_handoff = std::ranges::any_of(
                std::span{desc.commandBuffers, desc.commandBufferNum},
                [&](auto* command) { return std::ranges::find(self.buffers_, command) != self.buffers_.end(); }
            );
            if (has_handoff)
            {
                for (const auto& wait : std::span{desc.waitFences, desc.waitFenceNum})
                {
                    if (wait.stages != self.wait_) continue;
                    const bool matched = std::ranges::any_of(self.signals_, [&](const auto& signal)
                    {
                        return signal.fence == wait.fence && signal.value == wait.value;
                    });
                    if (matched && !self.observed_)
                    {
                        self.observed_ = true;
                        std::printf("Alias buffer handoff: before=%u after=%u wait=%u signal=%llu\n",
                            static_cast<unsigned>(self.expected_.before.stages),
                            static_cast<unsigned>(self.expected_.after.stages), static_cast<unsigned>(wait.stages),
                            static_cast<unsigned long long>(wait.value)
                        );
                    }
                }
            }
            return result;
        }

        inline static AliasHandoffProbe* active_ = nullptr;
        nri::CoreInterface& core_;
        decltype(core_.CmdBarrier) original_barrier_;
        decltype(core_.QueueSubmit) original_submit_;
        nri::GlobalBarrierDesc expected_;
        nri::StageBits wait_;
        nri::Queue& producer_;
        nri::Queue& consumer_;
        std::mutex mutex_;
        std::vector<nri::CommandBuffer*> buffers_;
        std::vector<nri::FenceSubmitDesc> signals_;
        bool observed_ = false;
    };
} // namespace foundation_test
