#include <lux/engine/simulation/ecs/VisualSchema.hpp>

#include <lux/engine/serialization/Serialization.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>

#include <array>

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
