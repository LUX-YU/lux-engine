#pragma once
/**
 * @file RenderResourceOps.hpp — 渲染资源编排的自由函数外观(驻留 T4R3,
 *       J6-七修)。
 *
 * **编排不是对象,是管道**(asio 外观):没有 Orchestrator 类 —— 长寿
 * 对象只剩状态表与服务两个数据体,流程是 `ensure` 发起的 stdexec 链
 * (链体在 .cpp,stdexec 选择性 TU):
 *
 *   loadAsset(exec, id)                 // 命中即过/在途共享/终态 set_error
 *     | let_value(submitSender(...))    // 域子服务上传,回执桥成 sender
 *     | then(markReady)                 // 表 READY + 服务扇出
 *     | upon_error(markFailed)          // 表 FAILED + 扇出 + 失败观察
 *     | upon_stopped(...)
 *
 * 三条完成路的归属(J9-4)由此成为编译期结构。去重仍是表:UNLOADED
 * 才 spawn(置 LOADING);链的每操作状态活在 operation state 里。
 *
 * 本头 stdexec-free(AsyncRuntime 前置声明);全部函数在主线程安全点调用。
 * 相位**调用。Context 是宿主建一次的 wiring owner；它内部的一份
 * generation control 把新请求 admission 与既有管道 continuation 分开，
 * sender 不再跨异步边界捕获 Context/Executor 裸引用。
 */

#include <lux/engine/runtime/render/scene/visibility.h>
#include <lux/engine/runtime/render/scene/detail/residency/RenderResourceStateManager.hpp>
#include <lux/engine/runtime/render/scene/detail/residency/RenderResourceService.hpp>
#include <lux/engine/runtime/assets/AssetLoadService.hpp>
#include <lux/cxx/core/move_only_function.hpp>

#include <memory>
#include <unordered_set>
#include <vector>

namespace lux::asset { class AssetManager; }
namespace lux::exec  { class AsyncRuntime; class AsyncScope; }

namespace lux::runtime::render_resource
{
    class ResidencyOperationControl;
    class Context;

    /// 需要资源(胶水的回调接到这里;调用前胶水先 service.await)。
    /// 行就绪 → 立即扇出;终态 → 失败扇出;读取/上传中 → 去重返回;
    /// 未读取 → 置读取中并 spawn 管道。
    LUX_RUNTIME_RENDER_SCENE_PUBLIC
    void ensure(Context&, const lux::asset::asset_id_t&,
                lux::ecs::EResourceDomain);

    /// One non-moving wiring owner per ResidencyAssembly. Synchronous helpers
    /// borrow its references only for the current call stack. Sender operation
    /// states carry the private generation control instead of `Context*` or
    /// `AsyncRuntime*`, so an INVALID generation can never reach a reused
    /// owner address.
    class LUX_RUNTIME_RENDER_SCENE_PUBLIC Context final
    {
    public:
        using FailureSink =
            lux::cxx::move_only_function<void(
                const lux::ecs::RenderResourceFailed&)>;

        Context(RenderResourceStateManager& table,
                RenderResourceService& service,
                lux::asset::AssetManager& assets,
                lux::asset_runtime::AssetClient asset_client,
                lux::exec::AsyncScope& tasks,
                lux::exec::AsyncRuntime& runtime,
                FailureSink failure_sink = {},
                std::unordered_set<lux::ecs::EResourceDomain>
                    no_domain_diagnosed = {});
        ~Context();

        Context(const Context&)            = delete;
        Context& operator=(const Context&) = delete;
        Context(Context&&)                 = delete;
        Context& operator=(Context&&)      = delete;

        /// Root requests and external asset facts are admitted only here.
        [[nodiscard]] bool acceptsNewOperations() const noexcept;
        /// Existing sender stages and close compensation may continue while
        /// draining; this transition is persistent across retryable close.
        void beginDraining() noexcept;
        [[nodiscard]] bool isDraining() const noexcept;
        /// Terminal proof: the AsyncScope is joined, reapers are empty and no
        /// sender still holds this generation. INVALID is permanent.
        [[nodiscard]] bool operationQuiescent() const noexcept;
        [[nodiscard]] std::size_t operationControlReferences() const noexcept;
        void invalidateAfterJoin() noexcept;

        RenderResourceStateManager& table;
        RenderResourceService&      service;
        lux::asset::AssetManager&   assets;
        lux::asset_runtime::AssetClient asset_client;
        /// Mandatory structured owner for every residency pipeline. The
        /// scope must stop/join before this wiring aggregate, its table, or
        /// its service can leave scope.
        lux::exec::AsyncScope&       tasks;
        lux::exec::AsyncRuntime&     runtime;
        FailureSink                  failure_sink;
        /// 「无域子服务」的一次性诊断记忆(可见不刷屏)。
        std::unordered_set<lux::ecs::EResourceDomain> no_domain_diagnosed;

    private:
        friend LUX_RUNTIME_RENDER_SCENE_PUBLIC void ensure(
            Context&,
            const lux::asset::asset_id_t&,
            lux::ecs::EResourceDomain
        );

        std::shared_ptr<ResidencyOperationControl> operation_control_;
    };

    // ── 账本四事实(宿主订阅接线;承载序 invalidated → content →
    //    unreferenced,registered 无序要求)──────────────────────────────
    LUX_RUNTIME_RENDER_SCENE_PUBLIC
    void onUnreferenced  (Context&, const lux::asset::asset_id_t&);
    LUX_RUNTIME_RENDER_SCENE_PUBLIC
    void onInvalidated   (Context&, const lux::asset::asset_id_t&);
    LUX_RUNTIME_RENDER_SCENE_PUBLIC
    void onContentChanged(Context&, const lux::asset::asset_id_t&);   // 波前级联
    LUX_RUNTIME_RENDER_SCENE_PUBLIC
    void onAssetRegistered(Context&, const lux::asset::asset_id_t&);  // 推式解封

    /// 依赖声明(域子服务构建期经编排;票据代持进表行,先钉新再放旧)。
    LUX_RUNTIME_RENDER_SCENE_PUBLIC
    void setDependencies(Context&, const lux::asset::asset_id_t&,
                         std::vector<lux::asset::asset_id_t> deps);

    // ── 宿主关停面 ──────────────────────────────────────────────────────
    LUX_RUNTIME_RENDER_SCENE_PUBLIC bool hasInflight(const Context&);
    /// 依赖序力扫(实例→材质→网格→贴图),不依赖事件。
    LUX_RUNTIME_RENDER_SCENE_PUBLIC void teardown(Context&);

} // namespace lux::runtime::render_resource
