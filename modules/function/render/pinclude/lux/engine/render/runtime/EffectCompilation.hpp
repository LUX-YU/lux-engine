#pragma once

#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/features/FeatureDefinition.hpp>
#include <lux/engine/render/runtime/Scene.hpp>

namespace lux::render::runtime_detail
{
    struct FeatureEntry final
    {
        // Stable table construction binds views only after their owned metadata exists.
        FeatureId id;
        std::string name;
        FeatureDescriptor descriptor;
        FeatureGraphOps graph;
        decltype(FeatureDefinition::programs) programs;

        FeatureEntry(FeatureDefinition input) noexcept
            : name(input.descriptor->type.name()), descriptor(*input.descriptor), graph(*input.graph),
              programs(input.programs)
        {
            descriptor.type = FeatureName(name);
        }
    };

    RenderResult<RenderEffect> compileEffect(
        nri::Device& device,
        const nri::CoreInterface& nri_core,
        const detail::EnabledDeviceFeatures& enabled,
        std::span<const nri::FormatSupportBits> formats,
        std::array<std::uint32_t, 2> vertex_limits,
        nri::QueueType queue_type,
        std::uint32_t sampled_image_capacity,
        std::uint32_t sampler_capacity,
        std::span<const StorageNameView> storage_types,
        const CreateEffectRequest& request,
        std::span<const FeatureEntry* const> registrations,
        std::size_t max_config_bytes
    ) noexcept;
} // namespace lux::render::runtime_detail
