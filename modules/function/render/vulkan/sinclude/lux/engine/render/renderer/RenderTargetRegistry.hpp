#pragma once
/**
 * @file RenderTargetRegistry.hpp
 * @brief 一个渲染实例拥有的全部渲染目标(离屏 / 呈现面)及其合成链。
 *
 * 从 GeneralRenderServer::Impl 搬出来的。搬的理由:这里的每一个词——渲染目标、
 * 合成层、图像池、呈现上下文——**都是渲染层词汇,没有一个属于线协议**。它长在
 * L5 只是因为当初在那里写下的;而它住在 L5 直接导致帧编排也搬不动(编排的主体
 * 就是"遍历 target、逐层渲染"),UI 想扩展就只能整份复制 tick。
 *
 * 本类只管**目标本身**:增删查、层链维护、池的创建与退休。
 * 它不驱动帧 —— 谁在什么时候渲染这些目标,是 FrameOrchestrator 的事。
 *
 * 线程:仅渲染线程。
 */

#include <lux/engine/function/render/client/core/FeatureHandle.hpp>       // RenderTargetId / ViewHandle
#include <lux/engine/function/render/client/core/RenderSceneId.hpp>
#include <lux/engine/function/render/client/RenderTargetLayout.hpp>
#include <lux/engine/render/targets/RenderTargetBinding.hpp>
#include <lux/engine/render/targets/OffscreenImagePool.hpp>
#include <lux/engine/render/targets/PresentContext.hpp>
#include <lux/engine/function/visibility.h>

#include <lux/cxx/container/BasicSparseSet.hpp>   // SlotKeyAutoSparseSet
#include <lux/cxx/container/SmallVector.hpp>

#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

namespace lux::render
{
    class ResourceContext;
    class SwapchainProvider;

    // ─────────────────────────────────────────────────────────────────────
    //  RenderTargetEntry — 一个渲染目标
    // ─────────────────────────────────────────────────────────────────────
    /// Offscreen 态拥有图像池;Surface 态拥有呈现上下文(surface/swapchain/
    /// 信号量环)。layers 是**有序合成链**:按序渲染,后者叠在前者之上。
    /// 一层在合成链中的位置。三个相位决策全部由它导出,不再由各方手算:
    ///   首层  → loadOp CLEAR、入层布局 UNDEFINED(内容可弃)
    ///   非首层 → loadOp LOAD、入层布局 COLOR_ATTACHMENT(自接力)
    ///   末层  → 收尾到 target 的终态(Surface 即 PRESENT_SRC_KHR)
    ///   非末层 → 终态改 COLOR_ATTACHMENT,把图像交棒给下一层
    struct LayerPhase
    {
        bool is_first{true};
        bool is_last{true};
    };

    struct RenderTargetEntry
    {
        enum class EKind : uint8_t { Offscreen, Surface };

        /// 合成链上的一层。两种形态:
        ///   SceneView    —— 渲染一个 (场景, 视图),经渲染图
        ///   CustomRecord —— 一段自定义录制(如 ImGui 叠加),直接录进命令缓冲
        ///
        /// 为什么要有第二种:ImGui 叠加做的三件事(入层屏障、loadOp 相位、末层
        /// 转 PRESENT)全都是"我在链中位置"的函数 —— 它本就是一层,只是此前
        /// 层的类型被写死成场景视图,于是叠加只能绕开链自己算相位,并把结果
        /// 手工同步进 target 的 layout(建层时一处、开关叠加时一处、录制时一处,
        /// 三处必须一致才不出错)。
        struct CompositeLayer
        {
            enum class EKind : uint8_t { SceneView, CustomRecord };

            /// 自定义录制:相位已由链算好,回调按它开 rendering / 收尾。
            using RecordFn = void (*)(void* user, VkCommandBuffer cmd,
                                      const RenderTargetBinding& binding,
                                      const LayerPhase& phase);

            EKind kind{EKind::SceneView};

            // kind == SceneView
            RenderSceneId scene_id{};
            ViewHandle    view_id{};

            // kind == CustomRecord
            RecordFn      record{nullptr};
            void*         user{nullptr};

            [[nodiscard]] static CompositeLayer sceneView(RenderSceneId s, ViewHandle v) noexcept
            {
                CompositeLayer l{};
                l.kind = EKind::SceneView;
                l.scene_id = s;
                l.view_id  = v;
                return l;
            }

            [[nodiscard]] static CompositeLayer customRecord(RecordFn fn, void* user) noexcept
            {
                CompositeLayer l{};
                l.kind   = EKind::CustomRecord;
                l.record = fn;
                l.user   = user;
                return l;
            }
        };

        EKind                               kind{EKind::Offscreen};
        uint32_t                            flags{0};   ///< kTargetFlag*(决定池类型/退休路由)
        RenderTargetLayout                  layout;
        std::unique_ptr<OffscreenImagePool> pool;       ///< Offscreen only
        std::unique_ptr<PresentContext>     present;    ///< Surface only
        lux::cxx::SmallVector<CompositeLayer, 2> layers;

        /// 挂起本 target 的 swapchain 重建。用于"改尺寸括号"语义:宿主在
        /// ResizeBegin..ResizeEnd 之间置位,期间不论事件驱动还是 present 报
        /// SUBOPTIMAL 都不重建,ResizeEnd 时对已定尺寸重建一次 —— 闭合
        /// caps 查询↔创建之间的 TOCTOU 竞速(停靠期 VUID 刷屏的根因)。
        bool rebuild_suspended{false};
    };

