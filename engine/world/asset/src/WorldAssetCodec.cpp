#include <lux/engine/world/WorldAssetCodec.hpp>

#include <lux/engine/serialization/BinaryReader.hpp>
#include <lux/engine/world/WorldDescriptionBuilder.hpp>

#include <algorithm>
#include <array>
#include <iterator>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace lux::world
{
    namespace
    {
        constexpr std::uint32_t kWireVersion = 1U;
        constexpr std::size_t kHeaderBytes = 48U;
        constexpr std::size_t kSchemaFixedBytes = 16U;
        constexpr std::size_t kObjectFixedBytes = 24U;
        constexpr std::size_t kDataFixedBytes = 20U;
        constexpr std::size_t kDecodedSchemaBytes = 64U;
        constexpr std::size_t kDecodedObjectBytes = 64U;
        constexpr std::size_t kDecodedDataBytes = 64U;

        [[nodiscard]] auto codecFailure() noexcept
        {
            return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
        }

        [[nodiscard]] bool toSize(std::uint64_t value, std::size_t& result) noexcept
        {
            if (value > std::numeric_limits<std::size_t>::max())
                return false;
            result = static_cast<std::size_t>(value);
            return true;
        }

        [[nodiscard]] bool addSize(
            std::size_t& total,
            std::size_t value
        ) noexcept
        {
            if (value > std::numeric_limits<std::size_t>::max() - total)
                return false;
            total += value;
            return true;
        }

        [[nodiscard]] bool multiplySize(
            std::size_t count,
            std::size_t stride,
            std::size_t& result
        ) noexcept
        {
            if (count != 0U && stride > std::numeric_limits<std::size_t>::max() / count)
                return false;
            result = count * stride;
            return true;
        }

        class WireWriter final
        {
          public:
            explicit WireWriter(std::vector<std::byte>& bytes) noexcept
                : writer_(bytes)
            {
            }

            void u32(std::uint32_t value) noexcept
            {
                if (ok_)
                    ok_ = static_cast<bool>(writer_.writeUnsigned(value));
            }

            void u64(std::uint64_t value) noexcept
            {
                if (ok_)
                    ok_ = static_cast<bool>(writer_.writeUnsigned(value));
            }

            void bytes(std::span<const std::byte> value) noexcept
            {
                if (ok_)
                    ok_ = static_cast<bool>(writer_.writeBytes(value));
            }

            [[nodiscard]] bool ok() const noexcept
            {
                return ok_;
            }

          private:
            lux::serialization::BinaryWriter writer_;
            bool ok_{true};
        };

        template <class T>
        [[nodiscard]] bool readUnsigned(
            lux::serialization::BinaryReader& reader,
            T& value
        ) noexcept
        {
            auto result = reader.readUnsigned<T>();
            if (!result)
                return false;
            value = *result;
            return true;
        }

        [[nodiscard]] lux::cxx::expected<
            lux::asset::DecodedAsset,
            lux::asset::EAssetCodecError>
        decodeWorld(
            std::span<const std::byte> input,
            const lux::asset::AssetDecodeContext& context
        ) noexcept
        {
            if (input.size() > context.limits.max_input_bytes ||
                input.size() < kHeaderBytes)
            {
                return codecFailure();
            }

            try
            {
                lux::serialization::BinaryReader reader(input);
                std::uint32_t magic{};
                std::uint32_t version{};
                std::uint64_t schema_count_u64{};
                std::uint64_t object_count_u64{};
                std::uint64_t data_count_u64{};
                std::uint64_t schema_name_bytes_u64{};
                std::uint64_t payload_bytes_u64{};
                if (!readUnsigned(reader, magic) ||
                    !readUnsigned(reader, version) ||
                    !readUnsigned(reader, schema_count_u64) ||
                    !readUnsigned(reader, object_count_u64) ||
                    !readUnsigned(reader, data_count_u64) ||
                    !readUnsigned(reader, schema_name_bytes_u64) ||
                    !readUnsigned(reader, payload_bytes_u64) ||
                    magic != WorldAssetPrimaryMagic ||
                    version != kWireVersion)
                {
                    return codecFailure();
                }

                std::size_t schema_count{};
                std::size_t object_count{};
                std::size_t data_count{};
                std::size_t schema_name_bytes{};
                std::size_t payload_bytes{};
                if (!toSize(schema_count_u64, schema_count) ||
                    !toSize(object_count_u64, object_count) ||
                    !toSize(data_count_u64, data_count) ||
                    !toSize(schema_name_bytes_u64, schema_name_bytes) ||
                    !toSize(payload_bytes_u64, payload_bytes))
                {
                    return codecFailure();
                }

                std::size_t decoded_bytes = sizeof(WorldDescription);
                std::size_t term{};
                if (!multiplySize(schema_count, kDecodedSchemaBytes, term) ||
                    !addSize(decoded_bytes, term) ||
                    !multiplySize(object_count, kDecodedObjectBytes, term) ||
                    !addSize(decoded_bytes, term) ||
                    !multiplySize(data_count, kDecodedDataBytes, term) ||
                    !addSize(decoded_bytes, term) ||
                    !addSize(decoded_bytes, schema_name_bytes) ||
                    !addSize(decoded_bytes, payload_bytes) ||
                    decoded_bytes > context.limits.max_decoded_bytes)
                {
                    return codecFailure();
                }

                std::vector<WorldDataSchemaId> schemas;
                schemas.reserve(schema_count);
                std::size_t actual_name_bytes{};
                for (std::size_t index{}; index < schema_count; ++index)
                {
                    std::uint64_t hash{};
                    std::uint64_t name_size_u64{};
                    std::size_t name_size{};
                    if (!readUnsigned(reader, hash) ||
                        !readUnsigned(reader, name_size_u64) ||
                        !toSize(name_size_u64, name_size) ||
                        name_size == 0U ||
                        !addSize(actual_name_bytes, name_size) ||
                        name_size > reader.remaining())
                    {
                        return codecFailure();
                    }
                    std::string name(name_size, '\0');
                    if (!reader.readBytes({
                            reinterpret_cast<std::byte*>(name.data()),
                            name.size()
                        }))
                    {
                        return codecFailure();
                    }
                    WorldDataSchemaId schema{hash, std::move(name)};
                    if (!schema.valid() ||
                        (index != 0U &&
                         !WorldDataSchemaIdLess{}(schemas.back(), schema)))
                    {
                        return codecFailure();
                    }
                    schemas.push_back(std::move(schema));
                }
                if (actual_name_bytes != schema_name_bytes)
                    return codecFailure();

                WorldDescriptionBuilder builder;
                WorldObjectId previous_object{};
                bool have_previous_object{};
                std::size_t actual_data_count{};
                std::size_t actual_payload_bytes{};
                for (std::size_t object_index{};
                     object_index < object_count;
                     ++object_index)
                {
                    std::array<std::uint8_t, 16> object_bytes{};
                    if (!reader.readBytes({
                            reinterpret_cast<std::byte*>(object_bytes.data()),
                            object_bytes.size()
                        }))
                    {
                        return codecFailure();
                    }
                    WorldObjectId object{uuids::uuid(object_bytes)};
                    std::uint64_t object_data_count_u64{};
                    std::size_t object_data_count{};
                    if (!object.valid() ||
                        (have_previous_object &&
                         !WorldObjectIdLess{}(previous_object, object)) ||
                        !readUnsigned(reader, object_data_count_u64) ||
                        !toSize(object_data_count_u64, object_data_count) ||
                        !addSize(actual_data_count, object_data_count) ||
                        actual_data_count > data_count ||
                        !builder.addObject(object))
                    {
                        return codecFailure();
                    }
                    previous_object = object;
                    have_previous_object = true;

                    std::uint64_t previous_schema_ordinal{};
                    bool have_previous_schema{};
                    for (std::size_t data_index{};
                         data_index < object_data_count;
                         ++data_index)
                    {
                        std::uint64_t schema_ordinal{};
                        std::uint32_t data_version{};
                        std::uint64_t data_size_u64{};
                        std::size_t data_size{};
                        if (!readUnsigned(reader, schema_ordinal) ||
                            !readUnsigned(reader, data_version) ||
                            !readUnsigned(reader, data_size_u64) ||
                            schema_ordinal >= schema_count_u64 ||
                            (have_previous_schema &&
                             schema_ordinal <= previous_schema_ordinal) ||
                            data_version == 0U ||
                            !toSize(data_size_u64, data_size) ||
                            !addSize(actual_payload_bytes, data_size) ||
                            actual_payload_bytes > payload_bytes ||
                            data_size > reader.remaining())
                        {
                            return codecFailure();
                        }
                        std::vector<std::byte> payload(data_size);
                        if (!reader.readBytes(payload) ||
                            !builder.addData(
                                object,
                                schemas[static_cast<std::size_t>(schema_ordinal)],
                                data_version,
                                payload
                            ))
                        {
                            return codecFailure();
                        }
                        previous_schema_ordinal = schema_ordinal;
                        have_previous_schema = true;
                    }
                }
                if (actual_data_count != data_count ||
                    actual_payload_bytes != payload_bytes ||
                    reader.remaining() != 0U)
                {
                    return codecFailure();
                }
                auto description = std::move(builder).build();
                if (!description ||
                    description->objectCount() != object_count ||
                    description->dataCount() != data_count)
                {
                    return codecFailure();
                }
                const std::size_t retained_bytes = description->retainedBytes();
                if (retained_bytes > context.limits.max_decoded_bytes)
                    return codecFailure();
                auto payload = std::make_shared<const WorldDescription>(
                    std::move(*description)
                );
                return lux::asset::DecodedAsset{
                    std::move(payload),
                    retained_bytes};
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(
                    lux::asset::EAssetCodecError::OUT_OF_MEMORY
                );
            }
            catch (...)
            {
                return codecFailure();
            }
        }

        [[nodiscard]] lux::cxx::expected<
            std::vector<std::byte>,
            lux::asset::EAssetCodecError>
        encodeWorld(
            const void* payload,
            const lux::asset::AssetEncodeContext& context
        ) noexcept
        {
            if (payload == nullptr)
                return codecFailure();
            const auto& world = *static_cast<const WorldDescription*>(payload);
            try
            {
                std::size_t name_bytes{};
                std::size_t encoded_bytes = kHeaderBytes;
                std::size_t term{};
                for (const auto& schema : world.schemas())
                {
                    if (!schema.valid() ||
                        !addSize(name_bytes, schema.name.size()))
                    {
                        return codecFailure();
                    }
                }
                if (!multiplySize(world.schemas().size(), kSchemaFixedBytes, term) ||
                    !addSize(encoded_bytes, term) ||
                    !addSize(encoded_bytes, name_bytes) ||
                    !multiplySize(world.objectCount(), kObjectFixedBytes, term) ||
                    !addSize(encoded_bytes, term) ||
                    !multiplySize(world.dataCount(), kDataFixedBytes, term) ||
                    !addSize(encoded_bytes, term) ||
                    !addSize(encoded_bytes, world.payloadBytes()) ||
                    encoded_bytes > context.limits.max_encoded_bytes)
                {
                    return codecFailure();
                }

                std::vector<std::byte> bytes;
                bytes.reserve(encoded_bytes);
                WireWriter writer(bytes);
                writer.u32(WorldAssetPrimaryMagic);
                writer.u32(kWireVersion);
                writer.u64(world.schemas().size());
                writer.u64(world.objectCount());
                writer.u64(world.dataCount());
                writer.u64(name_bytes);
                writer.u64(world.payloadBytes());
                for (const auto& schema : world.schemas())
                {
                    writer.u64(schema.hash);
                    writer.u64(schema.name.size());
                    writer.bytes({
                        reinterpret_cast<const std::byte*>(schema.name.data()),
                        schema.name.size()
                    });
                }
                for (std::size_t object_index{};
                     object_index < world.objectCount();
                     ++object_index)
                {
                    const auto object = world.objectAt(object_index);
                    writer.bytes(object.id().value.as_bytes());
                    writer.u64(object.dataCount());
                    for (std::size_t data_index{};
                         data_index < object.dataCount();
                         ++data_index)
                    {
                        const auto data = object.dataAt(data_index);
                        const auto schema = std::lower_bound(
                            world.schemas().begin(),
                            world.schemas().end(),
                            data.schema(),
                            WorldDataSchemaIdLess{}
                        );
                        if (schema == world.schemas().end() ||
                            *schema != data.schema())
                        {
                            return codecFailure();
                        }
                        writer.u64(static_cast<std::uint64_t>(
                            std::distance(world.schemas().begin(), schema)
                        ));
                        writer.u32(data.version());
                        writer.u64(data.payload().size());
                        writer.bytes(data.payload());
                    }
                }
                if (!writer.ok() || bytes.size() != encoded_bytes)
                    return codecFailure();
                return bytes;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(
                    lux::asset::EAssetCodecError::OUT_OF_MEMORY
                );
            }
            catch (...)
            {
                return codecFailure();
            }
        }
    }

    lux::asset::AssetCodecDescriptor worldAssetCodecDescriptor(
        std::shared_ptr<const void> code_lifetime
    )
    {
        return lux::asset::AssetCodecDescriptor{
            lux::asset::AssetTypeId::fromName(WorldAssetCanonicalName),
            std::string(WorldAssetCanonicalName),
            WorldAssetPrimaryMagic,
            0U,
            lux::cxx::typeToken<WorldDescription>(),
            &decodeWorld,
            &encodeWorld,
            std::move(code_lifetime)};
    }
}
