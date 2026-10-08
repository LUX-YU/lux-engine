#include <lux/engine/function/render/client/core/RenderFatal.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp> // ResourceContext / DeviceContext / InstanceContext
#include <lux/engine/render/targets/PresentContext.hpp>

#include <algorithm>
#include <limits>
#include <mutex>
#include <utility>

namespace lux::render
{
    namespace detail
    {
        Expected<void> waitPresentQueueIdle(VkQueue queue, PFN_vkQueueWaitIdle wait_idle) noexcept
        {
            if (queue == VK_NULL_HANDLE || wait_idle == nullptr)
            {
                return renderFailure<err::internal::InvalidArgument>();
            }

            const VkResult result = wait_idle(queue);
            if (result != VK_SUCCESS)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(result));
            }
            return {};
        }

        PresentSemaphoreCreateCandidate::PresentSemaphoreCreateCandidate(
            VkDevice device,
            PresentSemaphoreCreateOps ops
        ) noexcept
            : device_(device), ops_(ops)
        {
        }

        PresentSemaphoreCreateCandidate::~PresentSemaphoreCreateCandidate() noexcept
        {
            rollback();
        }

        PresentSemaphoreCreateCandidate::PresentSemaphoreCreateCandidate(PresentSemaphoreCreateCandidate&& other
        ) noexcept
            : device_(std::exchange(other.device_, VkDevice{})), ops_(other.ops_),
              acquire_semaphores_(std::move(other.acquire_semaphores_)),
              present_semaphores_(std::move(other.present_semaphores_))
        {
            other.acquire_semaphores_.clear();
            other.present_semaphores_.clear();
        }

        PresentSemaphoreCreateCandidate& PresentSemaphoreCreateCandidate::operator=(
            PresentSemaphoreCreateCandidate&& other
        ) noexcept
        {
            if (this == &other)
            {
                return *this;
            }

            rollback();
            device_ = std::exchange(other.device_, VkDevice{});
            ops_ = other.ops_;
            acquire_semaphores_ = std::move(other.acquire_semaphores_);
            present_semaphores_ = std::move(other.present_semaphores_);
            other.acquire_semaphores_.clear();
            other.present_semaphores_.clear();
            return *this;
        }

        Expected<PresentSemaphoreCreateCandidate> PresentSemaphoreCreateCandidate::create(
            VkDevice device,
            std::uint32_t acquire_count,
            std::uint32_t present_count,
            PresentSemaphoreCreateOps ops
        )
        {
            if (ops.create_semaphore == nullptr || ops.destroy_semaphore == nullptr)
            {
                return renderFailure<err::internal::InvalidArgument>();
            }

            PresentSemaphoreCreateCandidate candidate(device, ops);
            candidate.acquire_semaphores_.reserve(acquire_count);
            candidate.present_semaphores_.reserve(present_count);

            const VkSemaphoreCreateInfo create_info{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
            };

            auto create_batch = [&](std::vector<VkSemaphore>& destination, std::uint32_t count) -> Expected<void>
            {
                for (std::uint32_t i = 0; i < count; ++i)
                {
                    VkSemaphore semaphore = VK_NULL_HANDLE;
                    const VkResult result = ops.create_semaphore(device, &create_info, nullptr, &semaphore);
                    if (result != VK_SUCCESS)
                    {
                        // Vulkan normally creates no object on failure. Still
                        // compensate a non-conforming/fake producer so the
                        // transaction cannot leak a handle it was handed.
                        if (semaphore != VK_NULL_HANDLE)
                        {
                            ops.destroy_semaphore(device, semaphore, nullptr);
                        }
                        return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(result));
                    }
                    if (semaphore == VK_NULL_HANDLE)
                    {
                        return renderFailure<err::device::VulkanObjectCreationFailed>();
                    }
                    destination.push_back(semaphore);
                }
                return {};
            };

            auto acquired = create_batch(candidate.acquire_semaphores_, acquire_count);
            if (!acquired)
            {
                return lux::cxx::unexpected<RenderError>(acquired.error());
            }

            auto presented = create_batch(candidate.present_semaphores_, present_count);
            if (!presented)
            {
                return lux::cxx::unexpected<RenderError>(presented.error());
            }

            return Expected<PresentSemaphoreCreateCandidate>{std::move(candidate)};
        }

        void PresentSemaphoreCreateCandidate::commit() noexcept
        {
            disarm();
        }

        void PresentSemaphoreCreateCandidate::rollback() noexcept
        {
            for (auto it = present_semaphores_.rbegin(); it != present_semaphores_.rend(); ++it)
            {
                ops_.destroy_semaphore(device_, *it, nullptr);
            }
            for (auto it = acquire_semaphores_.rbegin(); it != acquire_semaphores_.rend(); ++it)
            {
                ops_.destroy_semaphore(device_, *it, nullptr);
            }
            disarm();
        }

        void PresentSemaphoreCreateCandidate::disarm() noexcept
        {
            device_ = VK_NULL_HANDLE;
            acquire_semaphores_.clear();
            present_semaphores_.clear();
        }
    } // namespace detail

    namespace
    {
        const detail::PresentSemaphoreCreateOps kPresentSemaphoreCreateOps{
            &vkCreateSemaphore,
            &vkDestroySemaphore,
        };
    } // namespace

    struct PresentBacking final
    {
        PresentBacking(ResourceContext& resources, RenderSurface&& surface) noexcept
            : res_ctx_(resources), surface_(std::move(surface))
        {
        }

        PresentBacking(const PresentBacking&) = delete;
        PresentBacking& operator=(const PresentBacking&) = delete;
        PresentBacking(PresentBacking&&) = delete;
        PresentBacking& operator=(PresentBacking&&) = delete;

        [[nodiscard]] Expected<void> resyncSemaphores();

        ResourceContext& res_ctx_;
        RenderSurface surface_;
        std::vector<gapi::vk::Semaphore> acquire_ring_;
        uint32_t acquire_cursor_{0};
        std::vector<gapi::vk::Semaphore> present_per_image_;
        // Reverse destruction: swapchain, semaphores, surface.
        std::unique_ptr<SwapchainProvider> provider_;
    };

    PresentRetirement::PresentRetirement() noexcept = default;

    PresentRetirement::~PresentRetirement() noexcept
    {
        if (backing_)
        {
            renderFatal("Present retirement destroyed before the original renderer settled presentation");
        }
    }

    bool PresentRetirement::pending() const noexcept
    {
        return bool(backing_);
    }

    Expected<void> PresentRetirement::settle(EPresentRetirementReason reason) noexcept
    {
        if (!backing_)
        {
            return {};
        }

        if (reason == EPresentRetirementReason::NORMAL)
        {
            auto& device = backing_->res_ctx_.deviceContext();
            const std::scoped_lock queue_lock(device.graphicsQueueMutex());
            const auto waited = detail::waitPresentQueueIdle(device.graphicsQueue(), &vkQueueWaitIdle);
            if (!waited)
            {
                return lux::cxx::unexpected(waited.error());
            }
        }
        backing_.reset();
        return {};
    }

    PresentContext::PresentContext(PresentRetirement& retirement, std::unique_ptr<PresentBacking> backing) noexcept
        : retirement_(retirement), backing_(std::move(backing))
    {
    }

    Expected<std::unique_ptr<PresentContext>> PresentContext::create(
        ResourceContext& res_ctx,
        PresentRetirement& retirement,
        RenderSurface&& surface,
        VkExtent2D initial_extent,
        bool enable_vsync,
        bool enable_present_scaling
    )
    {
        auto backing = std::make_unique<PresentBacking>(res_ctx, std::move(surface));

        SwapchainProvider::Config sc_cfg{};
        sc_cfg.width = (initial_extent.width > 0) ? initial_extent.width : 1u;
        sc_cfg.height = (initial_extent.height > 0) ? initial_extent.height : 1u;
        sc_cfg.enable_vsync = enable_vsync;
        sc_cfg.enable_present_scaling = enable_present_scaling;

        // Unpublished construction has no GPU users and rolls back directly.
        auto swapchain = SwapchainProvider::create(res_ctx, backing->surface_, sc_cfg);
        if (!swapchain)
        {
            return lux::cxx::unexpected<RenderError>(swapchain.error());
        }
        backing->provider_ = std::make_unique<SwapchainProvider>(std::move(*swapchain));

        auto synchronized = backing->resyncSemaphores();
        if (!synchronized)
        {
            return lux::cxx::unexpected<RenderError>(synchronized.error());
        }
        return std::unique_ptr<PresentContext>(new PresentContext(retirement, std::move(backing)));
    }

    PresentContext::~PresentContext()
    {
        if (retirement_.backing_)
        {
            renderFatal("Present retirement already owns a different backing");
        }
        retirement_.backing_ = std::move(backing_);
    }

    SwapchainProvider* PresentContext::provider() noexcept
    {
        return backing_->provider_.get();
    }

    const SwapchainProvider* PresentContext::provider() const noexcept
    {
        return backing_->provider_.get();
    }

    bool PresentContext::needsRebuild() const noexcept
    {
        return backing_->provider_->needsRebuild();
    }

    Expected<void> PresentBacking::resyncSemaphores()
    {
        auto& dev = res_ctx_.deviceContext().logicalDevice();
        const uint32_t image_count = provider_ ? provider_->imageCount() : 0u;

        if (image_count == 0u)
        {
            return renderFailure<err::device::SwapchainBuildContractViolated>(
                gapi::vk::encodeSwapchainBuildStage(gapi::vk::ESwapchainBuildStage::ENUMERATE_IMAGES)
            );
        }
        if (image_count == (std::numeric_limits<std::uint32_t>::max)())
        {
            return renderFailure<err::internal::InvalidArgument>();
        }

        // acquire 环 = imageCount + 1(imgui 副视口验证过的形态):任一时刻
        // 至多 imageCount 个 acquire 信号在飞,+1 保证轮转到的 sem 必已被
        // 上一轮 present 消费。
        const uint32_t ring = image_count + 1u;
        const bool existing_set_valid =
            acquire_ring_.size() == ring && present_per_image_.size() == image_count &&
            std::all_of(
                acquire_ring_.begin(),
                acquire_ring_.end(),
                [](const gapi::vk::Semaphore& semaphore) { return semaphore.handle() != VK_NULL_HANDLE; }
            ) &&
            std::all_of(
                present_per_image_.begin(),
                present_per_image_.end(),
                [](const gapi::vk::Semaphore& semaphore) { return semaphore.handle() != VK_NULL_HANDLE; }
            );
        if (existing_set_valid)
        {
            return {};
        }

        auto candidate =
            detail::PresentSemaphoreCreateCandidate::create(dev, ring, image_count, kPresentSemaphoreCreateOps);
        if (!candidate)
        {
            return lux::cxx::unexpected<RenderError>(candidate.error());
        }

        std::vector<gapi::vk::Semaphore> next_acquire;
        std::vector<gapi::vk::Semaphore> next_present;
        next_acquire.reserve(ring);
        next_present.reserve(image_count);
        for (std::uint32_t i = 0; i < ring; ++i)
        {
            next_acquire.emplace_back(gapi::vk::Semaphore::adopt(dev, candidate->acquireSemaphore(i)));
        }
        for (std::uint32_t i = 0; i < image_count; ++i)
        {
            next_present.emplace_back(gapi::vk::Semaphore::adopt(dev, candidate->presentSemaphore(i)));
        }
        candidate->commit();

        // Swap transfers publication atomically at the container level. The
        // local vectors now own the old set and release it on scope exit.
        acquire_ring_.swap(next_acquire);
        present_per_image_.swap(next_present);
        acquire_cursor_ = 0;
        return {};
    }

    Expected<void> PresentContext::rebuild()
    {

        // Frame fences prove submit completion, not completion of the
        // vkQueuePresentKHR operation that consumed present_sem. Rebuild is a
        // cold path, so wait for the presentation queue before destroying the
        // old swapchain and semaphore set.
        Expected<void> waited{};
        {
            const std::scoped_lock queue_lock(backing_->res_ctx_.deviceContext().graphicsQueueMutex());
            waited = detail::waitPresentQueueIdle(backing_->res_ctx_.deviceContext().graphicsQueue(), &vkQueueWaitIdle);
        }
        if (!waited)
        {
            return lux::cxx::unexpected<RenderError>(waited.error());
        }

        auto r = backing_->provider_->rebuild();
        if (!r)
        {
            return r;
        }
        return backing_->resyncSemaphores();
    }

    Expected<std::optional<PresentContext::Acquired>> PresentContext::acquire()
    {
        if (!backing_->provider_ || backing_->acquire_ring_.empty())
        {
            return std::optional<Acquired>{};
        }

        VkSemaphore sem = backing_->acquire_ring_[backing_->acquire_cursor_];
        const auto acquired = backing_->provider_->acquire(sem);
        if (!acquired)
        {
            return lux::cxx::unexpected<RenderError>(acquired.error());
        }
        if (!*acquired)
        {
            return std::optional<Acquired>{}; // sem 未被消费,环不轮转——原位复用,不错位
        }

        const auto& image = **acquired;
        if (image.image_index >= backing_->present_per_image_.size())
        {
            return renderFailure<err::internal::Unspecified>();
        }
        const VkSemaphore present_sem = backing_->present_per_image_[image.image_index];
        if (sem == VK_NULL_HANDLE || present_sem == VK_NULL_HANDLE)
        {
            return renderFailure<err::internal::Unspecified>();
        }

        backing_->acquire_cursor_ =
            (backing_->acquire_cursor_ + 1u) % static_cast<uint32_t>(backing_->acquire_ring_.size());

        Acquired out{};
        out.image_index = image.image_index;
        out.image = image.image;
        out.view = image.view;
        out.extent = image.extent;
        out.acquire_sem = sem;
        out.present_sem = present_sem;
        return std::optional{out};
    }

    Expected<void> PresentContext::present(uint32_t image_index, VkSemaphore wait_sem)
    {
        VkResult pr = backing_->provider_->present(image_index, wait_sem);
        auto disposition = detail::classifySwapchainPresentResult(pr, backing_->provider_->presentScalingEnabled());
        if (!disposition)
        {
            return lux::cxx::unexpected<RenderError>(disposition.error());
        }
        if (disposition->mark_rebuild)
        {
            backing_->provider_->markNeedsRebuild();
        }
        return {};
    }

} // namespace lux::render
