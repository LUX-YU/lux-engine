#pragma once
// ============================================================================
//  ViewCameraResource.hpp — per-scene store of per-view 3D camera state.
//
//  Owned by StandardViewCameraFeature (the LightFeature / SpatialCullFeature
//  resource-ownership pattern). Holds the per-view camera data (view/proj
//  matrices + derived inverses, world camera position, frustum, viewport) that
//  the core View used to carry inline. Consumers — Forward/Deferred cull
//  (GpuDrivenMeshFeatureBase), MeshShadow, Hzb, PointCloud, SpatialCull,
//  DeferredLighting — read it via sceneRegistry().find<ViewCameraResource>().
//
//  A 2D / headless / compute-only scene that omits StandardViewCamera carries
//  none of this — the core View stays domain-neutral (extent + GPU slot only).
// ============================================================================

#include <lux/engine/render/core/ViewFrameData.hpp> // ViewFrameData (CameraView, Frustum, ...)
#include <lux/cxx/container/BasicSparseSet.hpp>
#include <lux/engine/render/gpu/lifecycle/ResourceRegistry.hpp> // resolveViewCameraOnce

#include <cstdint>

namespace lux::render
{
    class ViewCameraResource
    {
    public:
        /// Upsert a view's camera state (called by the StandardViewCamera op handler).
        void setView(uint32_t view_id, const ViewFrameData& data)
        {
            views_.insert(view_id, data);
        }

        /// Drop a view's camera state (view removed / scene teardown).
        void removeView(uint32_t view_id) noexcept
        {
            (void)views_.erase(view_id);
        }

        /// Per-view camera state, or nullptr if this view has none yet.
        [[nodiscard]] const ViewFrameData* find(uint32_t view_id) const noexcept
        {
            return views_.tryGet(view_id);
        }

        [[nodiscard]] bool empty() const noexcept
        {
            return views_.empty();
        }

        [[nodiscard]] bool canRebaseSceneOrigin(const std::int64_t origin_delta[3]) const noexcept
        {
            for (const auto& view : views_.values())
            {
                if (!canRebaseRenderPageDelta(view.render_origin.page_delta, origin_delta))
                {
                    return false;
                }
            }
            return true;
        }

        void rebaseSceneOrigin(const std::int64_t origin_delta[3]) noexcept
        {
            for (const auto key : views_.keys())
            {
                auto* view = views_.tryGet(key);
                rebaseRenderPageDelta(view->render_origin.page_delta, origin_delta);
            }
        }

    private:
        lux::cxx::BasicSparseSet<uint32_t, ViewFrameData> views_;
    };

    /// Projection for positions that have already been made relative to the
    /// per-view RenderOrigin.  ViewFrameData also keeps a scene-origin-relative
    /// translated view for CPU consumers; applying that matrix here would
    /// subtract the camera translation twice.
    [[nodiscard]] inline Eigen::Matrix4f viewRelativeViewProjection(const ViewFrameData& view) noexcept
    {
        Eigen::Matrix4f rotation_view = view.camera_view.view;
        rotation_view.block<3, 1>(0, 3).setZero();
        return view.camera_view.proj * rotation_view;
    }

    /// 记忆化解析:查一次,之后读缓存。给每个消费方一个 `ViewCameraResource* cache`
    /// 成员,把每帧的 find<> 收敛掉。
    ///
    /// 为什么保留判空而不做成硬依赖:相机资源是**真可选**的 —— 2D / headless /
    /// compute-only 场景合法地不装 StandardViewCamera(见本文件头),所以空是合法
    /// 运行态,`requires=` 会把这些场景的安装打回。
    ///
    /// 为什么记忆化而不是 attach 期缓存:可选依赖不强制安装顺序,
    /// StandardViewCameraFeature 理论上可以后装,attach 期缓存会捕获一个永久的空。
    /// 记忆化对顺序免疫,代价只是资源确实缺失时每帧一次哈希查找 —— 那正是 2D
    /// 场景的常态,而那条路径本就什么都不做。
    ///
    /// 缓存一旦命中即永久有效:注册表没有 erase,type_map_ 永远保留首个实例;
    /// 消费方(feature)在 sceneRegistry 关停之前就已 detach 销毁。
    [[nodiscard]] inline ViewCameraResource*
    resolveViewCameraOnce(ViewCameraResource*& cache, ResourceRegistry& reg) noexcept
    {
        if (cache == nullptr)
            cache = reg.find<ViewCameraResource>();
        return cache;
    }

} // namespace lux::render
