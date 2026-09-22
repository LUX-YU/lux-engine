#include <lux/engine/scene/Observer.hpp>
#include <lux/engine/scene/WorldLoadingSystem.hpp>
#include <lux/engine/serialization/Serialization.hpp>

namespace lux::serialization
{
// Ordinals are relative to the containing WorldDescription. They are not Entity
// or persistent object identities and require no runtime identity resolver.
template <> struct Serializer<partition::PartitionOrdinal>
{
    static constexpr EWireExtent wire_extent = EWireExtent::FIXED;
    static constexpr std::size_t wire_size = sizeof(std::uint32_t);

    template <class Writer>
    static SerializationResult write(Writer &writer, partition::PartitionOrdinal value,
                                     const SerializationContext &) noexcept
    {
        return writer.writeUnsigned(value.value);
    }

    template <class Reader>
    static SerializationResult read(Reader &reader, partition::PartitionOrdinal &value,
                                    const SerializationContext &) noexcept
    {
        auto decoded = reader.template readUnsigned<std::uint32_t>();
        if (!decoded)
        {
            return lux::cxx::unexpected(decoded.error());
        }
        value.value = *decoded;
        return {};
    }
};
template <> struct Serializer<scene::Observer>
{
    static constexpr EWireExtent wire_extent = EWireExtent::VARIABLE;

    template <class Writer>
    static SerializationResult write(Writer &writer, const scene::Observer &value,
                                     const SerializationContext &context) noexcept
    {
        return serialization::write(writer, std::tie(value.partitions, value.required), context);
    }

    template <class Reader>
    static SerializationResult read(Reader &reader, scene::Observer &value,
                                    const SerializationContext &context) noexcept
    {
        scene::Observer decoded;
        auto fields = std::tie(decoded.partitions, decoded.required);
        auto result = serialization::read(reader, fields, context);
        if (result)
        {
            value = std::move(decoded);
        }
        return result;
    }
};

template <> struct Serializer<scene::WorldLoadingConfiguration>
{
    static constexpr EWireExtent wire_extent = EWireExtent::VARIABLE;

    template <class Writer>
    static SerializationResult write(Writer &writer, const scene::WorldLoadingConfiguration &value,
                                     const SerializationContext &context) noexcept
    {
        return serialization::write(writer, value.bootstrap, context);
    }

    template <class Reader>
    static SerializationResult read(Reader &reader, scene::WorldLoadingConfiguration &value,
                                    const SerializationContext &context) noexcept
    {
        scene::WorldLoadingConfiguration decoded;
        auto result = serialization::read(reader, decoded.bootstrap, context);
        if (result)
        {
            value = std::move(decoded);
        }
        return result;
    }
};
} // namespace lux::serialization

#include <lux/engine/scene/Observer.ecs_schema.hpp>

namespace lux::scene
{
serialization::PortableValueCodec worldLoadingConfigurationCodec() noexcept
{
    return serialization::makePortableValueCodec<WorldLoadingConfiguration>();
}

std::span<const simulation::ecs::ComponentSchema> worldLoadingComponentSchemas() noexcept
{
    return simulation::ecs::generated::sceneObserverComponentSchemas();
}
} // namespace lux::scene
