#include <lux/engine/render/targets/SwapchainProvider.hpp>
#include <lux/engine/render/core/VulkanContext.hpp>
#include <lux/engine/render/core/RenderSurface.hpp>
#include <lux/engine/render/core/Errors.hpp>
#include <lux/engine/render/core/RenderTypes.hpp>
#include <lux/engine/gapi/vk/Swapchain.hpp>

#include <algorithm>
#include <cassert>
#include <limits>
#include <vector>

namespace lux::render
{

// =============================================================================
// SwapchainProvider::Impl
// =============================================================================

class SwapchainProvider::Impl
{
public:
    Impl(ResourceContext& res_ctx, const Config& config)
        : res_ctx_(res_ctx)
        , extent_{config.width, config.height}
        , vsync_(config.enable_vsync)
        , hdr_(config.enable_hdr)
    {}

    Expected<void> init(RenderSurface& surface, const Config& /*config*/)
    {
        auto& phys = res_ctx_.deviceContext().physicalDevice();
        surface_ = surface;
        swapchain_builder_ = std::make_unique<gapi::vk::SwapchainBuilder>(phys, surface_);
        return createSwapchain(extent_);
    }

    ~Impl()
    {
        auto& dev = res_ctx_.deviceContext().logicalDevice();
        cleanupSwapchain();
    }

    // ── Swapchain lifecycle ─────────────────────────────────

    Expected<void> createSwapchain(VkExtent2D extent)
    {
        auto& dev  = res_ctx_.deviceContext().logicalDevice();
        auto& phys = res_ctx_.deviceContext().physicalDevice();

        const auto& capabilities = phys.surfaceCapabilities(surface_);
        if (capabilities.currentExtent.width == 0 || capabilities.currentExtent.height == 0)
            return lux::cxx::unexpected(make_error_code(ERenderError::SwapchainCreateFailed));

        extent.width  = std::clamp(extent.width,
                                   capabilities.minImageExtent.width,
                                   capabilities.maxImageExtent.width);
        extent.height = std::clamp(extent.height,
                                   capabilities.minImageExtent.height,
                                   capabilities.maxImageExtent.height);

        auto queue_family = phys.findQueueFamilyIndexByFlags(VK_QUEUE_GRAPHICS_BIT);
        swapchain_ = swapchain_builder_->enableHDR(hdr_)
            .enableVsync(vsync_)
            .setExtent(extent)
            .setQueueFamilyIndices({queue_family, queue_family})
            .build(dev);

        format_           = swapchain_builder_->getCreateInfo().imageFormat;
        extent_           = extent;
        swapchain_images_ = swapchain_.images(dev);

        swapchain_image_views_.resize(swapchain_images_.size());
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.viewType                        = VK_IMAGE_VIEW_TYPE_2D;
        vi.format                          = format_;
        vi.subresourceRange.aspectMask     = VK_IMAGE_ASPECT_COLOR_BIT;
        vi.subresourceRange.baseMipLevel   = 0;
        vi.subresourceRange.levelCount     = 1;
        vi.subresourceRange.baseArrayLayer = 0;
        vi.subresourceRange.layerCount     = 1;
        for (size_t i = 0; i < swapchain_images_.size(); ++i)
        {
            vi.image = swapchain_images_[i].handle();
            VkResult r = vkCreateImageView(dev, &vi, nullptr, &swapchain_image_views_[i]);
            if (r != VK_SUCCESS)
                return lux::cxx::unexpected(make_error_code(ERenderError::VulkanObjectCreationFailed));
        }

        swapchain_images_raw_cache_.clear();
        return {};
    }

    void cleanupSwapchain()
    {
        auto& dev = res_ctx_.deviceContext().logicalDevice();
        for (VkImageView v : swapchain_image_views_)
            if (v != VK_NULL_HANDLE)
                vkDestroyImageView(dev, v, nullptr);
        swapchain_image_views_.clear();
        swapchain_images_.clear();
        swapchain_images_raw_cache_.clear();
        swapchain_.release(dev);
    }

    // ── Acquire / Present ───────────────────────────────────

