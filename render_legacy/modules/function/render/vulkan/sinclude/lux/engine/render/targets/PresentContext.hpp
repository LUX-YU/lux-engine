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
 * vkQueuePresentKHR。语义 owner 析构只交还 backing;原服务器退休记录负责
 * 呈现队列安全点、错误和最终物理释放。窗口寿命必须覆盖原 TargetReleased 回执。
 *
 * Thread model: render-thread only。
 */

#include <lux/engine/function/render/client/core/Errors.hpp>
#include <lux/engine/function/visibility.h>
#include <lux/engine/gapi/vk/Semaphore.hpp>
#include <lux/engine/render/gpu/RenderSurface.hpp>
#include <lux/engine/render/targets/SwapchainProvider.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <vector>
#include <vulkan/vulkan.h>

namespace lux::render
{
    class ResourceContext;

    namespace detail
    {
        [[nodiscard]] LUX_FUNCTION_PUBLIC Expected<void> waitPresentQueueIdle(
            VkQueue queue,
            PFN_vkQueueWaitIdle wait_idle
        ) noexcept;

        struct PresentSemaphoreCreateOps final
        {
            PFN_vkCreateSemaphore create_semaphore{nullptr};
            PFN_vkDestroySemaphore destroy_semaphore{nullptr};
        };

        /// Owns a complete replacement semaphore set until PresentContext has
        /// adopted every handle. A failed call rolls back the partially-created
        /// batch in strict reverse creation order.
        class LUX_FUNCTION_PUBLIC PresentSemaphoreCreateCandidate final
        {
        public:
            ~PresentSemaphoreCreateCandidate() noexcept;

            PresentSemaphoreCreateCandidate(const PresentSemaphoreCreateCandidate&) = delete;
            PresentSemaphoreCreateCandidate& operator=(const PresentSemaphoreCreateCandidate&) = delete;
            PresentSemaphoreCreateCandidate(PresentSemaphoreCreateCandidate&& other) noexcept;
            PresentSemaphoreCreateCandidate& operator=(PresentSemaphoreCreateCandidate&& other) noexcept;

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

            [[nodiscard]] VkSemaphore acquireSemaphore(std::uint32_t index) const noexcept
            {
                return acquire_semaphores_[index];
            }

            [[nodiscard]] VkSemaphore presentSemaphore(std::uint32_t index) const noexcept
            {
                return present_semaphores_[index];
            }

            void commit() noexcept;

        private:
            PresentSemaphoreCreateCandidate(VkDevice device, PresentSemaphoreCreateOps ops) noexcept;

            void rollback() noexcept;
            void disarm() noexcept;

            VkDevice device_{VK_NULL_HANDLE};
            PresentSemaphoreCreateOps ops_{};
            std::vector<VkSemaphore> acquire_semaphores_;
            std::vector<VkSemaphore> present_semaphores_;
        };
    } // namespace detail

    struct PresentBacking;

    enum class EPresentRetirementReason
    {
        NORMAL,
        DEVICE_LOST
    };

    /// Fixed-address responsibility inside the original server release record.
    /// It outlives its semantic context and never exposes presentation operations.
    class LUX_FUNCTION_PUBLIC PresentRetirement final
    {
    public:
        PresentRetirement() noexcept;
        ~PresentRetirement() noexcept;
        PresentRetirement(const PresentRetirement&) = delete;
        PresentRetirement& operator=(const PresentRetirement&) = delete;
        PresentRetirement(PresentRetirement&&) = delete;
        PresentRetirement& operator=(PresentRetirement&&) = delete;

        [[nodiscard]] bool pending() const noexcept;
        [[nodiscard]] Expected<void> settle(EPresentRetirementReason reason) noexcept;

    private:
        friend class PresentContext;
        std::unique_ptr<PresentBacking> backing_;
    };

    class LUX_FUNCTION_PUBLIC PresentContext
    {
    public:
        /// surface 所有权移交进来;内部建 swapchain + 两套信号量。
        /// 失败时接管并销毁传入的 surface(调用方无需善后)。
        [[nodiscard]] static Expected<std::unique_ptr<PresentContext>> create(
            ResourceContext& res_ctx,
            PresentRetirement& retirement,
            RenderSurface&& surface,
            VkExtent2D initial_extent,
            bool enable_vsync,
            bool enable_present_scaling = false
        );

        ~PresentContext();

        PresentContext(const PresentContext&) = delete;
        PresentContext& operator=(const PresentContext&) = delete;

        [[nodiscard]] SwapchainProvider* provider() noexcept;
        [[nodiscard]] const SwapchainProvider* provider() const noexcept;
        [[nodiscard]] bool needsRebuild() const noexcept;

        /// swapchain 重建(调用方先 waitAllFences——帧级职责);信号量环
        /// 随 imageCount 变化同步重配。
        [[nodiscard]] Expected<void> rebuild();

        /// 本帧 acquire 的完整产物:图像 + 本次消费的 acquire sem + 该
        /// image 对应的 present sem(submit 端 wait/signal 直接取用,
        /// FrameDriver 不再自持呈现信号量)。
        struct Acquired
        {
            uint32_t image_index{0};
            VkImage image{VK_NULL_HANDLE};
            VkImageView view{VK_NULL_HANDLE};
            VkExtent2D extent{0, 0};
            VkSemaphore acquire_sem{VK_NULL_HANDLE};
            VkSemaphore present_sem{VK_NULL_HANDLE};
        };

        /// acquire 下一图像;成功才轮转 acquire 环(可恢复的无图像状态下 sem
        /// 未被消费,原位复用——不错位)。OUT_OF_DATE/SURFACE_LOST 等会标记
        /// needsRebuild 并返回成功的空 optional；其余 VkResult 走 Expected。
        [[nodiscard]] Expected<std::optional<Acquired>> acquire();

        /// present + 把 OUT_OF_DATE/SUBOPTIMAL/SURFACE_LOST 归一为重建标记
        ///(可恢复态),其余错误上抛。
        [[nodiscard]] Expected<void> present(uint32_t image_index, VkSemaphore wait_sem);

    private:
        PresentContext(PresentRetirement& retirement, std::unique_ptr<PresentBacking> backing) noexcept;

        PresentRetirement& retirement_;
        std::unique_ptr<PresentBacking> backing_;
    };

} // namespace lux::render
