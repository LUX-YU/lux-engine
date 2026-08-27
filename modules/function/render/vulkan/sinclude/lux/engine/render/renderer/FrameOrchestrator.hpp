#pragma once
// ============================================================================
//  FrameOrchestrator — 一帧怎么走。
//
//  这里曾只是个薄壳(持一个 FrameClock + 转发两个调用),真正的编排是
//  GeneralRenderServer::tick() 的函数体。后果:想在其中几处插东西(UI 的副视口
//  present、ImGui 叠加)只能整份复制 tick —— UIRenderServer::tick 因此长到
//  393 行,是基类那 152 行的一份分叉。
//
//  现在编排是**对象**:三个具名阶段。分成三段而不是一次调用,是因为通信层的
//  记账有硬时序 —— staging 退休必须在 beginRenderFrame 等到栅栏之后、渲染开始
//  之前。阶段边界正落在这些约束上,所以不需要发明任何回调钩子。
//
//      beginRenderFrame   判定可渲染性 → 重建所有需要重建的 Surface target
//                         → FrameDriver 开帧(等栅栏 + acquire)
//      renderTargets      上传 → 逐 target 逐层渲染 → 各场景 endViewFrame
//      endRenderFrame     结帧提交/呈现 → 目标池老化回收
//
//  线程:仅渲染线程。
// ============================================================================

#include <lux/engine/function/render/client/core/FrameStamp.hpp>
#include <lux/engine/render/renderer/FrameDriver.hpp> // FrameRuntime
#include <lux/engine/render/renderer/RenderTargetRegistry.hpp>
#include <lux/engine/render/targets/RenderTargetBinding.hpp>
#include <lux/engine/function/visibility.h>

#include <cstdint>
#include <vector>

namespace lux::render
{

    class Renderer;
    class RenderScene;
    struct View;

    // ─────────────────────────────────────────────────────────────────────────
    //  SceneViewBatch — 一帧内被渲染到的 (场景, 视图) 批
    // ─────────────────────────────────────────────────────────────────────────
    // 原先住在 comm/RenderTickPipeline.hpp。但它的词汇是场景与视图,和线协议毫无
    // 关系;它长在那里只是因为 tick 长在那里。编排下沉后它跟着归位。
    struct SceneViewBatchItem
    {
        RenderScene* scene{nullptr};
        View* view{nullptr};
        uint32_t cross_view_index{0};
    };

    /// 逐帧的场景/视图批。用紧凑的场景表 + 计数器代替每帧的哈希表。
    class LUX_FUNCTION_PUBLIC SceneViewBatch
    {
    public:
        void clear()
        {
            items_.clear();
            scenes_.clear();
            scene_counts_.clear();
            last_scene_ = nullptr;
            last_scene_index_ = 0;
        }

        void add(RenderScene* scene, View* view)
        {
            if (!scene || !view)
                return;
            const uint32_t cv = nextCrossViewIndex(scene);
            items_.push_back(SceneViewBatchItem{
                .scene = scene,
                .view = view,
                .cross_view_index = cv,
            }
            );
        }

        [[nodiscard]] const std::vector<SceneViewBatchItem>& items() const noexcept
        {
            return items_;
        }

        [[nodiscard]] const std::vector<RenderScene*>& touchedScenes() const noexcept
        {
            return scenes_;
        }

    private:
        uint32_t nextCrossViewIndex(RenderScene* scene)
        {
            if (last_scene_ == scene)
            {
                const uint32_t idx = last_scene_index_;
                return scene_counts_[idx]++;
            }

            for (uint32_t idx = 0; idx < scenes_.size(); ++idx)
            {
                if (scenes_[idx] != scene)
                    continue;
                last_scene_ = scene;
                last_scene_index_ = idx;
                return scene_counts_[idx]++;
            }

            const uint32_t idx = static_cast<uint32_t>(scenes_.size());
            scenes_.push_back(scene);
            scene_counts_.push_back(1u);
            last_scene_ = scene;
            last_scene_index_ = idx;
            return 0u;
        }

        std::vector<SceneViewBatchItem> items_;
        std::vector<RenderScene*> scenes_;
        std::vector<uint32_t> scene_counts_;
        RenderScene* last_scene_{nullptr};
        uint32_t last_scene_index_{0};
    };

