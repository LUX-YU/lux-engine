#include <lux/engine/render/targets/SwapchainProvider.hpp>
#include <lux/engine/render/gpu/VulkanContext.hpp>
#include <lux/engine/render/gpu/RenderSurface.hpp>
#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/function/render/client/core/RenderTypes.hpp>
#include <lux/engine/render/gpu/utils/FormatMap.hpp>
#include <lux/engine/gapi/vk/Swapchain.hpp>

#include <algorithm>
#include <cassert>
#include <limits>
#include <mutex>
#include <utility>
#include <vector>

namespace lux::render
{
    namespace detail
    {
        RenderError mapSwapchainBuildError(
            const gapi::vk::SwapchainBuildError& error) noexcept
        {
            const std::uint32_t stage =
                gapi::vk::encodeSwapchainBuildStage(error.stage);
            if (error.vk_result)
            {
                const VkResult result = *error.vk_result;
                const std::uint32_t encoded = encodeVkResult(result);
                if (result == VK_ERROR_OUT_OF_DATE_KHR
                    || result == VK_ERROR_SURFACE_LOST_KHR)
                {
                    return renderError<err::device::SwapchainSurfaceChanged>(
                        stage,
                        encoded
                    );
                }
                return renderError<err::device::SwapchainVulkanCallFailed>(
                    stage,
                    encoded
                );
            }

            switch (error.stage)
            {
            case gapi::vk::ESwapchainBuildStage::SURFACE_CAPABILITIES:
            case gapi::vk::ESwapchainBuildStage::SURFACE_FORMATS:
            case gapi::vk::ESwapchainBuildStage::SURFACE_PRESENT_MODES:
            case gapi::vk::ESwapchainBuildStage::ENUMERATE_IMAGES:
                return renderError<err::device::SwapchainUnavailable>(stage);
            case gapi::vk::ESwapchainBuildStage::CONFIGURE:
                return renderError<
                    err::device::SwapchainConfigurationInvalid>(stage);
            case gapi::vk::ESwapchainBuildStage::CREATE:
            case gapi::vk::ESwapchainBuildStage::CREATE_IMAGE_VIEWS:
                return renderError<
                    err::device::SwapchainBuildContractViolated>(stage);
            }
            return renderError<err::device::SwapchainBuildContractViolated>(
                stage
            );
        }

        bool isRetryableSwapchainFailure(const RenderError& error) noexcept
        {
            return isError<err::device::SwapchainUnavailable>(error)
                || isError<err::device::SwapchainSurfaceChanged>(error);
        }

        Expected<SwapchainImageViews> createSwapchainImageViews(
            VkDevice device,
            std::span<const VkImage> images,
            VkFormat format,
            SwapchainImageViewOps ops) noexcept
        {
            const auto stage = gapi::vk::encodeSwapchainBuildStage(
                gapi::vk::ESwapchainBuildStage::CREATE_IMAGE_VIEWS
            );
            if (device == VK_NULL_HANDLE
                || images.empty()
                || format == VK_FORMAT_UNDEFINED
                || ops.create == nullptr
                || ops.destroy == nullptr)
            {
                return renderFailure<
                    err::device::SwapchainConfigurationInvalid>(stage);
            }

            auto candidate = SwapchainImageViews::adopt(
                device,
                ops.destroy
            );
            candidate.reserve(images.size());

            VkImageViewCreateInfo info{
                VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO
            };
            info.viewType = VK_IMAGE_VIEW_TYPE_2D;
            info.format = format;
            info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            info.subresourceRange.baseMipLevel = 0;
            info.subresourceRange.levelCount = 1;
            info.subresourceRange.baseArrayLayer = 0;
            info.subresourceRange.layerCount = 1;

            for (VkImage image : images)
            {
                info.image = image;
                VkImageView view = VK_NULL_HANDLE;
                const VkResult result = ops.create(
                    device,
                    &info,
                    nullptr,
                    &view
                );
                if (result != VK_SUCCESS)
                {
                    return lux::cxx::unexpected<RenderError>(
                        mapSwapchainBuildError({
                            gapi::vk::ESwapchainBuildStage::CREATE_IMAGE_VIEWS,
                            result,
                        })
                    );
                }
                if (view == VK_NULL_HANDLE)
                {
                    return renderFailure<
                        err::device::SwapchainBuildContractViolated>(stage);
                }
                candidate.push(view);
            }

            return candidate;
        }

