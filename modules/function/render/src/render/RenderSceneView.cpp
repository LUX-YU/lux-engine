#include <lux/engine/render/RenderSceneView.hpp>

// The subject this facade forwards to. RenderSceneView HOLDS a RenderScene&
// (composition); it is NOT a base of RenderScene — the dependency flows one way:
// feature → facade → RenderScene, never the reverse.
#include <lux/engine/render/scene/RenderScene.hpp>

namespace lux::render
{
    ResourceRegistryBase& RenderSceneView::sceneRegistry() noexcept
    {
        return scene_->sceneRegistry();
    }

    const ResourceRegistryBase& RenderSceneView::sceneRegistry() const noexcept
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

    void RenderSceneView::invalidateGraph() noexcept
    {
        scene_->invalidateGraph();
    }

} // namespace lux::render
