#pragma once

#include <lux/engine/render/commands/Commands.hpp>
#include <lux/engine/render/features/FeatureAssembly.hpp>
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

        FeatureEntry(const FeatureDescriptor& declaration, const FeatureGraphOps& contribution) noexcept
            : name(declaration.type.name()), descriptor(declaration), graph(contribution)
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
        std::array<const detail::QueueProperties*, 3> queues,
        std::uint32_t sampled_image_capacity,
        std::uint32_t sampler_capacity,
        std::span<const StorageNameView> storage_types,
        const CreateEffectRequest& request,
        std::span<const FeatureEntry* const> registrations,
        std::size_t max_config_bytes
    ) noexcept;
} // namespace lux::render::runtime_detail
