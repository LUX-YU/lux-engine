#include <lux/engine/scene/SceneAssetCodec.hpp>
#include <lux/engine/scene/SceneStreamingCodec.hpp>
#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/resource/asset/detail/CookedAssetWriter.hpp>
#include <lux/engine/serialization/Serialization.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>

#include <algorithm>
#include <limits>
#include <tuple>
#include <unordered_set>

namespace lux::scene
{
    namespace detail
    {
        constexpr std::uint32_t kVersion = 3;
        constexpr std::uint64_t kContentHeaderSize = 28;

        bool addCharge(std::size_t count, std::size_t item, std::size_t limit, std::size_t& charged) noexcept
        {
            if (charged > limit || (count && item > (limit - charged) / count))
            {
                return false;
            }
            charged += count * item;
            return true;
        }

        bool validateRoot(const SceneData& data) noexcept
        {
            const auto& name = data.content_file;
            const bool invalid_member = name.empty() || name == "." || name == ".." ||
                name.find_first_of("/\\:") != std::string::npos || name.find('\0') != std::string::npos;
            if (invalid_member || data.blocks.size() >= UINT32_MAX)
            {
                return false;
            }
            std::unordered_set<std::string_view> geometries;
            for (const auto& geometry : data.description.world.supported_geometries)
            {
                const bool invalid = geometry.name.empty() || geometry.partitioner.name.empty() ||
                    !geometries.insert(geometry.name).second ||
                    (!geometry.partitioner.configuration_version && !geometry.partitioner.configuration.empty());
                if (invalid)
                {
                    return false;
                }
            }
            const auto validSystems = [](const auto& systems) noexcept
            {
                return std::ranges::all_of(systems, [](const auto& system) noexcept
                {
                    return !system.type.empty() && (system.configuration_version || system.configuration.empty());
                });
            };
            if (!validSystems(data.description.systems))
            {
                return false;
            }
            for (const auto& group : data.description.simulation.groups)
            {
                const bool invalid_step = group.fixed_step && group.fixed_step->count() <= 0;
                const bool invalid = invalid_step || !validSystems(group.systems);
                if (invalid)
                {
                    return false;
                }
            }
            for (const auto& block : data.blocks)
            {
                if (!block.size)
                {
                    const bool invalid_hole = block.summary_size != 0 ||
                        block.summary_digest != cxx::algorithm::Sha256Digest{} ||
                        block.digest != cxx::algorithm::Sha256Digest{};
                    if (invalid_hole)
                    {
                        return false;
                    }
                    continue;
                }
                const bool invalid = block.offset < kContentHeaderSize || !block.summary_size ||
                    block.summary_size > block.size || block.size > UINT64_MAX - block.offset;
                if (invalid)
                {
                    return false;
                }
            }
            const bool valid_bootstrap = std::ranges::all_of(data.bootstrap_blocks, [&](std::uint32_t block) noexcept
            {
                return block < data.blocks.size() && data.blocks[block].size != 0;
            });
            const auto& streaming = data.streaming;
            const bool invalid_sizes = streaming.block_groups.size() != data.blocks.size() ||
                streaming.geometry_indexes.size() != data.description.world.supported_geometries.size();
            if (!valid_bootstrap || invalid_sizes) return false;
            for (std::size_t block = 0; block < data.blocks.size(); ++block)
            {
                const auto group = streaming.block_groups[block];
                const bool invalid_group = data.blocks[block].size == 0
                    ? group != UINT32_MAX : group >= streaming.groups.size();
                if (invalid_group) return false;
            }
            std::vector<std::uint32_t> bootstrap;
            for (auto block : data.bootstrap_blocks) bootstrap.push_back(streaming.block_groups[block]);
            std::ranges::sort(bootstrap);
            bootstrap.erase(std::unique(bootstrap.begin(), bootstrap.end()), bootstrap.end());
            auto digest = sceneStreamingInputDigest(data);
            return digest && *digest == streaming.input_digest && bootstrap == streaming.bootstrap_groups;
        }

