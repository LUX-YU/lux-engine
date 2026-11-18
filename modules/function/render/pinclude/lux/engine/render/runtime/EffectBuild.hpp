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

    // Build owns the immutable logical result. No device, native queues, pipelines or scene instances enter here.
    RenderResult<RenderEffect> buildEffect(
        std::span<const ResourceDeclaration> declarations,
        const CreateEffectRequest& request,
        std::span<const FeatureEntry* const> registrations,
        std::size_t max_config_bytes,
        std::vector<RenderEffect::ResourceDependency> resource_dependencies
    ) noexcept;
} // namespace lux::render::runtime_detail
