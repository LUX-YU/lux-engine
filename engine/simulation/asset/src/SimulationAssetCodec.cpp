#include <lux/engine/simulation/SimulationAssetCodec.hpp>

#include <lux/engine/serialization/BinaryReader.hpp>
#include <lux/engine/simulation/SimulationDescriptionBuilder.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace lux::simulation
{
    namespace
    {
        constexpr std::uint32_t kWireVersion = 1U;
        constexpr std::size_t kHeaderBytes = 40U;
        constexpr std::size_t kSchemaFixedBytes = 16U;
        constexpr std::size_t kDataFixedBytes = 20U;
        constexpr std::size_t kDecodedSchemaBytes = 64U;
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
        decodeSimulation(
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
                std::uint64_t data_count_u64{};
                std::uint64_t schema_name_bytes_u64{};
                std::uint64_t payload_bytes_u64{};
                if (!readUnsigned(reader, magic) ||
                    !readUnsigned(reader, version) ||
                    !readUnsigned(reader, schema_count_u64) ||
                    !readUnsigned(reader, data_count_u64) ||
                    !readUnsigned(reader, schema_name_bytes_u64) ||
                    !readUnsigned(reader, payload_bytes_u64) ||
                    magic != SimulationAssetPrimaryMagic ||
                    version != kWireVersion ||
                    schema_count_u64 != data_count_u64)
                {
                    return codecFailure();
                }

                std::size_t schema_count{};
                std::size_t data_count{};
                std::size_t schema_name_bytes{};
                std::size_t payload_bytes{};
                if (!toSize(schema_count_u64, schema_count) ||
                    !toSize(data_count_u64, data_count) ||
                    !toSize(schema_name_bytes_u64, schema_name_bytes) ||
                    !toSize(payload_bytes_u64, payload_bytes))
                {
                    return codecFailure();
                }

                std::size_t decoded_bytes = sizeof(SimulationDescription);
                std::size_t term{};
                if (!multiplySize(schema_count, kDecodedSchemaBytes, term) ||
                    !addSize(decoded_bytes, term) ||
                    !multiplySize(data_count, kDecodedDataBytes, term) ||
                    !addSize(decoded_bytes, term) ||
                    !addSize(decoded_bytes, schema_name_bytes) ||
                    !addSize(decoded_bytes, payload_bytes) ||
                    decoded_bytes > context.limits.max_decoded_bytes)
                {
                    return codecFailure();
                }

                std::vector<SimulationDataSchemaId> schemas;
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
                    SimulationDataSchemaId schema{hash, std::move(name)};
                    if (!schema.valid() ||
                        (index != 0U &&
                         !SimulationDataSchemaIdLess{}(schemas.back(), schema)))
                    {
                        return codecFailure();
                    }
                    schemas.push_back(std::move(schema));
                }
                if (actual_name_bytes != schema_name_bytes)
                    return codecFailure();

                SimulationDescriptionBuilder builder;
                std::size_t actual_payload_bytes{};
                for (std::size_t data_index{};
                     data_index < data_count;
                     ++data_index)
                {
                    std::uint64_t schema_ordinal{};
                    std::uint32_t data_version{};
                    std::uint64_t data_size_u64{};
                    std::size_t data_size{};
                    if (!readUnsigned(reader, schema_ordinal) ||
                        !readUnsigned(reader, data_version) ||
                        !readUnsigned(reader, data_size_u64) ||
                        schema_ordinal != data_index ||
                        data_version == 0U ||
                        !toSize(data_size_u64, data_size) ||
                        !addSize(actual_payload_bytes, data_size) ||
                        actual_payload_bytes > payload_bytes ||
                        data_size > reader.remaining())
                    {
                        return codecFailure();
                    }
                    std::vector<std::byte> data_payload(data_size);
                    if (!reader.readBytes(data_payload) ||
                        !builder.addData(
                            schemas[data_index],
                            data_version,
                            data_payload
                        ))
                    {
                        return codecFailure();
                    }
                }
                if (actual_payload_bytes != payload_bytes ||
                    reader.remaining() != 0U)
                {
                    return codecFailure();
                }
                auto description = std::move(builder).build();
                if (!description || description->dataCount() != data_count)
                    return codecFailure();
                auto payload = std::make_shared<const SimulationDescription>(
                    std::move(*description)
                );
                return lux::asset::DecodedAsset{
                    std::move(payload),
                    decoded_bytes};
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
        encodeSimulation(
            const void* payload,
            const lux::asset::AssetEncodeContext& context
        ) noexcept
        {
            if (payload == nullptr)
                return codecFailure();
            const auto& description =
                *static_cast<const SimulationDescription*>(payload);
            try
            {
                std::size_t name_bytes{};
                std::size_t encoded_bytes = kHeaderBytes;
                std::size_t term{};
                for (const auto& schema : description.schemas())
                {
                    if (!schema.valid() ||
                        !addSize(name_bytes, schema.name.size()))
                    {
                        return codecFailure();
                    }
                }
                if (!multiplySize(
                        description.schemas().size(),
                        kSchemaFixedBytes,
                        term
                    ) ||
                    !addSize(encoded_bytes, term) ||
                    !addSize(encoded_bytes, name_bytes) ||
                    !multiplySize(
                        description.dataCount(),
                        kDataFixedBytes,
                        term
                    ) ||
                    !addSize(encoded_bytes, term) ||
                    !addSize(encoded_bytes, description.payloadBytes()) ||
                    encoded_bytes > context.limits.max_encoded_bytes)
                {
                    return codecFailure();
                }

                std::vector<std::byte> bytes;
                bytes.reserve(encoded_bytes);
                WireWriter writer(bytes);
                writer.u32(SimulationAssetPrimaryMagic);
                writer.u32(kWireVersion);
                writer.u64(description.schemas().size());
                writer.u64(description.dataCount());
                writer.u64(name_bytes);
                writer.u64(description.payloadBytes());
                for (const auto& schema : description.schemas())
                {
                    writer.u64(schema.hash);
                    writer.u64(schema.name.size());
                    writer.bytes({
                        reinterpret_cast<const std::byte*>(schema.name.data()),
                        schema.name.size()
                    });
                }
                for (std::size_t index{};
                     index < description.dataCount();
                     ++index)
                {
                    const auto data = description.dataAt(index);
                    writer.u64(index);
                    writer.u32(data.version());
                    writer.u64(data.payload().size());
                    writer.bytes(data.payload());
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

    lux::asset::AssetCodecDescriptor simulationAssetCodecDescriptor(
        std::shared_ptr<const void> code_lifetime
    )
    {
        return lux::asset::AssetCodecDescriptor{
            lux::asset::AssetTypeId::fromName(SimulationAssetCanonicalName),
            std::string(SimulationAssetCanonicalName),
            SimulationAssetPrimaryMagic,
            0U,
            lux::cxx::typeToken<SimulationDescription>(),
            &decodeSimulation,
            &encodeSimulation,
            std::move(code_lifetime)};
    }
}