        bool
        writeDescription(
            serialization::BinaryWriter& writer, const SceneDescription& value,
            const serialization::SerializationBudget& budget
        ) noexcept
        {
            const auto fields = [&](const auto&... values) noexcept
            {
                return bool(serialization::write(writer, std::tie(values...), budget));
            };
            const auto systems = [&](const auto& values) noexcept
            {
                if (!writer.writeUnsigned(static_cast<std::uint64_t>(values.size()))) return false;
                for (const auto& value : values)
                {
                    if (!fields(value.type, value.name, value.configuration_version, value.configuration)) return false;
                }
                return true;
            };
            const bool described = fields(value.world.name) &&
                writer.writeUnsigned(static_cast<std::uint64_t>(value.world.supported_geometries.size()));
            if (!described) return false;
            for (const auto& geometry : value.world.supported_geometries)
            {
                const auto& partitioner = geometry.partitioner;
                const bool valid = fields(
                    geometry.name, partitioner.name, partitioner.configuration_version, partitioner.configuration
                );
                if (!valid) return false;
            }
            if (!writer.writeUnsigned(static_cast<std::uint64_t>(value.simulation.groups.size()))) return false;
            for (const auto& group : value.simulation.groups)
            {
                const bool fixed = group.fixed_step.has_value();
                if (!fields(fixed)) return false;
                if (fixed && !writer.writeSigned(static_cast<std::int64_t>(group.fixed_step->count()))) return false;
                if (!systems(group.systems)) return false;
            }
            return systems(value.systems);
        }

        bool
        readDescription(
            serialization::BinaryReader& reader, SceneDescription& value, std::size_t limit, std::size_t& charged,
            asset::AssetDecodeFailure& failure
        ) noexcept
        {
            const serialization::SerializationBudget budget{limit, limit, 32};
            const auto fields = [&](auto&... values) noexcept
            {
                auto fields = std::tie(values...);
                auto result = serialization::read(reader, fields, budget);
                if (!result && result.error().code == serialization::ESerializationError::LIMIT_EXCEEDED)
                {
                    failure.code = asset::EAssetDecodeError::LIMIT_EXCEEDED;
                }
                return bool(result);
            };
            const auto resize = [&](auto& values) noexcept
            {
                auto count = reader.readUnsigned<std::uint64_t>();
                using Value = typename std::remove_reference_t<decltype(values)>::value_type;
                if (!count) return false;
                if (*count > SIZE_MAX || !addCharge(static_cast<std::size_t>(*count), sizeof(Value), limit, charged))
                {
                    failure.code = asset::EAssetDecodeError::LIMIT_EXCEEDED;
                    return false;
                }
                values.resize(static_cast<std::size_t>(*count));
                return true;
            };
            const auto systems = [&](auto& values) noexcept
            {
                if (!resize(values)) return false;
                for (auto& value : values)
                {
                    if (!fields(value.type, value.name, value.configuration_version, value.configuration)) return false;
                }
                return true;
            };
            if (!fields(value.world.name) || !resize(value.world.supported_geometries)) return false;
            for (auto& geometry : value.world.supported_geometries)
            {
                auto& partitioner = geometry.partitioner;
                const bool valid = fields(
                    geometry.name, partitioner.name, partitioner.configuration_version, partitioner.configuration
                );
                if (!valid) return false;
            }
            if (!resize(value.simulation.groups)) return false;
            for (auto& group : value.simulation.groups)
            {
                bool fixed{};
                if (!fields(fixed)) return false;
                if (fixed)
                {
                    auto step = reader.readSigned<std::int64_t>();
                    if (!step || *step <= 0) return false;
                    group.fixed_step = simulation::SimulationDuration{*step};
                }
                if (!systems(group.systems)) return false;
            }
            return systems(value.systems);
        }

        cxx::expected<std::vector<std::byte>, asset::AssetEncodeFailure>
        encode(const SceneData& data, std::size_t limit) noexcept
        {
            if (!validateRoot(data))
            {
                return cxx::unexpected(asset::AssetEncodeFailure{asset::EAssetEncodeError::INVALID_PAYLOAD});
            }
            std::vector<std::byte> output;
            serialization::BinaryWriter writer(output);
            const serialization::SerializationBudget budget{limit, limit, 32};
            const bool described = writer.writeUnsigned(kVersion) &&
                writeDescription(writer, data.description, budget) &&
                serialization::write(writer, data.content_file, budget) &&
                writer.writeUnsigned(static_cast<std::uint64_t>(data.blocks.size()));
            if (!described || writer.offset() > limit)
            {
                return cxx::unexpected(asset::AssetEncodeFailure{
                    asset::EAssetEncodeError::LIMIT_EXCEEDED, writer.offset()
                });
            }
            for (const auto& block : data.blocks)
            {
                const auto fields = std::tie(block.offset, block.size, block.summary_size);
                const bool written = serialization::write(writer, fields, budget) &&
                    writer.writeBytes(block.summary_digest.bytes()) && writer.writeBytes(block.digest.bytes());
                if (!written || writer.offset() > limit)
                {
                    return cxx::unexpected(asset::AssetEncodeFailure{
                        asset::EAssetEncodeError::LIMIT_EXCEEDED, writer.offset()
                    });
                }
            }
            if (!serialization::write(writer, data.bootstrap_blocks, budget) || writer.offset() > limit)
            {
                return cxx::unexpected(asset::AssetEncodeFailure{
                    asset::EAssetEncodeError::LIMIT_EXCEEDED, writer.offset()
                });
            }
            auto remaining = limit - writer.offset();
            if (auto written = writeSceneStreaming(writer, data.streaming, remaining); !written)
            {
                const auto code = written.error().type == error::errorId("lux.scene.content.capacity")
                    ? asset::EAssetEncodeError::LIMIT_EXCEEDED : asset::EAssetEncodeError::INVALID_PAYLOAD;
                return cxx::unexpected(asset::AssetEncodeFailure{
                    code, writer.offset()
                });
            }
            return output;
        }

