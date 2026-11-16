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

    // Graph is retained only until capability frame bindings are resolved before publication.
    RenderResult<std::pair<RenderEffect, RenderGraph>> compileEffect(
        nri::Device& device,
        const nri::CoreInterface& nri_core,
        const detail::EnabledDeviceFeatures& enabled,
        std::array<const detail::QueueProperties*, 3> queues,
        std::span<const ResourceDeclaration> declarations,
        const GraphEffectRequest& request,
        std::span<const FeatureEntry* const> registrations,
        std::size_t max_config_bytes,
        std::vector<RenderEffect::ResourceDependency> resource_dependencies
    ) noexcept;
} // namespace lux::render::runtime_detail
