#include <lux/engine/render/renderer/RenderTargetRegistry.hpp>

#include <lux/engine/render/gpu/VulkanContext.hpp> // ResourceContext
#include <lux/engine/render/targets/SwapchainProvider.hpp>

#include <algorithm>

namespace lux::render
{
    PresentContext* RenderTargetRegistry::surfacePresent() noexcept
    {
        auto* st = surfaceTarget();
        return st ? st->present.get() : nullptr;
    }

    SwapchainProvider* RenderTargetRegistry::swapchainProvider() noexcept
    {
        auto* pc = surfacePresent();
        return pc ? pc->provider() : nullptr;
    }

    RenderTargetId RenderTargetRegistry::findOffscreenKeyByView(RenderSceneId s, ViewHandle v) const noexcept
    {
        const auto& keys = targets_.keys();
        const auto& vals = targets_.values();
        for (size_t i = 0; i < vals.size(); ++i)
        {
            if (vals[i].kind == Entry::EKind::OFFSCREEN)
            {
                for (const auto& l : vals[i].layers)
                {
                    if (l.scene_id == s && l.view_id == v)
                    {
                        return keys[i];
                    }
                }
            }
        }
        return {};
    }

    RenderTargetEntry* RenderTargetRegistry::findOffscreenByView(RenderSceneId s, ViewHandle v) noexcept
    {
        const auto key = findOffscreenKeyByView(s, v);
        return key.isValid() ? targets_.tryGet(key) : nullptr;
    }

    bool RenderTargetRegistry::detachLayerAndReapIfEmpty(
        RenderTargetId key,
        RenderSceneId s,
        ViewHandle v,
        uint64_t retire_serial
    )
    {
        auto* t = targets_.tryGet(key);
        if (!t)
        {
            return false;
        }

        bool removed = false;
        for (size_t i = t->layers.size(); i-- > 0;)
        {
            if (t->layers[i].scene_id == s && t->layers[i].view_id == v)
            {
                t->layers.erase(t->layers.begin() + i);
                removed = true;
            }
        }

        if (removed && t->layers.empty() && t->kind == Entry::EKind::OFFSCREEN)
        {
            retireTargetPool(*t, retire_serial);
            targets_.erase(key);
        }
        return removed;
    }

    Expected<std::unique_ptr<OffscreenImagePool>> RenderTargetRegistry::makeTargetPool(
        const RenderTargetLayout& layout,
        VkExtent2D extent
    ) noexcept
    {
        return OffscreenImagePool::create(*res_ctx_, layout, extent, frames_in_flight_);
    }

    void RenderTargetRegistry::retireTargetPool(Entry& t, uint64_t retire_serial)
    {
        if (!t.pool)
        {
            return;
        }
        deferred_pools_.emplace_back(retire_serial, std::move(t.pool));
    }

    void RenderTargetRegistry::collectRetiredPools(uint64_t gpu_completed)
    {
        std::erase_if(deferred_pools_, [&](const auto& e) { return e.first <= gpu_completed; });
    }

    void RenderTargetRegistry::shutdown()
    {
        // 设备空闲时调用(关服路径)——顺序:先放延迟池,再拆 target 本体
        // (Surface 的 PresentContext 析构会逆序拆 sems → swapchain → surface)。
        deferred_pools_.clear();
        targets_.clear();
        surface_target_ = {};
    }

} // namespace lux::render