        cxx::expected<std::shared_ptr<const SceneData>, asset::AssetDecodeFailure>
        decode(std::span<const std::byte> input, std::size_t limit) noexcept
        {
            serialization::BinaryReader reader(input);
            auto version = reader.readUnsigned<std::uint32_t>();
            if (!version || *version != kVersion)
            {
                return cxx::unexpected(asset::AssetDecodeFailure{asset::EAssetDecodeError::UNSUPPORTED_VERSION});
            }
            std::size_t charged = sizeof(SceneData);
            // All variable string/payload bytes are bounded by this input. Separately charge allocated records.
            if (!addCharge(input.size(), 1, limit, charged))
            {
                return cxx::unexpected(asset::AssetDecodeFailure{asset::EAssetDecodeError::LIMIT_EXCEEDED});
            }
            auto data = std::make_shared<SceneData>();
            const serialization::SerializationBudget budget{input.size(), input.size(), 32};
            asset::AssetDecodeFailure failure{asset::EAssetDecodeError::INVALID_PAYLOAD};
            if (!readDescription(reader, data->description, limit, charged, failure) ||
                !serialization::read(reader, data->content_file, budget))
            {
                failure.offset = reader.offset();
                return cxx::unexpected(failure);
            }
            auto count = reader.readUnsigned<std::uint64_t>();
            const bool invalid_count = !count || *count >= UINT32_MAX || *count > reader.remaining() / 88 ||
                !addCharge(static_cast<std::size_t>(*count), sizeof(SceneBlockDescription), limit, charged);
            if (invalid_count)
            {
                return cxx::unexpected(asset::AssetDecodeFailure{
                    asset::EAssetDecodeError::LIMIT_EXCEEDED, reader.offset()
                });
            }
            data->blocks.resize(static_cast<std::size_t>(*count));
            for (auto& block : data->blocks)
            {
                auto fields = std::tie(block.offset, block.size, block.summary_size);
                const bool read = serialization::read(reader, fields, budget) &&
                    reader.readBytes(std::span(block.summary_digest.data(), block.summary_digest.size())) &&
                    reader.readBytes(std::span(block.digest.data(), block.digest.size()));
                if (!read)
                {
                    return cxx::unexpected(asset::AssetDecodeFailure{
                        asset::EAssetDecodeError::TRUNCATED, reader.offset()
                    });
                }
            }
            if (!serialization::read(reader, data->bootstrap_blocks, budget) ||
                !addCharge(data->bootstrap_blocks.size(), sizeof(std::uint32_t), limit, charged))
                return cxx::unexpected(asset::AssetDecodeFailure{asset::EAssetDecodeError::LIMIT_EXCEEDED});
            auto remaining = limit - charged;
            auto streaming = readSceneStreaming(reader, remaining);
            if (!streaming)
            {
                const auto code = streaming.error().type == error::errorId("lux.scene.content.capacity")
                    ? asset::EAssetDecodeError::LIMIT_EXCEEDED : asset::EAssetDecodeError::INVALID_PAYLOAD;
                return cxx::unexpected(asset::AssetDecodeFailure{code, reader.offset()});
            }
            data->streaming = std::move(*streaming);
            const bool valid_root = reader.remaining() == 0 && validateRoot(*data);
            if (!valid_root)
            {
                return cxx::unexpected(asset::AssetDecodeFailure{
                    asset::EAssetDecodeError::INVALID_PAYLOAD, reader.offset()
                });
            }
            return data;
        }
    } // namespace detail

