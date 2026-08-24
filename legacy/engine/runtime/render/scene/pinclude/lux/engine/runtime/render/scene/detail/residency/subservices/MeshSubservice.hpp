#pragma once

#include <lux/engine/function/render/client/RenderUploadClient.hpp>
/**
 * @file MeshSubservice.hpp — 网格域子服务(驻留 T8,抄 T7 样)。
 *
 * 「数据 → `uploadMesh` RPC(StandardMeshStack 特性域)→ 回执双条件验证
 * → SubmitDone」+ `destroyMesh` 销毁。零 ecs 词汇、零自主(J6-六修)。
 *
 * 与贴图域的一处不同:网格上传不走核心协议,走特性动态 op —— op-id 从
 * **进程域 FeatureCatalog** 按名字取("StandardMeshStack")。目录是装配后
 * 只读的进程域数据(句柄才是场景域状态,上传/销毁不需要句柄),所以本
 * 子服务构造时直接持目录引用,没有每场景 bind —— 这正是 T18 通道化之后
 * 「场景关联消失」的终态形状,提前落位。
 *
 * 目录缺条目(特性类型没注册)= **响亮终败**,不是旧 GpuResourceCache
 * 的「代理整条 no-op」—— 静默 no-op 意味着回执永不到达,行卡
 * UPLOADING 永不解;恰一次回执契约必须由本层守住。
 *
 * 前置(编排保证):submit 时 CPU 数据已在账、帧 builder 开着;destroy
 * 同相位。宿主装配序保证目录活得比一切表行久(T11)。在途回执由
 * owner-reply reaper 统一持有；Forced close 只会让已进 MainThreadScheduler 的回执
 * 进入 compensation-only，不会丢非空 mesh owner。
 */

#include <lux/engine/runtime/render/scene/visibility.h>
#include <lux/engine/runtime/render/scene/detail/residency/RenderResourceService.hpp>

#include <memory>

namespace lux::asset  { class AssetManager; }
namespace lux::render
{
    class RenderControlSession;
    class FeatureCatalog;
    struct MeshUploadedReply;
}

namespace lux::runtime
{
    namespace detail { template <class Reply> class OwnerReplyReaper; }

    class LUX_RUNTIME_RENDER_SCENE_PUBLIC MeshSubservice final
        : public IRenderResourceSubservice
    {
    public:
        MeshSubservice(lux::render::RenderControlSession& control,
                       lux::render::RenderUploadClient upload,
                       lux::asset::AssetManager&          assets,
                       const lux::render::FeatureCatalog& catalog,
                       TryPostToMain                      post_main) noexcept;
        ~MeshSubservice() override;

        MeshSubservice(const MeshSubservice&)            = delete;
        MeshSubservice& operator=(const MeshSubservice&) = delete;

        [[nodiscard]] lux::ecs::EResourceDomain domain() const override;

        void submit(const lux::asset::asset_id_t& id, SubmitDone done) override;
        void destroy(std::uint64_t handle_bits) noexcept override;
        [[nodiscard]] std::size_t pendingReplies() const noexcept override;
        [[nodiscard]] bool ownerControlsQuiescent() const noexcept override;
        void abandonPendingReplies() noexcept override;

    private:
        void trySubmit(const lux::asset::asset_id_t& id, SubmitDone done);

        lux::render::RenderControlSession* control_{nullptr};
        lux::render::RenderUploadClient upload_;
        lux::asset::AssetManager*          assets_{nullptr};
        const lux::render::FeatureCatalog* catalog_{nullptr};
        TryPostToMain                      post_main_{};
        std::unique_ptr<detail::OwnerReplyReaper<
            lux::render::MeshUploadedReply>> replies_;
    };

} // namespace lux::runtime
