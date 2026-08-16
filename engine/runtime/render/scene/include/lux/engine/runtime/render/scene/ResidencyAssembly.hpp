#pragma once
/**
 * @file ResidencyAssembly.hpp — 驻留三件套的宿主装配唯一实现(驻留 T11)。
 *
 * 表 + 服务(四域子服务)+ 编排 Context 的构造与接线**只写这一遍**:
 * 三个宿主(LuxEditor / GameHost / android)各自手写装配必然漂
 * (RenderDiagnostics 的教训 —— 渲染桥 sink 曾三家三样)。宿主保有的
 * 只剩:总线订阅转接(账本四事实 → onXxx)与失败观察的 publish。
 * 关停状态机由本 owner 的 close sender 内聚，不再复制到三个宿主。
 *
 * 生命期:进程域,宿主拥有,切场景不重建(J6-六修)。构造前置:
 * session/assets/catalog/executor 已就位(编辑器在 7a-bis,游戏宿主在
 * 缓存装配段)。关停序铁律(疤痕,探针 material_residency_gpu_probe
 * 曾整进程挂死):所有场景 tearDown 后、渲染线程 stop 前，在宿主安全点
 * 连接 `closeAsync()`。它内聚 AsyncScope stop/join、全注册域 reply
 * reaper 和 teardown；只有 terminal 结果才允许外层继续停
 * executor/runtime/render thread。
 * owner-creating 叶子 RPC 不可取消；owner stop 之后的迟到回执仍须被观察，
 * 让域 reaper/RAII transaction 补偿其句柄。
 */

#include <lux/engine/runtime/render/scene/visibility.h>
#include <lux/engine/runtime/render/scene/TextureStreamingBudget.hpp>
#include <lux/engine/ecs/render/ResidencyCallbacks.hpp>
#include <lux/cxx/core/move_only_function.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

namespace lux::asset  { class AssetManager; }
namespace lux::ecs    { struct RenderResourceFailed; }
namespace lux::exec   { class AsyncRuntime; }
namespace lux::asset_runtime { class AssetClient; }
namespace lux::render {
    class RenderControlSession;
    class RenderUploadClient;
    class FeatureCatalog;
}

namespace lux::runtime
{
    class ResidencyCloseSender;
    class ResidencyAssembly;
    struct ResidencyCloseReport;

    namespace detail
    {
        LUX_RUNTIME_RENDER_SCENE_PUBLIC void subscribeResidencyClose(
            ResidencyAssembly& assembly,
            lux::cxx::move_only_function<void(ResidencyCloseReport)>
                completion) noexcept;
    }
    enum class EResidencyCloseStatus : std::uint8_t
    {
        Closed,
        AlreadyClosed,
        CloseInProgress,
    };

    [[nodiscard]] constexpr std::string_view toString(
        EResidencyCloseStatus status) noexcept
    {
        switch (status)
        {
        case EResidencyCloseStatus::Closed:                return "closed";
        case EResidencyCloseStatus::AlreadyClosed:         return "already_closed";
        case EResidencyCloseStatus::CloseInProgress:       return "close_in_progress";
        }
        return "unknown";
    }

    struct ResidencyCloseReport
    {
        EResidencyCloseStatus status{EResidencyCloseStatus::Closed};
        std::size_t pending_owner_replies{0u};
        bool inflight_work{false};

        [[nodiscard]] constexpr bool clean() const noexcept
        {
            return (status == EResidencyCloseStatus::Closed
                    || status == EResidencyCloseStatus::AlreadyClosed) &&
                pending_owner_replies == 0u && !inflight_work;
        }

        /// A terminal report permits the composition root to continue with
        /// runtime/render-thread shutdown. CloseInProgress retains every
        /// borrowed dependency; the sender remains connected until a later
        /// main safe point completes the same close state.
        [[nodiscard]] constexpr bool terminal() const noexcept
        {
            return clean();
        }

        [[nodiscard]] constexpr bool retryable() const noexcept
        {
            return status == EResidencyCloseStatus::CloseInProgress;
        }
    };

    /// Owner-thread snapshot used by host watchdog diagnostics.  It is a
    /// read-only description of the existing close protocol; taking it never
    /// pumps work or changes admission.
    struct ResidencyCloseSnapshot
    {
        bool closing{false};
        bool scope_close_started{false};
        bool scope_closed{false};
        std::uint32_t active_call_depth{0u};
        std::size_t close_waiters{0u};
        std::size_t rows_unloaded{0u};
        std::size_t rows_loading{0u};
        std::size_t rows_uploading{0u};
        std::size_t rows_ready{0u};
        std::size_t rows_failed{0u};
        std::size_t mesh_replies{0u};
        std::size_t texture_replies{0u};
        std::size_t material_replies{0u};
        std::size_t material_shader_replies{0u};
        std::size_t material_upload_replies{0u};
        std::size_t material_instance_replies{0u};
        std::size_t live_gpu_leases{0u};
        std::size_t release_control_references{0u};
        std::size_t operation_control_references{0u};
        bool domain_owner_controls_quiescent{false};
    };

    class LUX_RUNTIME_RENDER_SCENE_PUBLIC ResidencyAssembly
    {
    public:
        /// 终态失败的观察出口(宿主接 publish;UI 订阅是 T14)。
        using FailureSink =
            lux::cxx::move_only_function<void(
                const lux::ecs::RenderResourceFailed&)>;

