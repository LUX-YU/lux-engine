#pragma once

#include <Extensions/NRISwapChain.h>
#include <NRI.h>

#include <exec/create.hpp>
#include <exec/task.hpp>
#include <lux/engine/render/extensions/device/SubmissionFrontiers.hpp>
#include <lux/engine/render/target/TargetOutput.hpp>
#include <memory>
#include <vector>

namespace lux::render::detail
{
    class DeviceSession;

    // Owns presentation objects and borrows the platform window until close() settles native retirement.
    class SwapchainTarget final
    {
    public:
        [[nodiscard]] bool belongsTo(const DeviceSession& device) const noexcept { return &session_ == &device; }

        static RenderResult<SwapchainTarget> create(
            DeviceSession&,
            TargetDesc,
            SurfaceDescription,
            std::uint32_t frame_count
        ) noexcept;
        ~SwapchainTarget() noexcept;
        SwapchainTarget(SwapchainTarget&&) noexcept;
        SwapchainTarget(const SwapchainTarget&) = delete;
        SwapchainTarget& operator=(const SwapchainTarget&) = delete;

        [[nodiscard]] TargetOutput output() const noexcept
        {
            return {description_, frames_};
        }

        [[nodiscard]] const TargetDesc& description() const noexcept { return description_; }

        [[nodiscard]] const SurfaceDescription& surface() const noexcept
        {
            return surface_;
        }

        [[nodiscard]] bool available() const noexcept
        {
            return swap_chain_ != nullptr;
        }

        TargetState state;

        [[nodiscard]] bool needsRecreation() const noexcept
        {
            return !available() || failure_.type || presentation_.phase == EPresentationPhase::WAIT_ACCEPTED;
        }

        [[nodiscard]] RenderResult<bool> prefetched() const noexcept;
        [[nodiscard]] RenderResult<bool> bindImage(std::uint32_t frame_slot) noexcept;

        enum class ENativeStep : std::uint8_t { NONE, ACQUIRE, PRESENT };
        [[nodiscard]] ENativeStep beginAdvance(bool acquire) noexcept;
        [[nodiscard]] exec::task<RenderResult<bool>> advance(ENativeStep) noexcept;
        [[nodiscard]] exec::task<RenderResult<void>> close() noexcept;

    private:
        [[nodiscard]] auto whenIdle() noexcept
        {
            return exec::create<stdexec::set_value_t()>([this](auto& completion) noexcept
            {
                if (!nativePending())
                {
                    stdexec::set_value(std::move(completion.receiver));
                    return;
                }
                using Completion = std::remove_reference_t<decltype(completion)>;
                idle_context_ = &completion;
                idle_ = [](void* object) noexcept
                {
                    auto& done = *static_cast<Completion*>(object);
                    stdexec::set_value(std::move(done.receiver));
                };
            });
        }

        SwapchainTarget(DeviceSession&, TargetDesc, SurfaceDescription) noexcept;

        [[nodiscard]] bool nativePending() const noexcept
        {
            return presentation_.phase == EPresentationPhase::ACQUIRING ||
                presentation_.phase == EPresentationPhase::PRESENTING;
        }

        struct AcquireFence final
        {
            nri::Fence* native = nullptr;
            SubmissionPoint last_wait;
        };

        struct Retirement final
        {
            DeviceSession& session;
            nri::SwapChainInterface operations;
            nri::SwapChain* swap_chain;
            nri::Fence* unused_acquire;
            std::vector<nri::Descriptor*> views;
            std::vector<AcquireFence> acquired;
            std::vector<nri::Fence*> released;

            [[nodiscard]] RenderResult<void> release(bool lost) noexcept;
        };

        [[nodiscard]] Retirement takeRetirement() noexcept;

        DeviceSession& session_;
        TargetDesc description_;
        SurfaceDescription surface_;
        nri::SwapChainInterface swap_chain_operations_{};
        nri::SwapChain* swap_chain_ = nullptr;
        std::vector<nri::Texture*> images_; // Borrowed from the swap chain.
        std::vector<nri::Descriptor*> views_;
        std::vector<AcquireFence> acquire_semaphores_;
        std::vector<nri::Fence*> release_semaphores_;
        std::vector<TargetFrame> frames_;
        PresentationSync presentation_;
        std::uint32_t acquire_slot_ = 0;
        std::uint32_t acquired_image_ = UINT32_MAX;
        std::uint64_t next_present_id_ = 1;
        RenderError failure_;
        SubmissionFrontiers uses_;
        void* idle_context_ = nullptr;
        void (*idle_)(void*) noexcept = nullptr;
    };
} // namespace lux::render::detail