        Expected<SwapchainAcquireDisposition> classifySwapchainAcquireResult(
            VkResult result,
            bool present_scaling
        ) noexcept
        {
            switch (result)
            {
            case VK_SUCCESS:
                return SwapchainAcquireDisposition{
                    .image_available = true,
                    .mark_rebuild = false,
                };
            case VK_SUBOPTIMAL_KHR:
                return SwapchainAcquireDisposition{
                    .image_available = true,
                    .mark_rebuild = !present_scaling,
                };
            case VK_NOT_READY:
            case VK_TIMEOUT:
                return SwapchainAcquireDisposition{};
            case VK_ERROR_OUT_OF_DATE_KHR:
            case VK_ERROR_SURFACE_LOST_KHR:
                return SwapchainAcquireDisposition{
                    .image_available = false,
                    .mark_rebuild = true,
                };
            default:
                return renderFailure<err::device::VulkanCallFailed>(
                    encodeVkResult(result)
                );
            }
        }

        Expected<SwapchainPresentDisposition> classifySwapchainPresentResult(
            VkResult result,
            bool present_scaling
        ) noexcept
        {
            switch (result)
            {
            case VK_SUCCESS:
                return SwapchainPresentDisposition{};
            case VK_SUBOPTIMAL_KHR:
                return SwapchainPresentDisposition{
                    .mark_rebuild = !present_scaling,
                };
            case VK_ERROR_OUT_OF_DATE_KHR:
            case VK_ERROR_SURFACE_LOST_KHR:
                return SwapchainPresentDisposition{
                    .mark_rebuild = true,
                };
            default:
                return renderFailure<err::device::VulkanCallFailed>(
                    encodeVkResult(result)
                );
            }
        }
    } // namespace detail

    // =============================================================================
    // SwapchainProvider::Impl
    // =============================================================================
    class SwapchainProvider::Impl
    {
    public:
        Impl(ResourceContext &res_ctx, const Config &config)
            : res_ctx_(res_ctx), extent_{config.width, config.height}, vsync_(config.enable_vsync), hdr_(config.enable_hdr)
        {
            // 这是**请求**,不是结果:设备没开 swapchain_maintenance1 就连问都不用
            // 问,直接留在精确尺寸那条路上。真正生效与否由每次 createSwapchain
            // 回填 present_scaling_ —— 它还要看驱动对当前呈现模式支不支持 STRETCH。
            present_scaling_requested_ = config.enable_present_scaling &&
                                         res_ctx_.deviceContext().supportsSwapchainMaintenance1();
        }

        Expected<void> init(RenderSurface &surface, const Config & /*config*/)
        {
            auto &phys = res_ctx_.deviceContext().physicalDevice();
            surface_ = surface;
            swapchain_builder_ = std::make_unique<gapi::vk::SwapchainBuilder>(phys, surface_);
            return createSwapchain(extent_);
        }

        ~Impl()
        {
            cleanupSwapchain();
        }

        // ── Swapchain lifecycle ─────────────────────────────────

