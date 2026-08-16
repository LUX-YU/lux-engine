#pragma once
/**
 * @file PresentContext.hpp
 * @brief Surface Target 的 target 级呈现机件(RenderTarget 一等化,设计 §3)。
 *
 * FrameDriver 拆两层后本类持有 target 级的全部呈现状态:
 *   surface → swapchain → acquire sem 环 + per-image present sems。
 * 帧级(per-FIF fence、主 CB、gpuCompletedSerial 水位)留在 FrameDriver;
 * 每个 Surface RenderTargetEntry 拥有一个 PresentContext——主窗与 imgui
 * 副窗同构,多窗即多实例。
 *
 * acquire sem 环 = imageCount + 1,逐次 acquire 轮转(imgui 副视口
 * SemaphoreCount=N+1 方案已验证的形态):跳帧不消费 acquire 信号时环
 * 位置不错位(风险表 #2)。
 *
 * 生命周期:swapchain → sems → surface 逆序拆。仅等待 frame fence 盖不住
 * vkQueuePresentKHR,所以拥有者销毁前必须显式 close();close 的 Expected 是
 * vkQueueWaitIdle 失败的唯一可报告出口。析构只验证该结构契约,不承担隐式等待。
 *
 * Thread model: render-thread only。
 */

#include <lux/engine/function/visibility.h>
#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/render/gpu/RenderSurface.hpp>
#include <lux/engine/render/targets/SwapchainProvider.hpp>
#include <lux/engine/gapi/vk/Semaphore.hpp>

#include <vulkan/vulkan.h>
#include <cstdint>
#include <memory>
#include <vector>

namespace lux::render
{
    class ResourceContext;

    namespace detail
    {
        [[nodiscard]] LUX_FUNCTION_PUBLIC Expected<void> waitPresentQueueIdle(
            VkQueue queue,
            PFN_vkQueueWaitIdle wait_idle
        ) noexcept;

        [[nodiscard]] LUX_FUNCTION_PUBLIC Expected<void> ensurePresentContextOpen(
            bool closed
        ) noexcept;

        struct PresentSemaphoreCreateOps final
        {
            PFN_vkCreateSemaphore  create_semaphore{nullptr};
            PFN_vkDestroySemaphore destroy_semaphore{nullptr};
        };

        /// Owns a complete replacement semaphore set until PresentContext has
        /// adopted every handle. A failed call rolls back the partially-created
        /// batch in strict reverse creation order.
        class LUX_FUNCTION_PUBLIC PresentSemaphoreCreateCandidate final
        {
        public:
            ~PresentSemaphoreCreateCandidate() noexcept;

            PresentSemaphoreCreateCandidate(
                const PresentSemaphoreCreateCandidate&
            ) = delete;
            PresentSemaphoreCreateCandidate& operator=(
                const PresentSemaphoreCreateCandidate&
            ) = delete;
            PresentSemaphoreCreateCandidate(
                PresentSemaphoreCreateCandidate&& other
            ) noexcept;
            PresentSemaphoreCreateCandidate& operator=(
                PresentSemaphoreCreateCandidate&& other
            ) noexcept;

            [[nodiscard]] static Expected<PresentSemaphoreCreateCandidate> create(
                VkDevice device,
                std::uint32_t acquire_count,
                std::uint32_t present_count,
                PresentSemaphoreCreateOps ops
            );

            [[nodiscard]] std::uint32_t acquireCount() const noexcept
            {
                return static_cast<std::uint32_t>(acquire_semaphores_.size());
            }

            [[nodiscard]] std::uint32_t presentCount() const noexcept
            {
                return static_cast<std::uint32_t>(present_semaphores_.size());
            }

            [[nodiscard]] VkSemaphore acquireSemaphore(
                std::uint32_t index
            ) const noexcept
            {
                return acquire_semaphores_[index];
            }

            [[nodiscard]] VkSemaphore presentSemaphore(
                std::uint32_t index
            ) const noexcept
            {
                return present_semaphores_[index];
            }

            void commit() noexcept;

        private:
            PresentSemaphoreCreateCandidate(
                VkDevice device,
                PresentSemaphoreCreateOps ops
            ) noexcept;

            void rollback() noexcept;
            void disarm() noexcept;

