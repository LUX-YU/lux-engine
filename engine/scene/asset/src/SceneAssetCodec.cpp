#include <lux/engine/scene/SceneAssetCodec.hpp>

#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/resource/asset/detail/CookedAssetWriter.hpp>
#include <lux/engine/scene/SceneDescriptionBuilder.hpp>
#include <lux/engine/serialization/BinaryReader.hpp>
#include <lux/engine/serialization/BinaryWriter.hpp>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <new>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace lux::scene
{
    namespace detail
    {
        inline constexpr std::uint32_t kVersion = 2U;
        inline constexpr std::size_t kHeaderSize = 56U;
        inline constexpr std::size_t kDecodedSystemRecordCharge = 256U;
        inline constexpr std::size_t kDecodedBindingRecordCharge = 128U;
        inline constexpr std::size_t kDecodedDependencyRecordCharge = 32U;

        [[nodiscard]] lux::serialization::SerializationResult writeString(
            lux::serialization::BinaryWriter& writer,
            std::string_view value
        ) noexcept
        {
            if (value.size() > std::numeric_limits<std::uint32_t>::max())
            {
                return lux::cxx::unexpected(lux::serialization::SerializationFailure{
                    lux::serialization::ESerializationError::SIZE_OVERFLOW,
                    writer.offset()
                });
            }
            auto result = writer.writeUnsigned(static_cast<std::uint32_t>(value.size()));
            if (!result)
            {
                return result;
            }
            return writer.writeBytes(std::as_bytes(std::span(value)));
        }

        [[nodiscard]] lux::cxx::expected<std::string, lux::serialization::SerializationFailure> readString(
            lux::serialization::BinaryReader& reader,
            std::size_t& charged,
            std::size_t max_decoded_bytes
        ) noexcept
        {
            auto size = reader.readUnsigned<std::uint32_t>();
            if (!size)
            {
                return lux::cxx::unexpected(size.error());
            }
            if (*size > reader.remaining() || *size > max_decoded_bytes - charged)
            {
                return lux::cxx::unexpected(lux::serialization::SerializationFailure{
                    lux::serialization::ESerializationError::LIMIT_EXCEEDED,
                    reader.offset()
                });
            }
            try
            {
                std::string value(*size, '\0');
                auto read = reader.readBytes(std::as_writable_bytes(std::span(value)));
                if (!read)
                {
                    return lux::cxx::unexpected(read.error());
                }
                charged += *size;
                return value;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(lux::serialization::SerializationFailure{
                    lux::serialization::ESerializationError::ALLOCATION_FAILURE,
                    reader.offset()
                });
            }
        }

        [[nodiscard]] bool addCharge(
            std::size_t count,
            std::size_t item,
            std::size_t limit,
            std::size_t& total
        ) noexcept
        {
            if (count != 0U && item > std::numeric_limits<std::size_t>::max() / count)
            {
                return false;
            }
            const std::size_t amount = count * item;
            if (amount > limit - total)
            {
                return false;
            }
            total += amount;
            return true;
        }

        [[nodiscard]] lux::cxx::expected<std::vector<std::byte>, lux::asset::EAssetCodecError> encode(
            const SceneDescription& scene,
            std::size_t max_encoded_bytes
        ) noexcept
        {
            if (scene.world().isNull() || scene.simulation().isNull() ||
                scene.systemCount() > std::numeric_limits<std::uint32_t>::max() ||
                scene.dependencyCount() > std::numeric_limits<std::uint32_t>::max())
            {
                return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
            }
            std::size_t binding_count{};
            for (std::size_t index{}; index < scene.systemCount(); ++index)
            {
                binding_count += scene.systemAt(index).requirementBindingCount();
            }
            if (binding_count > std::numeric_limits<std::uint32_t>::max())
            {
                return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
            }
            try
            {
                std::vector<std::byte> bytes;
                bytes.reserve(kHeaderSize);
                lux::serialization::BinaryWriter writer(bytes);
                auto result = writer.writeUnsigned(SceneAssetPrimaryMagic);
                if (result) result = writer.writeUnsigned(kVersion);
                if (result) result = writer.writeBytes(scene.world().bytes());
                if (result) result = writer.writeBytes(scene.simulation().bytes());
                if (result) result = writer.writeUnsigned(static_cast<std::uint32_t>(scene.systemCount()));
                if (result) result = writer.writeUnsigned(static_cast<std::uint32_t>(binding_count));
                if (result) result = writer.writeUnsigned(static_cast<std::uint32_t>(scene.dependencyCount()));
                if (result) result = writer.writeUnsigned(std::uint32_t{});

                for (std::size_t index{}; result && index < scene.systemCount(); ++index)
                {
                    const auto system = scene.systemAt(index);
                    result = writer.writeUnsigned(system.instanceId().value);
                    if (result) result = writeString(writer, system.instanceName());
                    if (result) result = writer.writeUnsigned(system.type().hash);
                    if (result) result = writeString(writer, system.type().name);
                    if (result) result = writer.writeUnsigned(system.version());
                    if (result) result = writeString(writer, system.configurationSchemaName());
                    if (result) result = writer.writeUnsigned(system.configurationSchemaHash());
                    if (result) result = writer.writeUnsigned(system.configurationSchemaVersion());
                    if (result) result = writer.writeUnsigned(
                        static_cast<std::uint64_t>(system.configurationPayload().size())
                    );
                    if (result) result = writer.writeBytes(system.configurationPayload());
                }
                for (std::size_t system_ordinal{}; result && system_ordinal < scene.systemCount(); ++system_ordinal)
                {
                    const auto system = scene.systemAt(system_ordinal);
                    for (std::size_t binding{}; result && binding < system.requirementBindingCount(); ++binding)
                    {
                        const auto value = system.requirementBindingAt(binding);
                        result = writer.writeUnsigned(static_cast<std::uint32_t>(system_ordinal));
                        if (result) result = writeString(writer, value.requirement());
                        if (result) result = writeString(writer, value.provider());
                    }
                }
                for (std::size_t index{}; result && index < scene.dependencyCount(); ++index)
                {
                    const auto dependency = scene.dependencyAt(index);
                    std::uint32_t before_index{}, after_index{};
                    for (std::size_t system_index{}; system_index < scene.systemCount(); ++system_index)
                    {
                        const auto candidate = scene.systemAt(system_index).instanceId();
                        if (candidate == dependency.before()) before_index = static_cast<std::uint32_t>(system_index);
                        if (candidate == dependency.after()) after_index = static_cast<std::uint32_t>(system_index);
                    }
                    result = writer.writeUnsigned(before_index);
                    if (result) result = writer.writeUnsigned(after_index);
                }
                if (!result || bytes.size() > max_encoded_bytes)
                {
                    return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                }
                return bytes;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(lux::asset::EAssetCodecError::OUT_OF_MEMORY);
            }
        }

        [[nodiscard]] lux::cxx::expected<std::shared_ptr<const SceneDescription>, lux::asset::EAssetCodecError> decode(
            std::span<const std::byte> input,
            std::size_t max_input_bytes,
            std::size_t max_decoded_bytes
        ) noexcept
        {
            if (input.size() > max_input_bytes || input.size() < kHeaderSize ||
                sizeof(SceneDescription) > max_decoded_bytes)
            {
                return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
            }
            try
            {
                lux::serialization::BinaryReader reader(input);
                auto magic = reader.readUnsigned<std::uint32_t>();
                auto version = reader.readUnsigned<std::uint32_t>();
                std::array<std::uint8_t, 16U> world{};
                std::array<std::uint8_t, 16U> simulation{};
                auto world_read = reader.readBytes(std::as_writable_bytes(std::span(world)));
                auto simulation_read = reader.readBytes(std::as_writable_bytes(std::span(simulation)));
                auto system_count = reader.readUnsigned<std::uint32_t>();
                auto binding_count = reader.readUnsigned<std::uint32_t>();
                auto dependency_count = reader.readUnsigned<std::uint32_t>();
                auto reserved = reader.readUnsigned<std::uint32_t>();
                const bool invalid_header = !magic || !version || !world_read || !simulation_read || !system_count ||
                    !binding_count || !dependency_count || !reserved || *magic != SceneAssetPrimaryMagic ||
                    *version != kVersion || *reserved != 0U;
                if (invalid_header)
                {
                    return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                }

                std::size_t charged{sizeof(SceneDescription)};
                if (!addCharge(*system_count, kDecodedSystemRecordCharge, max_decoded_bytes, charged) ||
                    !addCharge(*binding_count, kDecodedBindingRecordCharge, max_decoded_bytes, charged) ||
                    !addCharge(*dependency_count, kDecodedDependencyRecordCharge, max_decoded_bytes, charged))
                {
                    return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                }

                SceneDescriptionBuilder builder;
                builder.setWorld(lux::asset::AssetId(world));
                builder.setSimulation(lux::asset::AssetId(simulation));
                std::vector<system::SystemInstanceId> instances;
                instances.reserve(*system_count);
                for (std::uint32_t index{}; index < *system_count; ++index)
                {
                    auto id = reader.readUnsigned<std::uint64_t>();
                    auto instance_name = readString(reader, charged, max_decoded_bytes);
                    auto type_hash = reader.readUnsigned<std::uint64_t>();
                    auto type_name = readString(reader, charged, max_decoded_bytes);
                    auto system_version = reader.readUnsigned<std::uint32_t>();
                    auto schema_name = readString(reader, charged, max_decoded_bytes);
                    auto schema_hash = reader.readUnsigned<std::uint64_t>();
                    auto schema_version = reader.readUnsigned<std::uint32_t>();
                    auto config_size = reader.readUnsigned<std::uint64_t>();
                    if (!id || !instance_name || !type_hash || !type_name || !system_version || !schema_name ||
                        !schema_hash || !schema_version || !config_size || *config_size > reader.remaining() ||
                        *config_size > max_decoded_bytes - charged ||
                        (!instances.empty() && instances.back().value >= *id))
                    {
                        return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                    }
                    const system::SystemTypeId type{*type_hash, *type_name};
                    const bool invalid_schema = schema_name->empty()
                        ? (*schema_hash != 0U || *schema_version != 0U || *config_size != 0U)
                        : (*schema_hash != lux::cxx::Fnv1a64::hash(*schema_name) || *schema_version == 0U);
                    if (!system::SystemInstanceId{*id}.valid() || instance_name->empty() || !type.valid() ||
                        *system_version == 0U || invalid_schema)
                    {
                        return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                    }
                    const auto config = input.subspan(reader.offset(), static_cast<std::size_t>(*config_size));
                    std::vector<std::byte> sink(static_cast<std::size_t>(*config_size));
                    if (!reader.readBytes(sink))
                    {
                        return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                    }
                    charged += static_cast<std::size_t>(*config_size);
                    const system::SystemInstanceId instance{*id};
                    if (!builder.addSystem(
                            instance,
                            *instance_name,
                            type,
                            *system_version,
                            *schema_name,
                            *schema_version,
                            config))
                    {
                        return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                    }
                    instances.push_back(instance);
                }
                for (std::uint32_t index{}; index < *binding_count; ++index)
                {
                    auto ordinal = reader.readUnsigned<std::uint32_t>();
                    auto requirement = readString(reader, charged, max_decoded_bytes);
                    auto provider = readString(reader, charged, max_decoded_bytes);
                    if (!ordinal || !requirement || !provider || *ordinal >= instances.size() ||
                        !builder.bindRequirement(instances[*ordinal], *requirement, *provider))
                    {
                        return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                    }
                }
                for (std::uint32_t index{}; index < *dependency_count; ++index)
                {
                    auto before = reader.readUnsigned<std::uint32_t>();
                    auto after = reader.readUnsigned<std::uint32_t>();
                    if (!before || !after || *before >= instances.size() || *after >= instances.size() ||
                        !builder.addDependency(instances[*before], instances[*after]))
                    {
                        return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                    }
                }
                if (reader.remaining() != 0U)
                {
                    return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                }
                auto built = std::move(builder).build();
                if (!built)
                {
                    return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
                }
                return std::make_shared<SceneDescription>(std::move(*built));
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(lux::asset::EAssetCodecError::OUT_OF_MEMORY);
            }
        }
    } // namespace detail

    SceneAsset::SceneAsset(
        lux::asset::AssetInfo info,
        std::shared_ptr<const SceneDescription> data,
        std::vector<lux::asset::AssetAuxiliaryPayload> auxiliary
    ) noexcept
        : TAsset(std::move(info), std::move(data), std::move(auxiliary))
    {
    }

    lux::cxx::expected<std::shared_ptr<const SceneAsset>, lux::asset::AssetDecodeFailure> SceneAsset::create(
        lux::asset::AssetInfo info,
        std::shared_ptr<const SceneDescription> data,
        std::vector<lux::asset::AssetAuxiliaryPayload> auxiliary
    ) noexcept
    {
        const bool invalid = !data || data->world().isNull() || data->simulation().isNull();
        if (info.id.isNull() || invalid)
        {
            return lux::cxx::unexpected(lux::asset::AssetDecodeFailure{
                lux::asset::EAssetDecodeError::INVALID_PAYLOAD,
                0U
            });
        }
        info.type = asset_type;
        try
        {
            std::sort(auxiliary.begin(), auxiliary.end(), [](const auto& left, const auto& right) noexcept {
                return left.tag < right.tag;
            });
            for (std::size_t index{}; index < auxiliary.size(); ++index)
            {
                const bool invalid_payload = auxiliary[index].tag == 0U || auxiliary[index].bytes.empty();
                const bool duplicate = index != 0U && auxiliary[index - 1U].tag == auxiliary[index].tag;
                if (invalid_payload || duplicate)
                {
                    return lux::cxx::unexpected(lux::asset::AssetDecodeFailure{
                        lux::asset::EAssetDecodeError::INVALID_PAYLOAD,
                        index
                    });
                }
            }
            return std::shared_ptr<const SceneAsset>(
                new SceneAsset(std::move(info), std::move(data), std::move(auxiliary))
            );
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(lux::asset::AssetDecodeFailure{
                lux::asset::EAssetDecodeError::ALLOCATION_FAILURE,
                0U
            });
        }
    }
} // namespace lux::scene