        ResidencyAssembly(lux::render::RenderControlSession& control,
                          lux::render::RenderUploadClient upload,
                          lux::asset::AssetManager&          assets,
                          const lux::render::FeatureCatalog& catalog,
                          lux::asset_runtime::AssetClient    asset_client,
                          lux::exec::AsyncRuntime&           async,
                          FailureSink                        failure_sink,
                          TextureStreamingBudget texture_streaming = {});
        /// 宿主必须在 RenderFrameSession/AsyncRuntime 仍可推进且渲染线程
        /// stop 之前显式调用 close()，由它提交 GPU 补偿。析构是一条
        /// RAII 安全后盾：依赖仍活着时发起同一份关闭状态机；
        /// 若协议不能立即到终态则响亮终止，绝不把被截断的关停伪装
        /// 成 sender 已 join / owner 已安全释放。
        ///
        /// Hard precondition: destruction must not occur inside a callback
        /// dispatched by this assembly or inside AsyncRuntime::drainMainThreadCompletions().
        /// close() reports CloseInProgress in those stacks; the composition
        /// root must retain this object until the close sender becomes terminal.
        /// Destruction before that terminal signal is a fatal
        /// lifetime-contract violation because the borrowed dependencies must be
        /// retained for a later host safe point.
        ~ResidencyAssembly();

        ResidencyAssembly(const ResidencyAssembly&)            = delete;
        ResidencyAssembly& operator=(const ResidencyAssembly&) = delete;

        // ── 账本四事实(宿主的订阅转接到这里;主线程 safe point)───────
        void onUnreferenced  (const lux::asset::asset_id_t& id);
        void onInvalidated   (const lux::asset::asset_id_t& id);
        void onContentChanged(const lux::asset::asset_id_t& id);
        void onAssetRegistered(const lux::asset::asset_id_t& id);

        /// Fail-safe facade for DomainEvents/composition-root subscriptions. The
        /// returned functions keep only the same weak callback gate as ECS
        /// ResidencyCallbacks; they may safely outlive this assembly and then
        /// become no-ops. Invocation remains main-thread confined.
        struct AssetEventCallbacks final
        {
            using Fn =
                lux::cxx::move_only_function<void(
                    const lux::asset::asset_id_t&)>;

            Fn unreferenced;
            Fn invalidated;
            Fn content_changed;
            Fn registered;
        };
        [[nodiscard]] AssetEventCallbacks makeAssetEventCallbacks();

        /// 每世界胶水的注入点(包装配期填给 ResidencySubsystem):
        /// request→ensure 管道,await/watch→服务 RAII 票据(不透明包装)。
        [[nodiscard]] lux::ecs::ResidencyCallbacks makeCallbacks();

        /// 需要资源(胶水之外的零散请求点 —— 材质预览的贴图槽等;
        /// 边沿语义同胶水,去重 = 行状态,主线程 safe point 调)。
        void request(const lux::asset::asset_id_t& id,
                     lux::ecs::EResourceDomain     domain);

        /// Frame-safe-point maintenance for edge-driven texture mip demands.
        /// Query cadence and adoption limits are deployment-configured; this
        /// never waits for render or upload work.
        void tickTextureStreaming();

        /// 纯查表:行 READY 返回句柄位,否则 0。不推进状态机、不发命令 ——
        /// 帧关闭时也安全(ImGui paint 期查询;真正的 request 由异步编排
        /// 发往独立 UploadChannel)。旧 GpuResourceCache::peekTexture 的接班人。
        [[nodiscard]] std::uint64_t peekReadyBits(
            const lux::asset::asset_id_t& id) const;

        /// Non-mutating close/probe diagnostic. True while any residency row
        /// is still loading or uploading; it never pumps a scheduler.
        [[nodiscard]] bool hasInflight() const noexcept;

        [[nodiscard]] ResidencyCloseSnapshot closeSnapshot() const noexcept;

        // ── 关停面 ──────────
        /// Main-thread deterministic close. Owns the complete residency drain
        /// protocol: drain control/upload replies → pump in-flight pipelines →
        /// force table teardown → final reply/main drain. It never opens or
        /// publishes a frame. Idempotent; the residency
        /// state/service/reaper hot path adds no lock. AsyncScope retains stdexec's
        /// documented low-frequency control-plane synchronization.
        /// It may be started from a callback or non-owner thread: initiation is
        /// posted to MainThreadMailbox and every observer connects to the same close
        /// state. It never self-waits. MainCloseDriver keeps the assembly and
        /// borrowed dependencies alive while safe points advance the protocol.
        [[nodiscard]] ResidencyCloseSender closeAsync() noexcept;

    private:
        void subscribeClose(
            lux::cxx::move_only_function<void(ResidencyCloseReport)>
                completion) noexcept;
        class Impl;
        std::unique_ptr<Impl> impl_;

        friend class ResidencyCloseSender;
        friend void detail::subscribeResidencyClose(
            ResidencyAssembly&,
            lux::cxx::move_only_function<void(ResidencyCloseReport)>)
            noexcept;
    };

} // namespace lux::runtime
