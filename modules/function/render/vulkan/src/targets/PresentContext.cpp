#include <lux/engine/render/targets/PresentContext.hpp>
#include <lux/engine/function/render/client/core/RenderFatal.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp> // ResourceContext / DeviceContext / InstanceContext

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
                return renderFailure<err::internal::InvalidArgument>();

            const VkResult result = wait_idle(queue);
            if (result != VK_SUCCESS)
            {
                return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(result));
            }
            return {};
        }

        Expected<void> ensurePresentContextOpen(bool closed) noexcept
        {
            if (closed)
                return renderFailure<err::internal::InvalidArgument>();
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

        PresentSemaphoreCreateCandidate::PresentSemaphoreCreateCandidate(
            PresentSemaphoreCreateCandidate&& other
        ) noexcept
            : device_(std::exchange(other.device_, VkDevice{})), ops_(other.ops_),
              acquire_semaphores_(std::move(other.acquire_semaphores_)),
              present_semaphores_(std::move(other.present_semaphores_))
        {
            other.acquire_semaphores_.clear();
            other.present_semaphores_.clear();
        }

        PresentSemaphoreCreateCandidate&
        PresentSemaphoreCreateCandidate::operator=(PresentSemaphoreCreateCandidate&& other) noexcept
        {
            if (this == &other)
                return *this;

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
                return renderFailure<err::internal::InvalidArgument>();

            PresentSemaphoreCreateCandidate candidate(device, ops);
            candidate.acquire_semaphores_.reserve(acquire_count);
            candidate.present_semaphores_.reserve(present_count);

            const VkSemaphoreCreateInfo create_info{
                .sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
            };

            auto create_batch = [&](std::vector<VkSemaphore>& destination, std::uint32_t count) -> Expected<void> {
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
                            ops.destroy_semaphore(device, semaphore, nullptr);
                        return renderFailure<err::device::VulkanCallFailed>(encodeVkResult(result));
                    }
                    if (semaphore == VK_NULL_HANDLE)
                        return renderFailure<err::device::VulkanObjectCreationFailed>();
                    destination.push_back(semaphore);
                }
                return {};
            };

            auto acquired = create_batch(candidate.acquire_semaphores_, acquire_count);
            if (!acquired)
                return lux::cxx::unexpected<RenderError>(acquired.error());

            auto presented = create_batch(candidate.present_semaphores_, present_count);
            if (!presented)
                return lux::cxx::unexpected<RenderError>(presented.error());

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

    PresentContext::PresentContext(ConstructionKey, ResourceContext& res_ctx, RenderSurface&& surface)
        : res_ctx_(res_ctx), surface_(std::move(surface))
    {
    }

    Expected<std::unique_ptr<PresentContext>> PresentContext::create(
        ResourceContext& res_ctx,
        RenderSurface&& surface,
        VkExtent2D initial_extent,
        bool enable_vsync,
        bool enable_present_scaling
    )
    {
        auto ctx = std::make_unique<PresentContext>(ConstructionKey{}, res_ctx, std::move(surface));

        SwapchainProvider::Config sc_cfg{};
        sc_cfg.width = (initial_extent.width > 0) ? initial_extent.width : 1u;
        sc_cfg.height = (initial_extent.height > 0) ? initial_extent.height : 1u;
        sc_cfg.enable_vsync = enable_vsync;
        sc_cfg.enable_present_scaling = enable_present_scaling;

        // Every early return below just drops `ctx`; the surface owns itself and
        // is released by ~RenderSurface, so failure paths carry no cleanup code.
        auto swapchain = SwapchainProvider::create(res_ctx, ctx->surface_, sc_cfg);
        if (!swapchain)
            return lux::cxx::unexpected<RenderError>(swapchain.error());
        ctx->provider_ = std::make_unique<SwapchainProvider>(std::move(*swapchain));

        auto synchronized = ctx->resyncSemaphores();
        if (!synchronized)
            return lux::cxx::unexpected<RenderError>(synchronized.error());
        ctx->close_required_ = true;
        return ctx;
    }

    PresentContext::~PresentContext()
    {
        if (close_required_ && !closed_)
        {
            renderFatal("PresentContext destroyed without a successful close(); "
                        "presentation completion was not proven"
            );
        }
        // Member order performs the complete teardown: provider_ (swapchain),
        // then owning semaphore vectors, then surface_. No hand-written reset.
    }

    Expected<void> PresentContext::close() noexcept
    {
        if (closed_ || !close_required_)
            return {};

        Expected<void> waited{};
        {
            const std::scoped_lock queue_lock(res_ctx_.deviceContext().graphicsQueueMutex());
            waited = detail::waitPresentQueueIdle(res_ctx_.deviceContext().graphicsQueue(), &vkQueueWaitIdle);
        }
        if (!waited)
            return lux::cxx::unexpected<RenderError>(waited.error());

        closed_ = true;
        return {};
    }

    Expected<void> PresentContext::resyncSemaphores()
    {
        auto& dev = res_ctx_.deviceContext().logicalDevice();
        const uint32_t image_count = provider_ ? provider_->imageCount() : 0u;

        if (image_count == 0u)
            return renderFailure<err::device::SwapchainBuildContractViolated>(
                gapi::vk::encodeSwapchainBuildStage(gapi::vk::ESwapchainBuildStage::ENUMERATE_IMAGES)
            );
        if (image_count == (std::numeric_limits<std::uint32_t>::max)())
            return renderFailure<err::internal::InvalidArgument>();

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
            std::all_of(present_per_image_.begin(), present_per_image_.end(), [](const gapi::vk::Semaphore& semaphore) {
                return semaphore.handle() != VK_NULL_HANDLE;
            }
            );
        if (existing_set_valid)
            return {};

        auto candidate =
            detail::PresentSemaphoreCreateCandidate::create(dev, ring, image_count, kPresentSemaphoreCreateOps);
        if (!candidate)
            return lux::cxx::unexpected<RenderError>(candidate.error());

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
        auto opened = detail::ensurePresentContextOpen(closed_);
        if (!opened)
            return lux::cxx::unexpected<RenderError>(opened.error());
        if (!provider_)
            return renderFailure<err::internal::Unspecified>();

        // Frame fences prove submit completion, not completion of the
        // vkQueuePresentKHR operation that consumed present_sem. Rebuild is a
        // cold path, so wait for the presentation queue before destroying the
        // old swapchain and semaphore set.
        Expected<void> waited{};
        {
            const std::scoped_lock queue_lock(res_ctx_.deviceContext().graphicsQueueMutex());
            waited = detail::waitPresentQueueIdle(res_ctx_.deviceContext().graphicsQueue(), &vkQueueWaitIdle);
        }
        if (!waited)
            return lux::cxx::unexpected<RenderError>(waited.error());

        auto r = provider_->rebuild();
        if (!r)
            return r;
        return resyncSemaphores();
    }

    Expected<PresentContext::Acquired> PresentContext::acquire()
    {
        auto opened = detail::ensurePresentContextOpen(closed_);
        if (!opened)
            return lux::cxx::unexpected<RenderError>(opened.error());

        Acquired out{};
        if (!provider_ || acquire_ring_.empty())
            return out;

        VkSemaphore sem = acquire_ring_[acquire_cursor_];
        const auto acquired = provider_->acquire(sem);
        if (!acquired)
            return lux::cxx::unexpected<RenderError>(acquired.error());
        if (!acquired->valid)
            return out; // sem 未被消费,环不轮转——原位复用,不错位

        if (acquired->image_index >= present_per_image_.size())
            return renderFailure<err::internal::Unspecified>();
        const VkSemaphore present_sem = present_per_image_[acquired->image_index];
        if (sem == VK_NULL_HANDLE || present_sem == VK_NULL_HANDLE)
            return renderFailure<err::internal::Unspecified>();

        acquire_cursor_ = (acquire_cursor_ + 1u) % static_cast<uint32_t>(acquire_ring_.size());

        out.valid = true;
        out.image_index = acquired->image_index;
        out.image = acquired->image;
        out.view = acquired->view;
        out.extent = acquired->extent;
        out.acquire_sem = sem;
        out.present_sem = present_sem;
        return out;
    }

    Expected<void> PresentContext::present(uint32_t image_index, VkSemaphore wait_sem)
    {
        auto opened = detail::ensurePresentContextOpen(closed_);
        if (!opened)
            return lux::cxx::unexpected<RenderError>(opened.error());

        VkResult pr = provider_->present(image_index, wait_sem);
        auto disposition = detail::classifySwapchainPresentResult(pr, provider_->presentScalingEnabled());
        if (!disposition)
            return lux::cxx::unexpected<RenderError>(disposition.error());
        if (disposition->mark_rebuild)
            provider_->markNeedsRebuild();
        return {};
    }

} // namespace lux::render
