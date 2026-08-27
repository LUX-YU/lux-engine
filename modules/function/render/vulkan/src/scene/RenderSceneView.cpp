// 头在 include/ 根(公开 SDK 面),实现在 src/render/scene/(它属于场景关注点)。
// 理由同 RenderFeature.cpp:头文件层级表达"交给谁",src/ 目录表达"属于哪个关注点"。
#include <lux/engine/render/RenderSceneView.hpp>

// The subject this facade forwards to. RenderSceneView HOLDS a RenderScene&
// (composition); it is NOT a base of RenderScene — the dependency flows one way:
// feature → facade → RenderScene, never the reverse.
#include <lux/engine/render/scene/RenderScene.hpp>

namespace lux::render
{
    ResourceRegistry& RenderSceneView::sceneRegistry() noexcept
    {
        return scene_->sceneRegistry();
    }

    const ResourceRegistry& RenderSceneView::sceneRegistry() const noexcept
    {
        return scene_->sceneRegistry();
    }

    SceneDescriptorArena& RenderSceneView::descriptorArena() noexcept
    {
        return scene_->descriptorArena();
    }

    TransferScheduler& RenderSceneView::transferScheduler() noexcept
    {
        return scene_->transferScheduler();
    }

    FrameRetireScheduler::OwnerToken RenderSceneView::retireOwnerToken() const noexcept
    {
        return scene_->retireOwnerToken();
    }

    void RenderSceneView::invalidateGraph(EGraphInvalidationReason reason) noexcept
    {
        scene_->invalidateGraph(reason);
    }

} // namespace lux::render