    SwapchainProvider::AcquiredImage acquire(VkSemaphore signal_semaphore)
    {
        if (need_rebuild_)
            return {};  // invalid

        constexpr uint64_t kAcquireTimeoutNs = 50ull * 1000ull * 1000ull; // 50 ms
        uint32_t image_index = std::numeric_limits<uint32_t>::max();
        VkResult result = vkAcquireNextImageKHR(
            res_ctx_.deviceContext().logicalDevice(),
            swapchain_, kAcquireTimeoutNs, signal_semaphore, VK_NULL_HANDLE, &image_index);

        if (result == VK_ERROR_OUT_OF_DATE_KHR)
        {
            need_rebuild_ = true;
            return {};  // invalid
        }

        if (result == VK_SUBOPTIMAL_KHR)
            need_rebuild_ = true;
        else if (result == VK_TIMEOUT || result == VK_NOT_READY)
            return {};
        else if (result != VK_SUCCESS)
        {
            need_rebuild_ = true;
            return {};
        }

        if (image_index >= swapchain_images_.size()
            || image_index >= swapchain_image_views_.size())
        {
            need_rebuild_ = true;
            return {};
        }

        AcquiredImage img{};
        img.image       = swapchain_images_[image_index];
        img.view        = swapchain_image_views_[image_index];
        img.image_index = image_index;
        img.extent      = extent_;
        img.format      = format_;
        img.valid       = true;
        return img;
    }

    VkResult present(uint32_t image_index, VkSemaphore wait_semaphore)
    {
        VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
        pi.waitSemaphoreCount = wait_semaphore ? 1u : 0u;
        pi.pWaitSemaphores    = wait_semaphore ? &wait_semaphore : nullptr;
        pi.swapchainCount     = 1;
        pi.pSwapchains        = &swapchain_;
        pi.pImageIndices      = &image_index;
        return vkQueuePresentKHR(res_ctx_.deviceContext().graphicsQueue(), &pi);
    }

    // ── Rebuild ─────────────────────────────────────────────

    Expected<void> rebuild()
    {
        // Caller must have waited all fences before calling.
        need_rebuild_ = false;

        VkExtent2D ext = pending_resize_ ? pending_resize_extent_ : queryExtent();
        pending_resize_ = false;

        if (ext.width == 0 || ext.height == 0)
            return lux::cxx::unexpected(make_error_code(ERenderError::SwapchainCreateFailed));

        cleanupSwapchain();
        auto result = createSwapchain(ext);
        if (!result)
        {
            need_rebuild_ = true;
            return result;
        }

        if (rebuild_callback_) rebuild_callback_();
        return {};
    }

    bool needsRebuild() const noexcept { return need_rebuild_ || pending_resize_; }
    void markNeedsRebuild() noexcept { need_rebuild_ = true; }

    // ── Queries ─────────────────────────────────────────────

    VkExtent2D extent() const noexcept { return extent_; }
    VkFormat   format() const noexcept { return format_; }
    uint32_t imageCount() const noexcept { return static_cast<uint32_t>(swapchain_images_.size()); }

    RenderTargetBinding makeFrameBinding(uint32_t image_index) const
    {
        const auto& raw = rawImages();
        RenderTargetBinding b;
        if (image_index >= raw.size() || image_index >= swapchain_image_views_.size())
            return b;

        b.layout         = nullptr;
        b.extent         = extent_;
        b.is_presentable = true;
        auto& sc = b.slot_images[static_cast<size_t>(TargetSlot::SceneColor)];
        sc.images = { raw[image_index] };
        sc.views  = { swapchain_image_views_[image_index] };
        return b;
    }

    RenderTargetLayout layout() const
    {
        RenderTargetLayout l;
        l.slots[static_cast<size_t>(TargetSlot::SceneColor)] = RenderTargetSlotDesc{
            .format       = format_,
            .final_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
            .is_presentable = true,
        };
        return l;
    }

    // ── Resize ──────────────────────────────────────────────

    void requestResize(VkExtent2D new_extent)
    {
        if (new_extent.width == 0 || new_extent.height == 0) return;
        if (new_extent.width == extent_.width && new_extent.height == extent_.height) return;
        pending_resize_        = true;
        pending_resize_extent_ = new_extent;
    }

