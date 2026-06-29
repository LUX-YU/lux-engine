#pragma once

#include <lux/engine/function/visibility.h>
#include <lux/engine/render/FrameOrchestrator.hpp>
#include <lux/engine/render/FrameStamp.hpp>

#include <cstdint>
#include <vector>

namespace lux::render
{

class RenderScene;
struct View;

struct SceneViewBatchItem
{
    RenderScene* scene{nullptr};
    View*        view{nullptr};
    uint32_t     cross_view_index{0};
};

/// Frame-local scene/view batching helper used by server tick loops.
/// Avoids per-frame hash maps by keeping a compact scene list + counters.
class LUX_FUNCTION_PUBLIC SceneViewBatch
{
public:
    void clear()
    {
        items_.clear();
        scenes_.clear();
        scene_counts_.clear();
        last_scene_ = nullptr;
        last_scene_index_ = 0;
    }

    void add(RenderScene* scene, View* view)
    {
        if (!scene || !view) return;
        const uint32_t cv = nextCrossViewIndex(scene);
        items_.push_back(SceneViewBatchItem{
            .scene = scene,
            .view = view,
            .cross_view_index = cv,
        });
    }

    [[nodiscard]] const std::vector<SceneViewBatchItem>& items() const noexcept
    {
        return items_;
    }

    [[nodiscard]] const std::vector<RenderScene*>& touchedScenes() const noexcept
    {
        return scenes_;
    }

private:
    uint32_t nextCrossViewIndex(RenderScene* scene)
    {
        if (last_scene_ == scene)
        {
            const uint32_t idx = last_scene_index_;
            return scene_counts_[idx]++;
        }

        for (uint32_t idx = 0; idx < scenes_.size(); ++idx)
        {
            if (scenes_[idx] != scene)
                continue;
            last_scene_ = scene;
            last_scene_index_ = idx;
            return scene_counts_[idx]++;
        }

        const uint32_t idx = static_cast<uint32_t>(scenes_.size());
        scenes_.push_back(scene);
        scene_counts_.push_back(1u);
        last_scene_ = scene;
        last_scene_index_ = idx;
        return 0u;
    }

    std::vector<SceneViewBatchItem> items_;
    std::vector<RenderScene*>       scenes_;
    std::vector<uint32_t>           scene_counts_;
    RenderScene*                    last_scene_{nullptr};
    uint32_t                        last_scene_index_{0};
};

/// Provides one canonical "beginTick -> drainRequests" ordering.
class LUX_FUNCTION_PUBLIC RenderTickPipeline
{
public:
    template <typename DrainFn>
    static bool runTick(
        FrameOrchestrator& orchestrator,
        FrameStamp& stamp,
        DrainFn&& drain_requests,
        uint32_t image_index_hint = 0)
    {
        stamp = orchestrator.beginTick(image_index_hint);
        return drain_requests();
    }
};

} // namespace lux::render