    cxx::expected<cxx::algorithm::Sha256Digest, error::Error>
    sceneStreamingInputDigest(const SceneData& data) noexcept
    {
        cxx::algorithm::Sha256 digest;
        digest.update("lux.scene.streaming.input.v1");
        std::vector<std::byte> bytes;
        serialization::BinaryWriter writer(bytes);
        if (!detail::writeDescription(writer, data.description, {SIZE_MAX, SIZE_MAX, 32}))
            return cxx::unexpected(error::Error{error::errorId("lux.scene.content.invalid")});
        digest.update(bytes);
        bytes.clear();
        static_cast<void>(writer.writeUnsigned(static_cast<std::uint64_t>(data.blocks.size())));
        digest.update(bytes);
        for (std::size_t index = 0; index < data.blocks.size(); ++index)
        {
            bytes.clear();
            const auto& block = data.blocks[index];
            static_cast<void>(writer.writeUnsigned(static_cast<std::uint64_t>(index)));
            static_cast<void>(writer.writeUnsigned(std::uint8_t{block.size != 0}));
            if (block.size) static_cast<void>(writer.writeBytes(block.summary_digest.bytes()));
            digest.update(bytes);
        }
        bytes.clear();
        if (!serialization::write(writer, data.bootstrap_blocks, {SIZE_MAX, SIZE_MAX, 32}))
            return cxx::unexpected(error::Error{error::errorId("lux.scene.content.invalid")});
        digest.update(bytes);
        return digest.digest();
    }

    SceneAsset::SceneAsset(
        asset::AssetInfo info, std::shared_ptr<const SceneData> data,
        std::vector<asset::AssetAuxiliaryPayload> auxiliary
    ) noexcept : TAsset(std::move(info), std::move(data), std::move(auxiliary)) {}

    cxx::expected<std::shared_ptr<const SceneAsset>, asset::AssetDecodeFailure>
    SceneAsset::create(
        asset::AssetInfo info, std::shared_ptr<const SceneData> data,
        std::vector<asset::AssetAuxiliaryPayload> auxiliary
    ) noexcept
    {
        if (info.id.isNull() || !data || !detail::validateRoot(*data))
        {
            return cxx::unexpected(asset::AssetDecodeFailure{asset::EAssetDecodeError::INVALID_PAYLOAD});
        }
        info.type = asset_type;
        std::ranges::sort(auxiliary, {}, &asset::AssetAuxiliaryPayload::tag);
        for (std::size_t i = 0; i < auxiliary.size(); ++i)
        {
            const bool invalid = !auxiliary[i].tag || auxiliary[i].bytes.empty() ||
                (i && auxiliary[i - 1].tag == auxiliary[i].tag);
            if (invalid)
            {
                return cxx::unexpected(asset::AssetDecodeFailure{asset::EAssetDecodeError::INVALID_PAYLOAD, i});
            }
        }
        return std::shared_ptr<const SceneAsset>{
            new SceneAsset(std::move(info), std::move(data), std::move(auxiliary))
        };
    }
} // namespace lux::scene

namespace lux::scene
{
    namespace
    {
        constexpr std::string_view kRootFormat = "LXSCENE1";

        class SceneAssetCodec final : public asset::AssetCodec
        {
        public:
            asset::AssetTypeId type() const noexcept override { return SceneAsset::asset_type; }
            std::uint32_t magic() const noexcept override { return SceneAsset::primary_magic; }
            cxx::type_info dataType() const noexcept override { return cxx::make_basic_type_info<SceneData>(); }

            cxx::expected<std::shared_ptr<const asset::Asset>, asset::AssetDecodeFailure>
            decode(
                asset::AssetId id, cxx::SharedBytes<> bytes, const asset::AssetDecodeLimits& limits
            ) const noexcept override
            {
                auto decoded = asset::TAssetSerDeser<SceneAsset>::decode(id, std::move(bytes), limits);
                if (!decoded)
                {
                    return cxx::unexpected(decoded.error());
                }
                return std::move(*decoded);
            }

            cxx::expected<std::vector<std::byte>, asset::AssetEncodeFailure>
            encode(
                const asset::AssetInfo& info, const void* value,
                std::span<const asset::AssetAuxiliaryPayload> auxiliary, const asset::AssetEncodeLimits& limits
            ) const noexcept override
            {
                if (!value)
                {
                    return cxx::unexpected(asset::AssetEncodeFailure{asset::EAssetEncodeError::INVALID_ASSET});
                }
                return asset::TAssetSerDeser<SceneAsset>::encode(
                    info, *static_cast<const SceneData*>(value), auxiliary, limits
                );
            }

