#pragma once

#include <Extensions/NRISwapChain.h>
#include <NRI.h>

#include <exec/create.hpp>
#include <lux/engine/render/extensions/device/SubmissionQueue.hpp>
#include <lux/engine/render/execution/TargetOutput.hpp>
#include <memory>
#include <vector>

namespace lux::render::detail
{
    class DeviceSession;

    // Owns presentation objects, never the platform window. All physical destruction is admitted
    // to DeviceSession's cleanup scope; close() additionally returns the window borrow to its caller.
    class SurfaceTarget final
    {
    public:
        static RenderResult<std::unique_ptr<SurfaceTarget>> create(
            DeviceSession&,
            TargetDesc,
            SurfaceDescription,
            std::uint32_t frame_count
        ) noexcept;
        ~SurfaceTarget() noexcept;
        SurfaceTarget(const SurfaceTarget&) = delete;
        SurfaceTarget& operator=(const SurfaceTarget&) = delete;

        [[nodiscard]] TargetOutput output() const noexcept
        {
            return {description_, frames_};
        }

        [[nodiscard]] const SurfaceDescription& surface() const noexcept
        {
            return surface_;
        }

        [[nodiscard]] bool available() const noexcept
        {
            return swap_chain_ != nullptr;
        }

        // Same-size Resize can reuse only a live output without an Acquire/Present failure.
        [[nodiscard]] bool needsRecreation() const noexcept
        {
            const bool incomplete_submission =
                presentation_.acquire_wait_submitted && !presentation_.render_signal_submitted;
            return !swap_chain_ || failure_.type != 0 || incomplete_submission;
        }

        // Called only for an already reusable GPU slot. A previously acquired, unsubmitted image
        // is retained for recovery; it is never acquired again or allowed to lose its semaphore.
        // False is normal output backpressure: no image or the preceding Present is still pending.
        [[nodiscard]] RenderResult<bool> acquire(std::uint32_t frame_slot) noexcept;
        [[nodiscard]] RenderResult<bool> present() noexcept;

        [[nodiscard]] auto close(SubmissionFrontiers frontiers) noexcept
        {
            return exec::create<stdexec::set_value_t(RenderResult<void>)>(
                [this, frontiers](auto& completion) noexcept
                {
                    using Completion = std::remove_reference_t<decltype(completion)>;
                    retire(
                        &frontiers,
                        &completion,
                        [](void* object, RenderResult<void> result) noexcept
                        {
                            auto& completion = *static_cast<Completion*>(object);
                            stdexec::set_value(std::move(completion.receiver), std::move(result));
                        }
                    );
                }
            );
        }

    private:
        SurfaceTarget(DeviceSession&, TargetDesc, SurfaceDescription) noexcept;
        using Completion = void (*)(void*, RenderResult<void>) noexcept;
        void retire(const SubmissionFrontiers* = nullptr, void* completion = nullptr, Completion = nullptr) noexcept;

        DeviceSession& session_;
        TargetDesc description_;
        SurfaceDescription surface_;
        nri::SwapChainInterface swap_chain_operations_{};
        nri::SwapChain* swap_chain_ = nullptr;
        std::vector<nri::Texture*> images_; // Borrowed from the swap chain.
        std::vector<nri::Descriptor*> views_;
        std::vector<nri::Fence*> acquire_semaphores_, release_semaphores_;
        std::vector<TargetFrame> frames_;
        PresentationSync presentation_;
        std::uint32_t acquired_image_ = UINT32_MAX;
        RenderError failure_;
    };
} // namespace lux::render::detail
