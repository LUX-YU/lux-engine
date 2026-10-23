#include <lux/engine/scene/RenderSystemConfiguration.hpp>
#include <lux/engine/serialization/Serialization.hpp>

#include <cmath>
#include <tuple>

namespace lux::serialization
{
    template <> struct TSerializer<scene::RenderSystemConfiguration>
    {
        static constexpr EWireExtent wire_extent = EWireExtent::VARIABLE;

        template <class Writer>
        static SerializationResult write(
            Writer& writer,
            const scene::RenderSystemConfiguration& value,
            const SerializationContext& context
        ) noexcept
        {
            return serialization::write(
                writer,
                std::tie(
                    value.recipe,
                    value.recipe_version,
                    value.shadows,
                    value.shadow_resolution,
                    value.shadow_filter_radius,
                    value.shadow_distance,
                    value.cluster_index_capacity
                ),
                context
            );
        }

        template <class Reader>
        static SerializationResult read(
            Reader& reader,
            scene::RenderSystemConfiguration& value,
            const SerializationContext& context
        ) noexcept
        {
            scene::RenderSystemConfiguration candidate;
            auto fields = std::tie(
                candidate.recipe,
                candidate.recipe_version,
                candidate.shadows,
                candidate.shadow_resolution,
                candidate.shadow_filter_radius,
                candidate.shadow_distance,
                candidate.cluster_index_capacity
            );
            auto result = serialization::read(reader, fields, context);
            if (result)
            {
                value = std::move(candidate);
            }
            return result;
        }
    };
} // namespace lux::serialization

namespace lux::scene
{
    bool validRenderConfiguration(const RenderSystemConfiguration& value) noexcept
    {
        const bool has_known_recipe = value.recipe == "lux.scene.deferred" || value.recipe == "lux.scene.clustered";
        const bool has_valid_shadow = value.shadow_filter_radius <= 4 && value.shadow_resolution > 2 * (value.shadow_filter_radius + 2) &&
                                      std::isfinite(value.shadow_distance) && value.shadow_distance > 0;
        return has_known_recipe && value.recipe_version == 1 && has_valid_shadow && value.cluster_index_capacity != 0;
    }

    serialization::PortableValueCodec renderConfigurationCodec() noexcept
    {
        return serialization::makePortableValueCodec<RenderSystemConfiguration>();
    }
} // namespace lux::scene
