// 头在 include/lux/engine/render/ 根(公开 SDK 面:外部特性作者要继承它),
// 实现在 src/render/scene/(它属于场景关注点:通篇是向 RenderScene 的转发)。
//
// **头文件层级表达"交给谁",src/ 目录表达"属于哪个关注点",两者不必一致。**
// 这不是遗漏 —— 让实现跟着被转发的主体走,__top__ 才不会为了实现的 include
// 而反向依赖 scene(那正是 L4 内部那个强连通分量的一半)。
#include <lux/engine/render/SceneFeature.hpp>
#include <lux/engine/render/scene/RenderScene.hpp>

#include <cassert>

namespace lux::render
{
    RenderScene &SceneFeature::renderScene() noexcept
    {
        assert(scene_ && "SceneFeature::renderScene() called before attach");
        return *scene_;
    }

    const RenderScene &SceneFeature::renderScene() const noexcept
    {
        assert(scene_ && "SceneFeature::renderScene() called before attach");
        return *scene_;
    }

    RenderContext &SceneFeature::renderContext() noexcept
    {
        return renderScene().renderContext();
    }

    const RenderContext &SceneFeature::renderContext() const noexcept
    {
        return renderScene().renderContext();
    }

    // Narrow views — lightweight (one pointer), constructed on demand over the
    // concrete subject. Returned by value, like a std::*_view.
    RenderContextView SceneFeature::contextView() noexcept
    {
        return RenderContextView{renderContext()};
    }

    RenderSceneView SceneFeature::sceneView() noexcept
    {
        return RenderSceneView{renderScene()};
    }

} // namespace lux::render