        Expected<void> createSwapchain(VkExtent2D extent)
        {
            auto &dev = res_ctx_.deviceContext().logicalDevice();
            auto &phys = res_ctx_.deviceContext().physicalDevice();

            if (extent.width == 0 || extent.height == 0)
            {
                return renderFailure<err::device::SwapchainUnavailable>(
                    gapi::vk::encodeSwapchainBuildStage(
                        gapi::vk::ESwapchainBuildStage::CONFIGURE
                    )
                );
            }

            struct Candidate final
            {
                gapi::vk::Swapchain swapchain;
                std::vector<gapi::vk::Image> images;
                detail::SwapchainImageViews views;
            } candidate;

            auto queue_family = phys.findQueueFamilyIndexByFlags(VK_QUEUE_GRAPHICS_BIT);
            auto built = swapchain_builder_->enableHDR(hdr_)
                             .enableVsync(vsync_)
                             .setInstance(
                                 res_ctx_.deviceContext()
                                     .instanceContext().instance()
                             )
                             .enablePresentScaling(present_scaling_requested_)
                             .setExtent(extent)
                             .setQueueFamilyIndices({queue_family, queue_family})
                             .build(dev.handle());
            if (!built)
            {
                return lux::cxx::unexpected<RenderError>(
                    detail::mapSwapchainBuildError(built.error())
                );
            }
            candidate.swapchain = std::move(*built);

            auto images = candidate.swapchain.images(
                swapchain_builder_->imageEnumerationFn()
            );
            if (!images)
            {
                return lux::cxx::unexpected<RenderError>(
                    detail::mapSwapchainBuildError(images.error())
                );
            }
            candidate.images = std::move(*images);

            std::vector<VkImage> raw_images;
            raw_images.reserve(candidate.images.size());
            for (const auto& image : candidate.images)
                raw_images.push_back(image.handle());

            const VkFormat candidate_format =
                swapchain_builder_->getCreateInfo().imageFormat;
            auto views = detail::createSwapchainImageViews(
                dev.handle(),
                raw_images,
                candidate_format
            );
            if (!views)
                return lux::cxx::unexpected<RenderError>(views.error());
            candidate.views = std::move(*views);

            if (swapchain_.handle() != VK_NULL_HANDLE
                || !swapchain_images_.empty()
                || !swapchain_image_views_.empty())
            {
                return renderFailure<
                    err::device::SwapchainBuildContractViolated>(
                        gapi::vk::encodeSwapchainBuildStage(
                            gapi::vk::ESwapchainBuildStage::CONFIGURE
                        )
                    );
            }

            // 请求缩放 ≠ 拿到缩放:驱动按「表面 × 呈现模式」逐一决定支不支持
            // STRETCH。以请求值当真会让下面的 SUBOPTIMAL 分支永远不重建。
            present_scaling_ = swapchain_builder_->presentScalingActive();

            format_ = candidate_format;
            present_mode_ = swapchain_builder_->getCreateInfo().presentMode;
            extent_ = swapchain_builder_->getCreateInfo().imageExtent;
            swapchain_ = std::move(candidate.swapchain);
            swapchain_images_ = std::move(candidate.images);
            swapchain_image_views_ = std::move(candidate.views);

            swapchain_images_raw_cache_.clear();
            return {};
        }

        void cleanupSwapchain()
        {
            swapchain_image_views_.reset();
            swapchain_images_.clear();
            swapchain_images_raw_cache_.clear();
            swapchain_.reset();
        }

        // ── Acquire / Present ───────────────────────────────────

        Expected<SwapchainProvider::AcquiredImage> acquire(
            VkSemaphore signal_semaphore
        )
        {
            if (need_rebuild_)
                return SwapchainProvider::AcquiredImage{};

            constexpr uint64_t kAcquireTimeoutNs = 50ull * 1000ull * 1000ull; // 50 ms
            uint32_t image_index = std::numeric_limits<uint32_t>::max();
            VkResult result = vkAcquireNextImageKHR(
                res_ctx_.deviceContext().logicalDevice(),
                swapchain_.handle(),
                kAcquireTimeoutNs,
                signal_semaphore,
                VK_NULL_HANDLE,
                &image_index
            );

            auto disposition = detail::classifySwapchainAcquireResult(
                result,
                present_scaling_
            );
            if (!disposition)
                return lux::cxx::unexpected<RenderError>(disposition.error());
            if (disposition->mark_rebuild)
                need_rebuild_ = true;
            if (!disposition->image_available)
                return SwapchainProvider::AcquiredImage{};

            if (image_index >= swapchain_images_.size() || image_index >= swapchain_image_views_.size())
            {
                need_rebuild_ = true;
                return SwapchainProvider::AcquiredImage{};
            }

            AcquiredImage img{};
            img.image = swapchain_images_[image_index];
            img.view = swapchain_image_views_[image_index];
            img.image_index = image_index;
            img.extent = extent_;
            img.format = format_;
            img.valid = true;
            return img;
        }

