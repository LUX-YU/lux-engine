#include <lux/engine/scene/SceneRenderInputs.hpp>

namespace lux::scene
{
    SceneViewId SceneRenderInputs::addView(SceneViewRequest request) noexcept
    {
        auto id = views_.emplace(SceneViewInput{{}, std::move(request), ++revision_});
        views_[id].id = id;
        return id;
    }

    bool SceneRenderInputs::updateView(SceneViewId id, SceneViewRequest request) noexcept
    {
        auto* view = views_.find(id);
        if (!view)
        {
            return false;
        }
        view->request = std::move(request);
        view->revision = ++revision_;
        return true;
    }

    bool SceneRenderInputs::removeView(SceneViewId id) noexcept
    {
        if (!views_.erase(id))
        {
            return false;
        }
        ++revision_;
        return true;
    }

    const SceneViewInput* SceneRenderInputs::findView(SceneViewId id) const noexcept
    {
        return views_.find(id);
    }

    std::span<const SceneViewInput> SceneRenderInputs::views() const noexcept
    {
        return views_.values();
    }

    std::uint64_t SceneRenderInputs::revision() const noexcept
    {
        return revision_;
    }
} // namespace lux::scene
