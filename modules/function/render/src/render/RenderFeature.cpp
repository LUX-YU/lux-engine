#include <lux/engine/render/RenderFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

#include <cassert>

namespace lux::render
{
    RenderScene &RenderFeature::renderScene() noexcept
    {
        assert(scene_ && "RenderFeature::renderScene() called before attach");
        return *scene_;
    }

    const RenderScene &RenderFeature::renderScene() const noexcept
    {
        assert(scene_ && "RenderFeature::renderScene() called before attach");
        return *scene_;
    }

    RenderContext &RenderFeature::renderContext() noexcept
    {
        return renderScene().renderContext();
    }

    const RenderContext &RenderFeature::renderContext() const noexcept
    {
        return renderScene().renderContext();
    }

    // Narrow views — lightweight (one pointer), constructed on demand over the
    // concrete subject. Returned by value, like a std::*_view.
    RenderContextView RenderFeature::contextView() noexcept
    {
        return RenderContextView{renderContext()};
    }

    RenderSceneView RenderFeature::sceneView() noexcept
    {
        return RenderSceneView{renderScene()};
    }

} // namespace lux::render