        VkResult present(uint32_t image_index, VkSemaphore wait_semaphore)
        {
            VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
            pi.waitSemaphoreCount = wait_semaphore ? 1u : 0u;
            pi.pWaitSemaphores = wait_semaphore ? &wait_semaphore : nullptr;
            pi.swapchainCount = 1;
            pi.pSwapchains = swapchain_.handlePtr();
            pi.pImageIndices = &image_index;
            const std::scoped_lock queue_lock(
                res_ctx_.deviceContext().graphicsQueueMutex());
            return vkQueuePresentKHR(
                res_ctx_.deviceContext().graphicsQueue(),
                &pi
            );
        }

        // ── Rebuild ─────────────────────────────────────────────

        Expected<void> rebuild()
        {
            // Caller must have waited all fences before calling.
            need_rebuild_ = false;

            VkExtent2D ext = pending_resize_ ? pending_resize_extent_ : queryExtent();
            pending_resize_ = false;

            if (ext.width == 0 || ext.height == 0)
            {
                return renderFailure<err::device::SwapchainUnavailable>(
                    gapi::vk::encodeSwapchainBuildStage(
                        gapi::vk::ESwapchainBuildStage::CONFIGURE
                    )
                );
            }

            cleanupSwapchain();
            auto result = createSwapchain(ext);
            if (!result)
            {
                need_rebuild_ = true;
                return result;
            }

            if (rebuild_callback_)
            {
                auto rebuilt = rebuild_callback_();
                if (!rebuilt)
                    return lux::cxx::unexpected<RenderError>(rebuilt.error());
            }
            return {};
        }

        bool needsRebuild() const noexcept { return need_rebuild_ || pending_resize_; }
        void markNeedsRebuild() noexcept { need_rebuild_ = true; }

        // ── Queries ─────────────────────────────────────────────

        VkExtent2D extent() const noexcept { return extent_; }
        VkFormat format() const noexcept { return format_; }
        VkPresentModeKHR presentMode() const noexcept { return present_mode_; }
        uint32_t imageCount() const noexcept { return static_cast<uint32_t>(swapchain_images_.size()); }

        RenderTargetBinding makeFrameBinding(uint32_t image_index) const
        {
            const auto &raw = rawImages();
            RenderTargetBinding b;
            if (image_index >= raw.size() || image_index >= swapchain_image_views_.size())
                return b;

            b.layout = nullptr;
            b.extent = extent_;
            b.is_presentable = true;
            auto &sc = b.slot_images[static_cast<size_t>(TargetSlot::SCENE_COLOR)];
            sc.images = {raw[image_index]};
            sc.views = {swapchain_image_views_[image_index]};
            return b;
        }

        RenderTargetLayout layout() const
        {
            RenderTargetLayout l;
            const auto neutral_format = tryMapVkFormat(format_);
            if (!neutral_format)
                return l;
            l.slots[static_cast<size_t>(TargetSlot::SCENE_COLOR)] = RenderTargetSlotDesc{
                .format = *neutral_format,
                .usage = ERenderImageUsage::COLOR_ATTACHMENT,
                .aspect = ERenderAspect::COLOR,
                .final_state = ERenderResourceState::PRESENT,
                .is_presentable = true,
            };
            return l;
        }

        // ── Resize ──────────────────────────────────────────────

        void requestResize(VkExtent2D new_extent)
        {
            if (new_extent.width == 0 || new_extent.height == 0)
                return;
            if (new_extent.width == extent_.width && new_extent.height == extent_.height)
                return;
            pending_resize_ = true;
            pending_resize_extent_ = new_extent;
        }

        void setRebuildCallback(std::function<Expected<void>()> fn)
        {
            rebuild_callback_ = std::move(fn);
        }
        void setExtentProvider(std::function<VkExtent2D()> fn) { extent_provider_ = std::move(fn); }

    private:
        VkExtent2D queryExtent() const
        {
            if (extent_provider_)
                return extent_provider_();
            return extent_;
        }

        const std::vector<VkImage> &rawImages() const
        {
            if (swapchain_images_raw_cache_.empty())
            {
                swapchain_images_raw_cache_.reserve(swapchain_images_.size());
                for (const auto &img : swapchain_images_)
                    swapchain_images_raw_cache_.push_back(img.handle());
            }
            return swapchain_images_raw_cache_;
        }

        // ── Data members ────────────────────────────────────────

        ResourceContext&                res_ctx_;

