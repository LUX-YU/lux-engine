#pragma once

#include <lux/engine/scene/render/RenderBindingDeclarations.hpp>

namespace lux::scene::detail
{
    // Owns one selected behavior and its EnTT connections. Scope/spawn owns its asynchronous
    // operations; no connected operation is stored here. Connections die before the behavior.
    struct RenderBindingInstance final
    {
        RenderBindingDeclaration declaration;
        std::unique_ptr<void, void (*)(void*)> object;
        std::vector<entt::scoped_connection> connections;

        RenderBindingInstance(RenderBindingDeclaration description, void* instance) noexcept
            : declaration(std::move(description)), object(instance, declaration.destroy)
        {
        }

        RenderBindingInstance(RenderBindingInstance&&) noexcept = default;
        RenderBindingInstance& operator=(RenderBindingInstance&&) = delete;
        RenderBindingInstance(const RenderBindingInstance&) = delete;
    };

} // namespace lux::scene::detail
