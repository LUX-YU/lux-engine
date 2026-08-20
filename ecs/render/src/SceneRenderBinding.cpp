#include "lux/engine/ecs/render/SceneRenderBinding.hpp"
#include "lux/engine/ecs/render/RenderSpatialTransform.hpp"

#include <lux/engine/function/render/client/RenderFrameSession.hpp>
#include <lux/engine/function/render/client/RenderControlSession.hpp>

#include <algorithm>
#include <vector>

namespace lux::ecs
{
    void SceneRenderBinding::requestSceneOriginRebase(
        const lux::math::Position3d& position) noexcept
    {
        if (const auto tile = renderTileOf(position))
            pending_scene_origin_tile_ = *tile;
    }

    void SceneRenderBinding::requestSceneOriginRebase(
        const lux::math::Position2d& position) noexcept
    {
        if (const auto tile = renderTileOf(position))
        {
            pending_scene_origin_tile_ = lux::math::GridCoord3i64{
                tile->x,
                tile->y,
                scene_origin_tile_3d_.z};
        }
    }

    bool SceneRenderBinding::applyPendingSceneOriginRebase() noexcept
    {
        if (!pending_scene_origin_tile_)
            return true;
        const std::int64_t page[3]{
            pending_scene_origin_tile_->x,
            pending_scene_origin_tile_->y,
            pending_scene_origin_tile_->z};
        if (!session_.get().rebaseSceneOrigin(scene_, page))
            return false;
        scene_origin_tile_3d_ = *pending_scene_origin_tile_;
        pending_scene_origin_tile_.reset();
        return true;
    }

    FeatureSettleReport settleSceneFeatures(
        SceneRenderBinding&                     ctx,
        std::span<const lux::render::FeatureAttach> plan,
        std::span<const std::string_view>           input_roots,
        std::string_view                            profile)
    {
        FeatureSettleReport rep;
        const auto* catalog = ctx.catalog();
        if (!catalog)
            return rep;   // setCatalog 没来过:无目录可解析,零 attach(空视图契约)

        // 根集合由调用方给(节点声明 ∪ 宿主管线选择);这里只去重 ——
        // 「谁需要哪个 feature」是节点的知识,不该由这段编排去搜集。
        std::vector<std::string_view> roots(input_roots.begin(), input_roots.end());
        std::sort(roots.begin(), roots.end());
        roots.erase(std::unique(roots.begin(), roots.end()), roots.end());

        rep.resolve = catalog->resolveAttachOrder(roots);

        // 同名条目里 profile 匹配的胜出;否则用标准条目(profile 空)。
        const auto pickEntry = [&](std::string_view n) -> const lux::render::FeatureAttach*
        {
            const lux::render::FeatureAttach* standard = nullptr;
            for (const auto& f : plan)
            {
                if (f.name != n) continue;
                if (f.profile == profile && !f.profile.empty()) return &f;
                if (f.profile.empty()) standard = &f;
            }
            return standard;
        };

        auto&      session = ctx.control();
        const auto scene   = ctx.scene();

        std::vector<lux::render::RenderRequest<lux::render::FeatureAddedReply>> adds;
        std::vector<std::string_view>                                           names;
        adds.reserve(rep.resolve.order.size());
        names.reserve(rep.resolve.order.size());
        for (const auto n : rep.resolve.order)
        {
            const auto* e = pickEntry(n);
            if (!e)
            {
                // 目录里有 TYPE、attach 计划里却没有配置条目 —— 与「根不在目录」
                // 同一类装配缺口,记进同一本账(上报归宿主)。
                rep.resolve.unknown.push_back(n);
                continue;
            }
            adds.push_back(session.addFeatureRaw(scene, e->type_id, e->config));
            names.push_back(n);
        }

        // Control requests publish independently of the lexical frame. Wait
        // only for their replies; frame admission remains untouched.
        const auto all_ready = [&] {
            for (auto& r : adds) if (!r.isReady()) return false;
            return true;
        };
        if (!session.awaitAllReady(all_ready))
        {
            rep.status = FeatureSettleReport::Status::CHANNEL_STOPPED;
            return rep;   // 收场权归调用方:通道死**不回滚**(回收命令等不到回复)
        }

        for (std::size_t i = 0; i < adds.size(); ++i)
        {
            if (adds[i].failed())
            {
                // 分发期失败(协议层):此前会被误报成「特性被拒」,且 error 为空。
                rep.status   = FeatureSettleReport::Status::DISPATCH_FAILED;
                rep.rejected = names[i];
                rep.error    = adds[i].error();
                return rep;   // 收场权归调用方(与 AttachRejected 同款)
            }
            const auto r = adds[i].tryResult()->get();
            if (!r.feature.isValid())
            {
                rep.status   = FeatureSettleReport::Status::ATTACH_REJECTED;
                rep.rejected = names[i];
                rep.error    = r.error;
                return rep;   // 收场权归调用方:failBringUp 整体回收(原语义)
            }
            ctx.bindFeature(names[i], r.feature);
        }

        return rep;
    }

} // namespace lux::ecs
