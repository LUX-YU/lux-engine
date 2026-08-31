#include <lux/engine/simulation/ecs/VisualSchema.hpp>

#include <lux/engine/serialization/Serialization.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>

#include <array>
#include <tuple>

namespace lux::serialization
{
    template <> struct Serializer<lux::asset::AssetId>
    {
        static constexpr EWireExtent wire_extent = EWireExtent::FIXED;
        static constexpr std::size_t fixed_wire_size = 16U;

        template <class Writer>
        [[nodiscard]] static SerializationResult
        write(Writer& writer, const lux::asset::AssetId& value, const SerializationContext&) noexcept
        {
            const auto bytes = value.bytes();
            return writer.writeBytes(std::as_bytes(std::span(bytes)));
        }

        template <class Reader>
        [[nodiscard]] static SerializationResult
        read(Reader& reader, lux::asset::AssetId& value, const SerializationContext&) noexcept
        {
            std::array<std::uint8_t, 16> bytes{};
            auto result = reader.readBytes(std::as_writable_bytes(std::span(bytes)));
            if (result)
            {
                value = lux::asset::AssetId{bytes};
            }
            return result;
        }
    };

    template <> struct Serializer<lux::rdesc::MeshVisualDescription>
    {
        static constexpr EWireExtent wire_extent = EWireExtent::FIXED;
        static constexpr std::size_t fixed_wire_size = 35U;

        template <class Writer>
        [[nodiscard]] static SerializationResult write(
            Writer& writer,
            const lux::rdesc::MeshVisualDescription& value,
            const SerializationContext& context
        ) noexcept
        {
            return lux::serialization::write(
                writer,
                std::tie(value.mesh, value.material, value.visible, value.cast_shadow, value.receive_shadow),
                context
            );
        }

        template <class Reader>
        [[nodiscard]] static SerializationResult read(
            Reader& reader,
            lux::rdesc::MeshVisualDescription& value,
            const SerializationContext& context
        ) noexcept
        {
            auto fields = std::tie(
                value.mesh,
                value.material,
                value.visible,
                value.cast_shadow,
                value.receive_shadow
            );
            return lux::serialization::read(reader, fields, context);
        }
    };

    template <> struct Serializer<lux::rdesc::LightDescription>
    {
        static constexpr EWireExtent wire_extent = EWireExtent::FIXED;
        static constexpr std::size_t fixed_wire_size = 98U;

        template <class Writer>
        [[nodiscard]] static SerializationResult write(
            Writer& writer,
            const lux::rdesc::LightDescription& value,
            const SerializationContext& context
        ) noexcept
        {
            const bool is_invalid_type = value.type > lux::rdesc::ELightType::AREA;
            const bool is_invalid_cascade_count = value.cascade_count > lux::rdesc::kLightCascadeSlots;
            if (is_invalid_type || is_invalid_cascade_count)
            {
                return lux::cxx::unexpected<SerializationFailure>(
                    SerializationFailure{ESerializationError::INVALID_VALUE, writer.offset()}
                );
            }
            return lux::serialization::write(
                writer,
                std::tie(
                    value.type,
                    value.color,
                    value.intensity,
                    value.range,
                    value.attenuation_constant,
                    value.attenuation_linear,
                    value.attenuation_quadratic,
                    value.inner_cone_angle,
                    value.outer_cone_angle,
                    value.area_size,
                    value.cast_shadow,
                    value.shadow_map_size,
                    value.shadow_bias,
                    value.shadow_normal_bias,
                    value.cascade_count,
                    value.cascade_splits
                ),
                context
            );
        }

        template <class Reader>
        [[nodiscard]] static SerializationResult read(
            Reader& reader,
            lux::rdesc::LightDescription& value,
            const SerializationContext& context
        ) noexcept
        {
            auto fields = std::tie(
                value.type,
                value.color,
                value.intensity,
                value.range,
                value.attenuation_constant,
                value.attenuation_linear,
                value.attenuation_quadratic,
                value.inner_cone_angle,
                value.outer_cone_angle,
                value.area_size,
                value.cast_shadow,
                value.shadow_map_size,
                value.shadow_bias,
                value.shadow_normal_bias,
                value.cascade_count,
                value.cascade_splits
            );
            auto result = lux::serialization::read(
                reader,
                fields,
                context
            );
            const bool is_invalid_type = value.type > lux::rdesc::ELightType::AREA;
            const bool is_invalid_cascade_count = value.cascade_count > lux::rdesc::kLightCascadeSlots;
            if (result && (is_invalid_type || is_invalid_cascade_count))
            {
                return lux::cxx::unexpected<SerializationFailure>(
                    SerializationFailure{ESerializationError::INVALID_VALUE, reader.offset()}
                );
            }
            return result;
        }
    };
}

#include <lux/engine/simulation/ecs/Visual.ecs_schema.hpp>
#include <lux/engine/simulation/ecs/Visual.ecs_snapshot.hpp>

namespace lux::simulation::ecs
{
    std::span<const ComponentSchema> visualComponentSchemas() noexcept
    {
        return generated::visualComponentSchemas();
    }

    ComponentSnapshotContribution visualComponentSnapshotContribution() noexcept
    {
        return generated::visualComponentSnapshotContribution();
    }
}