namespace lux::asset
{
    lux::cxx::expected<std::shared_ptr<const lux::scene::SceneAsset>, AssetDecodeFailure>
    TAssetSerDeser<lux::scene::SceneAsset>::decode(
        AssetId requested,
        lux::cxx::SharedBytes<> cooked_image,
        const AssetDecodeLimits& limits
    ) noexcept
    {
        auto image = inspectCookedAssetImage(requested, std::move(cooked_image), limits);
        if (!image) return lux::cxx::unexpected(image.error());
        if (image->magic() != lux::scene::SceneAsset::primary_magic)
            return lux::cxx::unexpected(AssetDecodeFailure{EAssetDecodeError::INVALID_MAGIC, 0U});
        if (image->metadata().legacy_type_tag != lux::scene::SceneAsset::legacy_type_tag)
            return lux::cxx::unexpected(AssetDecodeFailure{EAssetDecodeError::INVALID_TYPE, 0U});
        if (!image->information().empty())
            return lux::cxx::unexpected(AssetDecodeFailure{EAssetDecodeError::INVALID_LAYOUT, 0U});
        auto description = lux::scene::detail::decode(
            image->data().view(),
            image->data().size(),
            limits.max_decoded_bytes
        );
        if (!description)
        {
            const auto code = description.error() == EAssetCodecError::OUT_OF_MEMORY
                ? EAssetDecodeError::ALLOCATION_FAILURE
                : EAssetDecodeError::INVALID_PAYLOAD;
            return lux::cxx::unexpected(AssetDecodeFailure{code, 0U});
        }
        try
        {
            std::vector<AssetAuxiliaryPayload> auxiliary(
                image->auxiliaryPayloads().begin(),
                image->auxiliaryPayloads().end()
            );
            return lux::scene::SceneAsset::create(
                AssetInfo{
                    image->metadata().id,
                    lux::scene::SceneAsset::asset_type,
                    image->metadata().date,
                    image->metadata().display_name,
                    image->metadata().source_path,
                    image->metadata().source_mtime
                },
                std::move(*description),
                std::move(auxiliary)
            );
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(AssetDecodeFailure{EAssetDecodeError::ALLOCATION_FAILURE, 0U});
        }
    }

    lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure>
    TAssetSerDeser<lux::scene::SceneAsset>::encode(
        const lux::scene::SceneAsset& asset,
        const AssetEncodeLimits& limits
    ) noexcept
    {
        auto payload = lux::scene::detail::encode(asset.data(), limits.max_encoded_bytes);
        if (!payload)
        {
            const auto code = payload.error() == EAssetCodecError::OUT_OF_MEMORY
                ? EAssetEncodeError::ALLOCATION_FAILURE
                : EAssetEncodeError::INVALID_PAYLOAD;
            return lux::cxx::unexpected(AssetEncodeFailure{code, 0U});
        }
        return detail::encodeCookedAssetImage(
            detail::CookedAssetWriteRequest{
                lux::scene::SceneAsset::primary_magic,
                lux::scene::SceneAsset::legacy_type_tag,
                asset.info(),
                {},
                *payload,
                asset.auxiliaryPayloads()
            },
            limits
        );
    }
} // namespace lux::asset
