#pragma once
// ============================================================================
//  RenderFeature.hpp — 产 render-graph pass 的场景单元。
//
//  装载相关的一切(生命周期、每视图状态、每帧回调、参数、身份)都在基类
//  SceneFeature 里;本头只加一件事:**产 pass**。
//
//  两者的划分只有这一条判据。此前只有 RenderFeature 一个类,它唯一的纯虚函数
//  就是 addPasses —— 于是 5 个只拥有资源的东西被迫写空实现,违反 is-a。
//  资源拥有者现在直接继承 SceneUnit。
// ============================================================================

#include <lux/engine/render/SceneFeature.hpp>

namespace lux::render
{
    class RGBuilder;

    // =========================================================================
    // RenderFeature — 描述一种渲染方式的场景单元
    // =========================================================================

    /**
     * @brief 一种自足的渲染能力,可在运行期注册进 RenderScene。
     *
     * 每个 RenderFeature 负责:
     *   1. 加载自己的着色器、注册管线模板
     *   2. 添加自己的 render-graph pass
     *   3. 创建并注册自己的 PassProcessor
     *   4. (可选)初始化特性专属的 GPU 资源
     *
     * 内置特性(DepthPrepass、ForwardMesh、Grid、PointCloud …)用的就是这个
     * 接口,所以用户自定义特性拥有完全相同的能力。
     */
    class LUX_FUNCTION_PUBLIC RenderFeature : public SceneFeature
    {
    public:
        using SceneFeature::SceneFeature;

        /**
         * @brief Add render-graph passes to the builder.
         *
         * Called once during RenderGraphBuildService::build().  The feature
         * declares its reads/writes via builder.referenceTexture()/referenceBuffer()
         * and any transients it creates.
         */
        virtual void addPasses(RGBuilder& builder) = 0;

    protected:
        RenderFeature() = default;
    };

} // namespace lux::render
