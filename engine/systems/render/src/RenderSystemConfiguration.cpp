#include <lux/engine/systems/render/RenderSystemConfiguration.hpp>

namespace lux::serialization
{
    template <> struct TSerializer<lux::engine::render::RenderSystemConfiguration>
    {
        static constexpr EWireExtent wire_extent = EWireExtent::VARIABLE;

        template <class Writer>
        static SerializationResult write(
            Writer& writer,
            const lux::engine::render::RenderSystemConfiguration& value,
            const SerializationContext& context
        ) noexcept
        {
            auto bytes = lux::render::encodeCapabilityComposition(value.composition);
            if (!bytes)
            {
                return cxx::unexpected(SerializationFailure{ESerializationError::INVALID_VALUE});
            }
            auto written = serialization::write(writer, *bytes, context);
            if (!written)
            {
                return written;
            }
            auto fields = std::tie(
                value.output.format,
                value.output.samples,
                value.output.kind,
                value.output.sampled
            );
            return serialization::write(writer, fields, context);
        }

        template <class Reader>
        static SerializationResult read(
            Reader& reader,
            lux::engine::render::RenderSystemConfiguration& value,
            const SerializationContext& context
        ) noexcept
        {
            std::vector<std::byte> bytes;
            auto loaded = serialization::read(reader, bytes, context);
            if (!loaded)
            {
                return loaded;
            }
            auto decoded = lux::render::decodeCapabilityComposition(bytes);
            if (!decoded)
            {
                return cxx::unexpected(SerializationFailure{ESerializationError::INVALID_VALUE});
            }
            value.composition = std::move(*decoded);
            auto fields = std::tie(
                value.output.format,
                value.output.samples,
                value.output.kind,
                value.output.sampled
            );
            auto loaded_output = serialization::read(reader, fields, context);
            if (!loaded_output || !lux::render::validateEffectOutput(value.output))
            {
                return cxx::unexpected(SerializationFailure{ESerializationError::INVALID_VALUE});
            }
            return {};
        }
    };
} // namespace lux::serialization

namespace lux::engine::render
{
    bool validRenderConfiguration(const RenderSystemConfiguration& value) noexcept
    {
        return lux::render::validCapabilityComposition(value.composition) &&
            bool(lux::render::validateEffectOutput(value.output));
    }

    serialization::PortableValueCodec renderConfigurationCodec() noexcept
    {
        return serialization::makePortableValueCodec<RenderSystemConfiguration>();
    }
} // namespace lux::engine::render
