#pragma once

#include <lux/engine/function/render/client/RenderUploadClient.hpp>
/**
 * @file TextureSubservice.hpp — 贴图域子服务(驻留 T7,四域端到端打样)。
 *
 * `IRenderResourceSubservice` 的第一个真实现:本域的「数据 →
 * `createTexture2D` 上传 RPC → 回执双条件验证 → SubmitDone 回执」配方,
 * 外加句柄销毁。**零 ecs 词汇、零自主**(J6-六修):何时 submit/destroy
 * 由引擎编排(RenderResourceOps 管道)决定,组件观察在每世界胶水。
 *
 * 配方原样搬自 GpuResourceCache::ensureTexture(T12 退役对象):
 *  - mip 决策:默认生成(导入贴图只带 mip 0,要缩小过滤);资产带
 *    NO_MIPS 意图(tileset/像素画/查找表 —— 缩小平均无意义)或压缩
 *    格式(链已烘焙或缺席)不生成;
 *  - 回执**双条件**验证(疤痕①):status==0 且句柄非空才算成功 ——
 *    泛化分派失败会送回 {status 0, null handle} 的默认回执,只看
 *    status 会当成功,坐实成永久重传环。
 *
 * 前置(编排保证,J9):submit 时 CPU 数据已在账、帧 builder 开着;
 * destroy 同样只在 builder 开着的相位被调。在途 continuation 由
 * owner-reply reaper 词法所有；stop/Forced close 后已到主队列的
 * 非空句柄仍会走 compensation-only 归还，不伪造 server cancellation。
 */

#include <lux/engine/runtime/render/scene/visibility.h>
#include <lux/engine/runtime/render/scene/TextureStreamingBudget.hpp>
#include <lux/engine/runtime/render/scene/detail/residency/RenderResourceService.hpp>

#include <memory>
#include <unordered_map>
#include <unordered_set>

namespace lux::asset  { class AssetManager; }
namespace lux::render
{
    class RenderControlSession;
    struct Texture2DCreatedReply;
}

namespace lux::runtime
{
    namespace detail { template <class Reply> class OwnerReplyReaper; }

    class LUX_RUNTIME_RENDER_SCENE_PUBLIC TextureSubservice final : public IRenderResourceSubservice
    {
    public:
        TextureSubservice(lux::render::RenderControlSession& control,
                          lux::render::RenderUploadClient upload,
                          lux::asset::AssetManager&   assets,
                          TryPostToMain               post_main,
                          TextureStreamingBudget budget) noexcept;
        ~TextureSubservice() override;

        TextureSubservice(const TextureSubservice&)            = delete;
        TextureSubservice& operator=(const TextureSubservice&) = delete;

        [[nodiscard]] lux::ecs::EResourceDomain domain() const override;

        void submit(const lux::asset::asset_id_t& id, SubmitDone done) override;
        void applyMipDemands(const lux::render::TextureMipDemandsReply& demands);
        void destroy(std::uint64_t handle_bits) noexcept override;
        [[nodiscard]] std::size_t pendingReplies() const noexcept override;
        [[nodiscard]] bool ownerControlsQuiescent() const noexcept override;
        void abandonPendingReplies() noexcept override;

    private:
        struct MipStreamingState final
        {
            std::unordered_map<std::uint64_t, lux::asset::asset_id_t>
                asset_by_handle;
            std::unordered_set<std::uint64_t> replacements_inflight;
        };

        void trySubmit(const lux::asset::asset_id_t& id, SubmitDone done);

        lux::render::RenderControlSession* control_{nullptr};
        lux::render::RenderUploadClient upload_;
        lux::asset::AssetManager*   assets_{nullptr};
        TryPostToMain               post_main_{};
        TextureStreamingBudget      budget_{};
        std::shared_ptr<MipStreamingState> mip_streaming_;
        std::unique_ptr<detail::OwnerReplyReaper<
            lux::render::Texture2DCreatedReply>> replies_;
        std::unique_ptr<detail::OwnerReplyReaper<
            lux::render::TextureMipRangeReplacedReply>> replacement_replies_;
    };
} // namespace lux::runtime