            VkDevice                    device_{VK_NULL_HANDLE};
            PresentSemaphoreCreateOps   ops_{};
            std::vector<VkSemaphore>    acquire_semaphores_;
            std::vector<VkSemaphore>    present_semaphores_;
        };
    } // namespace detail

    class LUX_FUNCTION_PUBLIC PresentContext
    {
        struct ConstructionKey final {};

    public:
        /// surface 所有权移交进来;内部建 swapchain + 两套信号量。
        /// 失败时接管并销毁传入的 surface(调用方无需善后)。
        [[nodiscard]] static Expected<std::unique_ptr<PresentContext>> create(
            ResourceContext& res_ctx,
            RenderSurface&& surface,
            VkExtent2D initial_extent,
            bool enable_vsync,
            bool enable_present_scaling = false
        );

        explicit PresentContext(
            ConstructionKey,
            ResourceContext& res_ctx,
            RenderSurface&& surface
        );

        ~PresentContext();

        PresentContext(const PresentContext&)            = delete;
        PresentContext& operator=(const PresentContext&) = delete;

        [[nodiscard]] SwapchainProvider* provider() noexcept
        {
            return closed_ ? nullptr : provider_.get();
        }
        [[nodiscard]] const SwapchainProvider* provider() const noexcept
        {
            return closed_ ? nullptr : provider_.get();
        }

        [[nodiscard]] bool needsRebuild() const noexcept
        {
            return !closed_ && provider_ && provider_->needsRebuild();
        }

        /// Complete all presentation work before ownership teardown. Idempotent;
        /// every published PresentContext must be closed explicitly so a queue
        /// failure can travel through Expected rather than disappear in a destructor.
        [[nodiscard]] Expected<void> close() noexcept;

        /// Mark the presentation boundary terminal after Vulkan has reported
        /// VK_ERROR_DEVICE_LOST. Queue completion cannot be proven in that
        /// state, but the device-loss error is already the authoritative
        /// terminal and object destruction is the only legal teardown path.
        void acknowledgeDeviceLoss() noexcept
        {
            closed_ = true;
        }

        /// swapchain 重建(调用方先 waitAllFences——帧级职责);信号量环
        /// 随 imageCount 变化同步重配。
        [[nodiscard]] Expected<void> rebuild();

        /// 本帧 acquire 的完整产物:图像 + 本次消费的 acquire sem + 该
        /// image 对应的 present sem(submit 端 wait/signal 直接取用,
        /// FrameDriver 不再自持呈现信号量)。
        struct Acquired
        {
            bool        valid{false};
            uint32_t    image_index{0};
            VkImage     image{VK_NULL_HANDLE};
            VkImageView view{VK_NULL_HANDLE};
            VkExtent2D  extent{0, 0};
            VkSemaphore acquire_sem{VK_NULL_HANDLE};
            VkSemaphore present_sem{VK_NULL_HANDLE};
        };

        /// acquire 下一图像;成功才轮转 acquire 环(可恢复的无图像状态下 sem
        /// 未被消费,原位复用——不错位)。OUT_OF_DATE/SURFACE_LOST 等会标记
        /// needsRebuild 并返回成功的 invalid 值；其余 VkResult 走 Expected。
        [[nodiscard]] Expected<Acquired> acquire();

        /// present + 把 OUT_OF_DATE/SUBOPTIMAL/SURFACE_LOST 归一为重建标记
        ///(可恢复态),其余错误上抛。
        [[nodiscard]] Expected<void> present(
            uint32_t image_index,
            VkSemaphore wait_sem
        );

    private:
        /// 按当前 imageCount 重配两套信号量(acquire 环 = imageCount+1)。
        [[nodiscard]] Expected<void> resyncSemaphores();

        ResourceContext&                   res_ctx_;
        RenderSurface                      surface_;

        std::vector<gapi::vk::Semaphore>   acquire_ring_;
        uint32_t                           acquire_cursor_{0};
        std::vector<gapi::vk::Semaphore>   present_per_image_;
        // Declared last so reverse member destruction removes the swapchain
        // before its semaphore sets, then finally the surface.
        std::unique_ptr<SwapchainProvider> provider_;
        bool                               close_required_{false};
        bool                               closed_{false};
    };

} // namespace lux::render