    void setRebuildCallback(std::function<void()> fn) { rebuild_callback_ = std::move(fn); }
    void setExtentProvider(std::function<VkExtent2D()> fn) { extent_provider_ = std::move(fn); }

private:
    VkExtent2D queryExtent() const
    {
        if (extent_provider_) return extent_provider_();
        return extent_;
    }

    const std::vector<VkImage>& rawImages() const
    {
        if (swapchain_images_raw_cache_.empty())
        {
            swapchain_images_raw_cache_.reserve(swapchain_images_.size());
            for (const auto& img : swapchain_images_)
                swapchain_images_raw_cache_.push_back(img.handle());
        }
        return swapchain_images_raw_cache_;
    }

    // ── Data members ────────────────────────────────────────

    ResourceContext& res_ctx_;

    // Swapchain state
    VkSurfaceKHR                            surface_{VK_NULL_HANDLE};
    gapi::vk::Swapchain                     swapchain_;
    std::unique_ptr<gapi::vk::SwapchainBuilder> swapchain_builder_;
    VkExtent2D                              extent_{};
    VkFormat                                format_{VK_FORMAT_UNDEFINED};

    std::vector<gapi::vk::Image>            swapchain_images_;
    mutable std::vector<VkImage>            swapchain_images_raw_cache_;
    std::vector<VkImageView>                swapchain_image_views_;

    // Rebuild state
    std::function<void()>                   rebuild_callback_;
    std::function<VkExtent2D()>             extent_provider_;
    bool                                    need_rebuild_{false};
    bool                                    pending_resize_{false};
    VkExtent2D                              pending_resize_extent_{};

    bool                                    vsync_{true};
    bool                                    hdr_{false};
};

// =============================================================================
// SwapchainProvider — forwarding to Impl
// =============================================================================

Expected<SwapchainProvider> SwapchainProvider::create(
    ResourceContext& res_ctx, RenderSurface& surface, const Config& config)
{
    SwapchainProvider provider;
    provider.impl_ = std::make_unique<Impl>(res_ctx, config);
    auto r = provider.impl_->init(surface, config);
    if (!r) return lux::cxx::unexpected(r.error());
    return std::move(provider);
}

SwapchainProvider::SwapchainProvider() = default;
SwapchainProvider::~SwapchainProvider() = default;

SwapchainProvider::SwapchainProvider(SwapchainProvider&&) noexcept = default;
SwapchainProvider& SwapchainProvider::operator=(SwapchainProvider&&) noexcept = default;

SwapchainProvider::AcquiredImage SwapchainProvider::acquire(VkSemaphore signal_semaphore)
{ return impl_->acquire(signal_semaphore); }

VkResult SwapchainProvider::present(uint32_t image_index, VkSemaphore wait_semaphore)
{ return impl_->present(image_index, wait_semaphore); }

Expected<void> SwapchainProvider::rebuild()
{ return impl_->rebuild(); }

VkExtent2D SwapchainProvider::extent() const noexcept { return impl_->extent(); }
VkFormat   SwapchainProvider::format() const noexcept  { return impl_->format(); }
uint32_t SwapchainProvider::imageCount() const noexcept { return impl_->imageCount(); }

RenderTargetBinding SwapchainProvider::makeFrameBinding(uint32_t image_index) const
{ return impl_->makeFrameBinding(image_index); }

RenderTargetLayout SwapchainProvider::layout() const
{ return impl_->layout(); }

bool SwapchainProvider::needsRebuild() const noexcept { return impl_->needsRebuild(); }
void SwapchainProvider::markNeedsRebuild() noexcept { impl_->markNeedsRebuild(); }

void SwapchainProvider::requestResize(VkExtent2D new_extent) { impl_->requestResize(new_extent); }
void SwapchainProvider::setRebuildCallback(std::function<void()> fn) { impl_->setRebuildCallback(std::move(fn)); }
void SwapchainProvider::setExtentProvider(std::function<VkExtent2D()> fn) { impl_->setExtentProvider(std::move(fn)); }

} // namespace lux::render
