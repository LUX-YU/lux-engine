#pragma once
/**
 * @file RegisterComponent.hpp
 * @brief Out-of-line definition of `RenderableSystem::registerComponent<C>()`.
 *
 * `RenderableSystem.hpp` only DECLARES the member template (keeping that header
 * free of the render-coupled bridge machinery). Translation units that actually
 * register components (the editor's bringUp; later a kit's install()) include THIS
 * header, which pulls the bridge templates and compiles the instantiation. Dispatch
 * is compile-time on `EcsRenderTraits<C>::kind` — no runtime branch, no IR.
 */

#include <memory>

#include "lux/engine/render_bridge/RenderableSystem.hpp"
#include "lux/engine/render_bridge/EcsRenderTraits.hpp"
#include "lux/engine/render_bridge/ParamBridge.hpp"
#include "lux/engine/render_bridge/PoolBridge.hpp"
#include "lux/engine/render_bridge/InstanceBridge.hpp"

namespace lux::render_bridge
{
    template <class C>
    void RenderableSystem::registerComponent()
    {
        using K = EcsRenderTraits<C>;
        if constexpr (K::kind == ERenderableKind::PARAM)
            addBridge(std::make_unique<ParamBridge<C>>());
        else if constexpr (K::kind == ERenderableKind::POOL)
            addBridge(std::make_unique<PoolBridge<C>>());
        else  // ERenderableKind::INSTANCE
            addBridge(std::make_unique<InstanceBridge<C>>());
    }

} // namespace lux::render_bridge
