#pragma once

#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/extensions/features/EffectDefinition.hpp>
#include <lux/engine/render/extensions/detail/FeatureEntry.hpp>
#include <lux/engine/render/extensions/features/FeatureAssembly.hpp>
#include <lux/engine/render/runtime/RenderEffect.hpp>
#include <utility>

namespace lux::render::runtime_detail
{
    // Frozen cold lookup input, copied from the sole ResourceRegistry before CPU compilation.
    struct ResourceDeclaration final
    {
        ResourceTypeId id;
        ResourceNameView name;
        EResourceDomain domain;
        std::span<const FrameChannel> channels;
        bool binds_graph = false;
    };

    struct QualifiedEffectRecipe final
    {
        std::vector<const FeatureEntry*> features;
        std::vector<RenderEffect::ResourceDependency> resources;
        std::vector<CommandBindingLocation> commands;
        bool uses_capabilities = false;
    };

    // Build owns the immutable logical result. No device, native queues, pipelines or scene instances enter here.
    RenderResult<RenderEffect> buildEffect(
        std::span<const ResourceDeclaration> declarations,
        const CreateEffectRequest& request,
        QualifiedEffectRecipe recipe,
        std::size_t max_config_bytes
    ) noexcept;
} // namespace lux::render::runtime_detail