            const void* data(const asset::Asset& value) const noexcept override
            {
                return &static_cast<const SceneAsset&>(value).data();
            }

            std::string_view contentFile(const asset::Asset& value) const noexcept override
            {
                return static_cast<const SceneAsset&>(value).data().content_file;
            }
        };
    }

    std::unique_ptr<asset::AssetCodec> createSceneCodec()
    {
        constexpr error::ErrorDescriptor descriptors[]{
            {"lux.scene.content.invalid", "Invalid Scene content at object {0}", error::ERecovery::NEEDS_INPUT,
                {error::EArgument::UNSIGNED}},
            {"lux.scene.content.capacity", "Scene content exceeds its explicit capacity"},
            {"lux.scene.content.schema", "Unavailable or invalid Scene component codec ({0}, {1})",
                error::ERecovery::NEEDS_INPUT, {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
            {"lux.scene.content.restore", "Scene group restore failed ({0}, object {1}, field {2})",
                error::ERecovery::NEEDS_INPUT,
                {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}}
        };
        if (!error::ErrorRegistry::instance().registerTypes(descriptors))
        {
            std::terminate(); // Fixed module declarations collided; this is not malformed asset input.
        }
        return std::make_unique<SceneAssetCodec>();
    }
} // namespace lux::scene

namespace lux::asset
{
    cxx::expected<std::shared_ptr<const scene::SceneAsset>, AssetDecodeFailure>
    TAssetSerDeser<scene::SceneAsset>::decode(
        AssetId requested, cxx::SharedBytes<> bytes, const AssetDecodeLimits& limits
    ) noexcept
    {
        auto image = inspectCookedAssetImage(requested, std::move(bytes), limits);
        if (!image)
        {
            return cxx::unexpected(image.error());
        }
        const bool wrong_type = image->magic() != scene::SceneAsset::primary_magic ||
            image->metadata().legacy_type_tag != scene::SceneAsset::legacy_type_tag;
        if (wrong_type)
        {
            return cxx::unexpected(AssetDecodeFailure{EAssetDecodeError::INVALID_TYPE});
        }
        const auto format = std::as_bytes(std::span(scene::kRootFormat));
        if (!std::ranges::equal(image->information().view(), format))
        {
            return cxx::unexpected(AssetDecodeFailure{EAssetDecodeError::UNSUPPORTED_VERSION});
        }
        auto data = scene::detail::decode(image->data().view(), limits.max_decoded_bytes);
        if (!data)
        {
            return cxx::unexpected(data.error());
        }
        const auto& metadata = image->metadata();
        return scene::SceneAsset::create({
            metadata.id, scene::SceneAsset::asset_type, metadata.date,
            metadata.display_name, metadata.source_path, metadata.source_mtime
        }, std::move(*data), {image->auxiliaryPayloads().begin(), image->auxiliaryPayloads().end()});
    }

    cxx::expected<std::vector<std::byte>, AssetEncodeFailure>
    TAssetSerDeser<scene::SceneAsset>::encode(const scene::SceneAsset& value, const AssetEncodeLimits& limits) noexcept
    {
        return encode(value.info(), value.data(), value.auxiliaryPayloads(), limits);
    }
    cxx::expected<std::vector<std::byte>, AssetEncodeFailure>
    TAssetSerDeser<scene::SceneAsset>::encode(
        const AssetInfo& info, const scene::SceneData& value,
        std::span<const AssetAuxiliaryPayload> auxiliary, const AssetEncodeLimits& limits
    ) noexcept
    {
        const bool invalid_identity = info.id.isNull() || info.type != scene::SceneAsset::asset_type;
        if (invalid_identity)
        {
            return cxx::unexpected(AssetEncodeFailure{EAssetEncodeError::INVALID_ASSET});
        }
        auto data = scene::detail::encode(value, limits.max_encoded_bytes);
        if (!data)
        {
            return cxx::unexpected(data.error());
        }
        return detail::encodeCookedAssetImage({
            scene::SceneAsset::primary_magic, scene::SceneAsset::legacy_type_tag, info,
            std::as_bytes(std::span(scene::kRootFormat)), *data, auxiliary
        }, limits);
    }
} // namespace lux::asset
