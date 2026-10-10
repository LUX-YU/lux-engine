#include <lux/engine/scene/RenderSystemConfiguration.hpp>

namespace lux::scene
{
    namespace
    {
        using namespace serialization;
        constexpr std::size_t PayloadLimit = 64U * 1024U * 1024U + 16U;
        constexpr std::size_t ConfigurationLimit = 65U * 1024U * 1024U;

        SerializationResult fail(ESerializationError error, std::size_t offset = 0) noexcept
        {
            return lux::cxx::unexpected(SerializationFailure{error, offset});
        }

        SerializationResult encode(const void* object, std::vector<std::byte>& output) noexcept
        {
            if (!object)
                return fail(ESerializationError::INVALID_VALUE);
            const auto& value = *static_cast<const RenderSystemConfiguration*>(object);
            if (value.features.size() > kPortableMetadataBudget.max_container_elements)
                return fail(ESerializationError::LIMIT_EXCEEDED);
            std::size_t bytes = sizeof(double) + sizeof(std::uint64_t);
            std::size_t payload{};
            for (const auto& feature : value.features)
            {
                const bool is_payload_excessive = feature.configuration.size() > PayloadLimit - payload;
                const bool is_schema_excessive =
                    feature.configuration_schema.size() > kPortableMetadataBudget.max_string_bytes;
                if (is_payload_excessive || is_schema_excessive)
                    return fail(ESerializationError::LIMIT_EXCEEDED);
                payload += feature.configuration.size();
                const auto added = 3U * sizeof(std::uint64_t) + sizeof(std::uint32_t) + feature.configuration.size() +
                                   feature.configuration_schema.size();
                if (added > ConfigurationLimit - bytes)
                    return fail(ESerializationError::LIMIT_EXCEEDED);
                bytes += added;
            }
            std::vector<std::byte> encoded;
            encoded.reserve(bytes);
            BinaryWriter writer(encoded);
            static_cast<void>(writer.writeFloat(value.coordinate_page_size));
            static_cast<void>(writer.writeUnsigned<std::uint64_t>(value.features.size()));
            for (const auto& feature : value.features)
            {
                static_cast<void>(writer.writeUnsigned(feature.type));
                static_cast<void>(writer.writeUnsigned<std::uint64_t>(feature.configuration.size()));
                static_cast<void>(writer.writeBytes(feature.configuration));
                static_cast<void>(writer.writeUnsigned<std::uint64_t>(feature.configuration_schema.size()));
                static_cast<void>(writer.writeBytes(std::as_bytes(std::span(feature.configuration_schema))));
                static_cast<void>(writer.writeUnsigned(feature.configuration_version));
            }
            output = std::move(encoded);
            return {};
        }

        SerializationResult decode(std::span<const std::byte> input, void* object) noexcept
        {
            if (!object)
                return fail(ESerializationError::INVALID_VALUE);
            if (input.size() > ConfigurationLimit)
                return fail(ESerializationError::LIMIT_EXCEEDED);
            BinaryReader reader(input);
            auto page = reader.readFloat<double>();
            if (!page)
                return lux::cxx::unexpected(page.error());
            auto count = reader.readUnsigned<std::uint64_t>();
            if (!count)
                return lux::cxx::unexpected(count.error());
            constexpr auto MinimumFeatureSize = 3U * sizeof(std::uint64_t) + sizeof(std::uint32_t);
            const bool is_count_excessive = *count > kPortableMetadataBudget.max_container_elements;
            const bool is_truncated = *count > reader.remaining() / MinimumFeatureSize;
            if (is_count_excessive || is_truncated)
                return fail(
                    is_count_excessive ? ESerializationError::LIMIT_EXCEEDED : ESerializationError::TRUNCATED,
                    reader.offset()
                );
            RenderSystemConfiguration prepared;
            prepared.coordinate_page_size = *page;
            prepared.features.reserve(static_cast<std::size_t>(*count));
            std::size_t payload{};
            for (std::uint64_t index{}; index != *count; ++index)
            {
                auto type = reader.readUnsigned<render::FeatureTypeId>();
                if (!type)
                    return lux::cxx::unexpected(type.error());
                auto length = reader.readUnsigned<std::uint64_t>();
                if (!length)
                    return lux::cxx::unexpected(length.error());
                if (*length > PayloadLimit - payload)
                    return fail(ESerializationError::LIMIT_EXCEEDED, reader.offset());
                if (*length > reader.remaining())
                    return fail(ESerializationError::TRUNCATED, reader.offset());
                auto& feature = prepared.features.emplace_back();
                feature.type = *type;
                feature.configuration.resize(static_cast<std::size_t>(*length));
                payload += feature.configuration.size();
                static_cast<void>(reader.readBytes(feature.configuration));
                auto schema_size = reader.readUnsigned<std::uint64_t>();
                if (!schema_size)
                    return lux::cxx::unexpected(schema_size.error());
                if (*schema_size > kPortableMetadataBudget.max_string_bytes)
                    return fail(ESerializationError::LIMIT_EXCEEDED, reader.offset());
                if (*schema_size > reader.remaining())
                    return fail(ESerializationError::TRUNCATED, reader.offset());
                feature.configuration_schema.resize(static_cast<std::size_t>(*schema_size));
                static_cast<void>(reader.readBytes(std::as_writable_bytes(std::span(feature.configuration_schema))));
                auto version = reader.readUnsigned<std::uint32_t>();
                if (!version)
                    return lux::cxx::unexpected(version.error());
                feature.configuration_version = *version;
            }
            if (reader.remaining())
                return fail(ESerializationError::INVALID_VALUE, reader.offset());
            *static_cast<RenderSystemConfiguration*>(object) = std::move(prepared);
            return {};
        }
    }

    serialization::PortableValueCodec renderSystemConfigurationCodec() noexcept
    {
        return {
            lux::cxx::typeToken<RenderSystemConfiguration>(),
            +[](std::vector<std::byte>& output) noexcept {
                const RenderSystemConfiguration value;
                return encode(&value, output);
            },
            &encode,
            &decode
        };
    }
}