        // Swapchain state
        VkSurfaceKHR                    surface_{VK_NULL_HANDLE};
        gapi::vk::Swapchain             swapchain_;
        std::unique_ptr<gapi::vk::SwapchainBuilder> swapchain_builder_;
        VkExtent2D                      extent_{};
        VkFormat                        format_{VK_FORMAT_UNDEFINED};
        /// The mode actually granted by the driver, not the one requested — a
        /// torn picture is easy to blame on the renderer, so make it observable.
        VkPresentModeKHR                present_mode_{VK_PRESENT_MODE_FIFO_KHR};

        std::vector<gapi::vk::Image>    swapchain_images_;
        mutable std::vector<VkImage>    swapchain_images_raw_cache_;
        detail::SwapchainImageViews     swapchain_image_views_;

        // Rebuild state
        std::function<Expected<void>()> rebuild_callback_;
        std::function<VkExtent2D()>     extent_provider_;
        bool                            need_rebuild_{false};
        bool                            pending_resize_{false};
        VkExtent2D                      pending_resize_extent_{};

        bool                            vsync_{true};
        bool                            hdr_{false};

    public:
        /// 请求(设备开了 maintenance1 且调用方要了)与实际生效(驱动对当前呈现
        /// 模式确实支持 STRETCH)分开存:混成一个的话,某次因呈现模式不支持而落空,
        /// 就会把请求也一起抹掉,之后再也不重试。
        /// (public 是因为外层 SwapchainProvider 读 present_scaling_ —— 外层类对
        ///  嵌套类的私有成员并无特权。)
        bool present_scaling_requested_{false};
        bool present_scaling_{false};
    };

    // =============================================================================
    // SwapchainProvider — forwarding to Impl
    // =============================================================================

    Expected<SwapchainProvider> SwapchainProvider::create(
        ResourceContext &res_ctx, RenderSurface &surface, const Config &config)
    {
        SwapchainProvider provider;
        provider.impl_ = std::make_unique<Impl>(res_ctx, config);
        auto r = provider.impl_->init(surface, config);
        if (!r)
            return lux::cxx::unexpected(r.error());
        return std::move(provider);
    }

    SwapchainProvider::SwapchainProvider() = default;
    SwapchainProvider::~SwapchainProvider() = default;

    SwapchainProvider::SwapchainProvider(SwapchainProvider &&) noexcept = default;
    SwapchainProvider &SwapchainProvider::operator=(SwapchainProvider &&) noexcept = default;

    Expected<SwapchainProvider::AcquiredImage> SwapchainProvider::acquire(
        VkSemaphore signal_semaphore
    )
    {
        return impl_->acquire(signal_semaphore);
    }

    VkResult SwapchainProvider::present(uint32_t image_index, VkSemaphore wait_semaphore)
    {
        return impl_->present(image_index, wait_semaphore);
    }

    Expected<void> SwapchainProvider::rebuild()
    {
        return impl_->rebuild();
    }

    VkExtent2D SwapchainProvider::extent() const noexcept { return impl_->extent(); }
    VkFormat SwapchainProvider::format() const noexcept { return impl_->format(); }
    VkPresentModeKHR SwapchainProvider::presentMode() const noexcept { return impl_->presentMode(); }
    uint32_t SwapchainProvider::imageCount() const noexcept { return impl_->imageCount(); }

    RenderTargetBinding SwapchainProvider::makeFrameBinding(uint32_t image_index) const
    {
        return impl_->makeFrameBinding(image_index);
    }

    RenderTargetLayout SwapchainProvider::layout() const
    {
        return impl_->layout();
    }

    bool SwapchainProvider::needsRebuild() const noexcept { return impl_->needsRebuild(); }
    void SwapchainProvider::markNeedsRebuild() noexcept { impl_->markNeedsRebuild(); }
    bool SwapchainProvider::presentScalingEnabled() const noexcept { return impl_->present_scaling_; }

    void SwapchainProvider::requestResize(VkExtent2D new_extent) { impl_->requestResize(new_extent); }
    void SwapchainProvider::setRebuildCallback(
        std::function<Expected<void>()> fn
    )
    {
        impl_->setRebuildCallback(std::move(fn));
    }
    void SwapchainProvider::setExtentProvider(std::function<VkExtent2D()> fn) { impl_->setExtentProvider(std::move(fn)); }

} // namespace lux::render
