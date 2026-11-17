#pragma once

#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/extensions/features/EffectDefinition.hpp>
#include <lux/engine/render/extensions/detail/FeatureEntry.hpp>
#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>
#include <lux/engine/render/runtime/Scene.hpp>
#include <utility>

namespace lux::render
{
    // Private result of capability lowering; never a Client command or an independent creation entry.
    struct GraphEffectRequest final
    {
        EffectOutputDesc output;
        EffectDefinition definition;
    };
}

namespace lux::render::runtime_detail
{
    // Frozen cold lookup input, copied from the sole ResourceRegistry before CPU compilation.
    struct ResourceDeclaration final
    {
        ResourceTypeId id;
        ResourceNameView name;
        EResourceDomain domain;
    };

    // Build owns the immutable logical result. No device, native queues, pipelines or scene instances enter here.
    RenderResult<RenderEffect> buildEffect(
        std::span<const ResourceDeclaration> declarations,
        const GraphEffectRequest& request,
        std::span<const FeatureEntry* const> registrations,
        std::size_t max_config_bytes,
        std::vector<RenderEffect::ResourceDependency> resource_dependencies
    ) noexcept;
} // namespace lux::render::runtime_detail