    // ─────────────────────────────────────────────────────────────────────
    //  RenderTargetRegistry
    // ─────────────────────────────────────────────────────────────────────
    class LUX_FUNCTION_PUBLIC RenderTargetRegistry
    {
    public:
        using Entry = RenderTargetEntry;
        using Set   = lux::cxx::SlotKeyAutoSparseSet<RenderTargetId, Entry>;

        /// 池创建扩展点:返回 nullptr = 不接管,本类造默认 OffscreenImagePool。
        /// UI 层经此把 SAMPLED 目标的池换成自带 ImGui 描述符的子类。
        using MakeTargetPoolFn = std::unique_ptr<OffscreenImagePool> (*)(
            RenderTargetRegistry& reg,
            const RenderTargetLayout& layout,
            VkExtent2D extent, uint32_t target_flags
        );

        /// 池退休扩展点:取走 pool 并返回 true = 接管(调用方自管退休列表);
        /// 返回 false = 本类按 fence 水位延迟释放。
        using RetireTargetPoolFn = bool (*)(
            RenderTargetRegistry& reg,
            std::unique_ptr<OffscreenImagePool>& pool,
            uint32_t target_flags,
            uint64_t retire_serial
        );

        RenderTargetRegistry() = default;
        ~RenderTargetRegistry() = default;

        RenderTargetRegistry(const RenderTargetRegistry&)            = delete;
        RenderTargetRegistry& operator=(const RenderTargetRegistry&) = delete;
        RenderTargetRegistry(RenderTargetRegistry&&)                 = delete;
        RenderTargetRegistry& operator=(RenderTargetRegistry&&)      = delete;

        /// 造默认池要用到设备资源上下文与在飞帧数,故在设备就绪后注入。
        void init(ResourceContext& res_ctx, uint32_t frames_in_flight) noexcept
        {
            res_ctx_ = &res_ctx;
            frames_in_flight_ = frames_in_flight;
        }

        [[nodiscard]] ResourceContext& resourceContext() const noexcept { return *res_ctx_; }
        [[nodiscard]] uint32_t framesInFlight() const noexcept { return frames_in_flight_; }

        /// 池回调的用户数据(UI 在此挂它的 UIState)。
        void  setUserData(void* user) noexcept { user_ = user; }
        [[nodiscard]] void* userData() const noexcept { return user_; }

        void setPoolCallbacks(MakeTargetPoolFn make, RetireTargetPoolFn retire) noexcept
        {
            make_pool_cb_   = make;
            retire_pool_cb_ = retire;
        }

        // ── 容器 ────────────────────────────────────────────────────────
        [[nodiscard]] Set&       all()       noexcept { return targets_; }
        [[nodiscard]] const Set& all() const noexcept { return targets_; }

        [[nodiscard]] RenderTargetId insert(Entry&& e) { return targets_.insert(std::move(e)); }
        [[nodiscard]] Entry*   tryGet(RenderTargetId key) noexcept { return targets_.tryGet(key); }
        void erase(RenderTargetId key) { targets_.erase(key); }
        void clear() { targets_.clear(); surface_target_ = {}; }

        // ── 主 Surface(过渡期单窗;多窗随逐 entry 取)────────────────────
        void setSurfaceTarget(RenderTargetId id) noexcept { surface_target_ = id; }
        [[nodiscard]] RenderTargetId surfaceTargetId() const noexcept { return surface_target_; }

        [[nodiscard]] Entry* surfaceTarget() noexcept
        {
            return surface_target_.isValid()
                ? targets_.tryGet(surface_target_)
                : nullptr;
        }
        [[nodiscard]] PresentContext*    surfacePresent() noexcept;
        [[nodiscard]] SwapchainProvider* swapchainProvider() noexcept;

        // ── 按 (scene, view) 反查离屏目标 ───────────────────────────────
        [[nodiscard]] RenderTargetId findOffscreenKeyByView(RenderSceneId s, ViewHandle v) const noexcept;
        [[nodiscard]] Entry*         findOffscreenByView(RenderSceneId s, ViewHandle v) noexcept;

        /// 从层链摘除一个 (scene, view);Offscreen 空链时池转延迟释放并删 entry
        /// (统一销毁门控)。返回是否有摘除发生。
        bool detachLayerAndReapIfEmpty(
            RenderTargetId key,
            RenderSceneId s,
            ViewHandle v,
            uint64_t retire_serial
        );

        // ── 池的创建与退休 ──────────────────────────────────────────────
        [[nodiscard]] std::unique_ptr<OffscreenImagePool> makeTargetPool(
            const RenderTargetLayout& layout,
            VkExtent2D extent,
            uint32_t target_flags
        );

        /// **统一销毁门控**:所有 target 池的退休都必须走这里 —— 直接 vkDestroy
        /// 或直接进延迟表都会绕开扩展点接管的描述符退休语义。
        void retireTargetPool(Entry& t, uint64_t retire_serial);

        /// 老化回收已越过 GPU 完成水位的延迟池。
        void collectRetiredPools(uint64_t gpu_completed);

        /// 设备空闲时的整体清理(关服路径)。
        void shutdown();

    private:
        ResourceContext*    res_ctx_{nullptr};
        uint32_t            frames_in_flight_{0};
        void*               user_{nullptr};

        MakeTargetPoolFn    make_pool_cb_{nullptr};
        RetireTargetPoolFn  retire_pool_cb_{nullptr};

        Set                 targets_;
        RenderTargetId      surface_target_{};

        /// 延迟释放的离屏池(按 retire_serial 记账,越过 GPU 完成水位才析构)。
        std::vector<std::pair<uint64_t, std::unique_ptr<OffscreenImagePool>>>
            deferred_pools_;
    };

} // namespace lux::render