    /// 本帧跨三个阶段共享的运行态。调用方在栈上开一个,依次传给三个阶段。
    ///
    /// 住在命名空间层(而不是 FrameOrchestrator 的嵌套类型),是为了让 L5 那个
    /// **安装**的 RenderServer.hpp 能只前置声明它 —— 派生服务器的 tick 三阶段
    /// 按引用收发它,于是安装面不必认识 FrameRuntime、RenderTargetEntry 与
    /// 一串 Vulkan 句柄。嵌套类型没法单独前置声明,这是唯一的理由。
    struct FrameTickState
    {
        FrameRuntime rt{};
        RenderTargetBinding present_binding{}; ///< 必须活过 renderTargets(rt 指向它)
        PresentContext* surface_present{nullptr};
        RenderTargetEntry* surface_target{nullptr};

        /// 主 Surface 之外、本帧成功 acquire 的 Surface target。
        ///
        /// 多窗的形状:各自 acquire 自己的图像,**全部录进同一个命令缓冲**,
        /// acquire/present 信号量折入同一次提交(FrameRuntime 的外部等待/信号
        /// 集),结帧之后各自 present。主 Surface 走 FrameDriver 的既有路径,
        /// 因为 fence/命令缓冲的生命周期归它;其余的在这里补齐。
        ///
        /// 只有一个 Surface target 时本表为空 —— 单窗路径逐字不变。
        struct AcquiredSurface
        {
            PresentContext* present{nullptr};
            RenderTargetBinding binding{};
            uint32_t image_index{0};
            VkSemaphore present_sem{VK_NULL_HANDLE};
            const RenderTargetEntry* target{nullptr};
        };
        lux::cxx::SmallVector<AcquiredSurface, 4> extra_surfaces;
    };

    class LUX_FUNCTION_PUBLIC FrameOrchestrator
    {
    public:
        /// beginRenderFrame 的结果。
        enum class EFrameStart
        {
            NoTarget, ///< 没有任何可渲染 target —— 直接结束本 tick,不必做任何收尾记账
            Skip,     ///< 已开帧但拿不到命令缓冲(最小化 / 重建失败)—— 调用方要走收尾记账
            Ready,    ///< 正常,继续渲染
        };

        explicit FrameOrchestrator(uint32_t frames_in_flight) : frame_clock_(frames_in_flight)
        {
        }

        [[nodiscard]] FrameStamp beginTick(uint32_t image_index_hint = 0)
        {
            current_stamp_ = frame_clock_.beginTick(image_index_hint);
            return current_stamp_;
        }

        void patchImageIndex(uint32_t image_index) noexcept
        {
            current_stamp_.image_index = image_index;
        }

        [[nodiscard]] const FrameStamp& stamp() const noexcept
        {
            return current_stamp_;
        }
        [[nodiscard]] uint32_t framesInFlight() const noexcept
        {
            return frame_clock_.framesInFlight();
        }

        void beginFrame(Renderer& renderer) const;
        void endFrame(Renderer& renderer) const;

        // ── 帧编排三阶段 ─────────────────────────────────────────────────────

        /// 阶段 1。判定是否有可渲染 target;重建所有 needsRebuild 的 Surface target
        /// (**必须先于开帧** —— 重建要 waitAllFences,帧中调用会死锁:当前槽的
        /// fence 已 reset);再经 FrameDriver 开帧(等栅栏 + acquire + 开命令缓冲)。
        ///
        /// 返回 Ready/Skip 时栅栏已等到,调用方此刻(且仅此刻)才可退休上一槽的记账。
        /// fence/command-buffer 状态失败则返回 RenderError；调用方必须停止当前
        /// device session，不能把它降级成 Skip 后继续复用槽位。
        [[nodiscard]] Expected<EFrameStart>
        beginRenderFrame(RenderTargetRegistry& targets, FrameDriver* driver, FrameTickState& st);

        /// 阶段 2。上传 → 逐离屏 target 的层链渲染 → 主 Surface 的层链渲染 →
        /// 各被触及场景 endViewFrame。
        void
        renderTargets(RenderTargetRegistry& targets, Renderer& renderer, SceneViewBatch& batch, FrameTickState& st);

        /// 阶段 3。结帧(提交 + 呈现)→ 目标池按栅栏证实的完成水位老化回收。
        /// command-buffer end / queue submit 的永久失败沿 Expected 返回，且错误
        /// 发生后不会继续呈现额外 Surface 或推进退休水位。
        [[nodiscard]] Expected<void>
        endRenderFrame(RenderTargetRegistry& targets, FrameDriver* driver, FrameTickState& st);

    private:
        FrameClock frame_clock_;
        FrameStamp current_stamp_{};
    };

} // namespace lux::render
