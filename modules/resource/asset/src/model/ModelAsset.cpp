#include <lux/engine/resource/asset/model/ModelAsset.hpp>

#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/resource/asset/detail/CookedAssetWriter.hpp>
#include <lux/engine/serialization/BinaryReader.hpp>
#include <lux/engine/serialization/BinaryWriter.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <new>
#include <span>
#include <utility>
#include <vector>

namespace lux::asset
{
    namespace
    {
        inline constexpr std::uint32_t kModelVersion = 2U;

        [[nodiscard]] AssetDecodeFailure decodeFailure(EAssetDecodeError code) noexcept
        {
            return AssetDecodeFailure{code, 0U};
        }

        [[nodiscard]] AssetEncodeFailure encodeFailure(EAssetEncodeError code) noexcept
        {
            return AssetEncodeFailure{code, 0U};
        }

        [[nodiscard]] bool canonicalize(std::vector<AssetAuxiliaryPayload>& values) noexcept
        {
            std::sort(values.begin(), values.end(), [](const auto& left, const auto& right) noexcept {
                return left.tag < right.tag;
            });
            for (std::size_t index = 0U; index < values.size(); ++index)
            {
                const bool invalid = values[index].tag == 0U || values[index].bytes.empty();
                const bool duplicate = index != 0U && values[index - 1U].tag == values[index].tag;
                if (invalid || duplicate)
                    return false;
            }
            return true;
        }

        [[nodiscard]] bool finiteAffine(const Eigen::Affine3f& value) noexcept
        {
            if (!value.matrix().allFinite())
                return false;
            constexpr float epsilon = 1.0e-6F;
            return std::abs(value.matrix()(3, 0)) <= epsilon &&
                std::abs(value.matrix()(3, 1)) <= epsilon &&
                std::abs(value.matrix()(3, 2)) <= epsilon &&
                std::abs(value.matrix()(3, 3) - 1.0F) <= epsilon;
        }

        [[nodiscard]] bool validModel(const lux::rdesc::ModelDescription& model) noexcept
        {
            if (model.root_node != 0U || model.primitives.empty() || model.nodes.empty() ||
                model.primitives.size() > std::numeric_limits<std::uint32_t>::max() ||
                model.nodes.size() > std::numeric_limits<std::uint32_t>::max() ||
                model.animations.size() > std::numeric_limits<std::uint32_t>::max())
            {
                return false;
            }
            for (const auto& primitive : model.primitives)
                if (primitive.mesh.isNull() || primitive.material.isNull()) return false;
            if ((model.skeleton && model.skeleton->isNull()) || (!model.skeleton && !model.animations.empty()))
                return false;
            for (std::size_t index = 0U; index < model.animations.size(); ++index)
            {
                if (model.animations[index].isNull()) return false;
                for (std::size_t previous = 0U; previous < index; ++previous)
                    if (model.animations[previous] == model.animations[index]) return false;
            }
            try
            {
                std::vector<std::uint8_t> parent_count(model.nodes.size());
                std::vector<std::uint8_t> primitive_referenced(model.primitives.size());
                for (std::size_t node_index = 0U; node_index < model.nodes.size(); ++node_index)
                {
                    const auto& node = model.nodes[node_index];
                    if (!finiteAffine(node.local_transform)) return false;
                    for (std::size_t reference = 0U; reference < node.primitives.size(); ++reference)
                    {
                        const auto ordinal = node.primitives[reference];
                        if (ordinal >= model.primitives.size()) return false;
                        for (std::size_t previous = 0U; previous < reference; ++previous)
                            if (node.primitives[previous] == ordinal) return false;
                        primitive_referenced[ordinal] = 1U;
                    }
                    for (std::size_t child_index = 0U; child_index < node.children.size(); ++child_index)
                    {
                        const auto child = node.children[child_index];
                        if (child <= node_index || child >= model.nodes.size()) return false;
                        for (std::size_t previous = 0U; previous < child_index; ++previous)
                            if (node.children[previous] == child) return false;
                        if (++parent_count[child] != 1U) return false;
                    }
                }
                if (parent_count[model.root_node] != 0U) return false;
                for (std::size_t node = 1U; node < parent_count.size(); ++node)
                    if (parent_count[node] != 1U) return false;
                return std::all_of(
                    primitive_referenced.begin(),
                    primitive_referenced.end(),
                    [](std::uint8_t referenced) noexcept { return referenced != 0U; }
                );
            }
            catch (const std::bad_alloc&)
            {
                return false;
            }
        }

        [[nodiscard]] bool consumeBudget(
            std::size_t& consumed,
            std::size_t count,
            std::size_t stride,
            std::size_t limit
        ) noexcept
        {
            if (consumed > limit || (stride != 0U && count > (limit - consumed) / stride))
                return false;
            consumed += count * stride;
            return true;
        }

        [[nodiscard]] bool writeId(lux::serialization::BinaryWriter& writer, AssetId value) noexcept
        {
            return static_cast<bool>(writer.writeBytes(value.bytes()));
        }

        [[nodiscard]] bool readId(lux::serialization::BinaryReader& reader, AssetId& value) noexcept
        {
            std::array<std::uint8_t, 16U> bytes{};
            if (!reader.readBytes(std::as_writable_bytes(std::span(bytes))))
                return false;
            value = AssetId{bytes};
            return true;
        }

        [[nodiscard]] lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure> encodeModel(
            const lux::rdesc::ModelDescription& model,
            std::size_t max_encoded_bytes
        ) noexcept
        {
            if (!validModel(model))
                return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::INVALID_ASSET));
            try
            {
                std::vector<std::byte> bytes;
                lux::serialization::BinaryWriter writer(bytes);
                if (!writer.writeUnsigned(kModelVersion) || !writer.writeUnsigned(model.root_node) ||
                    !writer.writeUnsigned(static_cast<std::uint32_t>(model.primitives.size())))
                {
                    return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::ALLOCATION_FAILURE));
                }
                for (const auto& primitive : model.primitives)
                {
                    if (!writeId(writer, primitive.mesh) || !writeId(writer, primitive.material))
                        return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::ALLOCATION_FAILURE));
                }
                if (!writer.writeUnsigned(static_cast<std::uint32_t>(model.nodes.size())))
                    return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::ALLOCATION_FAILURE));
                for (const auto& node : model.nodes)
                {
                    for (Eigen::Index row = 0; row < 4; ++row)
                        for (Eigen::Index column = 0; column < 4; ++column)
                            if (!writer.writeFloat(node.local_transform.matrix()(row, column)))
                                return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::ALLOCATION_FAILURE));
                    if (!writer.writeUnsigned(static_cast<std::uint32_t>(node.primitives.size())))
                        return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::ALLOCATION_FAILURE));
                    for (const auto ordinal : node.primitives)
                        if (!writer.writeUnsigned(ordinal))
                            return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::ALLOCATION_FAILURE));
                    if (!writer.writeUnsigned(static_cast<std::uint32_t>(node.children.size())))
                        return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::ALLOCATION_FAILURE));
                    for (const auto ordinal : node.children)
                        if (!writer.writeUnsigned(ordinal))
                            return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::ALLOCATION_FAILURE));
                }
                const std::uint8_t has_skeleton = model.skeleton ? 1U : 0U;
                if (!writer.writeUnsigned(has_skeleton) || !writer.writeUnsigned(std::uint8_t{}) ||
                    !writer.writeUnsigned(std::uint8_t{}) || !writer.writeUnsigned(std::uint8_t{}) ||
                    !writeId(writer, model.skeleton.value_or(AssetId{})) ||
                    !writer.writeUnsigned(static_cast<std::uint32_t>(model.animations.size())))
                {
                    return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::ALLOCATION_FAILURE));
                }
                for (const auto animation : model.animations)
                    if (!writeId(writer, animation))
                        return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::ALLOCATION_FAILURE));
                if (bytes.size() > max_encoded_bytes)
                    return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::LIMIT_EXCEEDED));
                return bytes;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::ALLOCATION_FAILURE));
            }
        }

        [[nodiscard]] lux::cxx::expected<std::shared_ptr<const lux::rdesc::ModelDescription>, AssetDecodeFailure>
        decodeModel(std::span<const std::byte> bytes, std::size_t max_decoded_bytes) noexcept
        {
            if (bytes.empty() || bytes.size() > max_decoded_bytes ||
                sizeof(lux::rdesc::ModelDescription) > max_decoded_bytes)
            {
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::LIMIT_EXCEEDED));
            }
            try
            {
                lux::serialization::BinaryReader reader(bytes);
                auto version = reader.readUnsigned<std::uint32_t>();
                auto root = reader.readUnsigned<std::uint32_t>();
                auto primitive_count = reader.readUnsigned<std::uint32_t>();
                if (!version || !root || !primitive_count || *version != kModelVersion)
                    return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
                std::size_t consumed = sizeof(lux::rdesc::ModelDescription);
                if (!consumeBudget(consumed, *primitive_count, sizeof(lux::rdesc::ModelPrimitive), max_decoded_bytes))
                    return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::LIMIT_EXCEEDED));
                auto model = std::make_shared<lux::rdesc::ModelDescription>();
                model->root_node = *root;
                model->primitives.resize(*primitive_count);
                for (auto& primitive : model->primitives)
                {
                    if (!readId(reader, primitive.mesh) || !readId(reader, primitive.material))
                        return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
                }
                auto node_count = reader.readUnsigned<std::uint32_t>();
                if (!node_count ||
                    !consumeBudget(consumed, *node_count, sizeof(lux::rdesc::ModelNode), max_decoded_bytes))
                {
                    return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::LIMIT_EXCEEDED));
                }
                model->nodes.resize(*node_count);
                for (auto& node : model->nodes)
                {
                    for (Eigen::Index row = 0; row < 4; ++row)
                    {
                        for (Eigen::Index column = 0; column < 4; ++column)
                        {
                            auto value = reader.readFloat<float>();
                            if (!value)
                                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
                            node.local_transform.matrix()(row, column) = *value;
                        }
                    }
                    auto primitive_references = reader.readUnsigned<std::uint32_t>();
                    if (!primitive_references || !consumeBudget(
                            consumed,
                            *primitive_references,
                            sizeof(std::uint32_t),
                            max_decoded_bytes
                        ))
                    {
                        return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::LIMIT_EXCEEDED));
                    }
                    node.primitives.resize(*primitive_references);
                    for (auto& ordinal : node.primitives)
                    {
                        auto value = reader.readUnsigned<std::uint32_t>();
                        if (!value)
                            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
                        ordinal = *value;
                    }
                    auto child_references = reader.readUnsigned<std::uint32_t>();
                    if (!child_references || !consumeBudget(
                            consumed,
                            *child_references,
                            sizeof(std::uint32_t),
                            max_decoded_bytes
                        ))
                    {
                        return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::LIMIT_EXCEEDED));
                    }
                    node.children.resize(*child_references);
                    for (auto& ordinal : node.children)
                    {
                        auto value = reader.readUnsigned<std::uint32_t>();
                        if (!value)
                            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
                        ordinal = *value;
                    }
                }
                auto has_skeleton = reader.readUnsigned<std::uint8_t>();
                auto reserved0 = reader.readUnsigned<std::uint8_t>();
                auto reserved1 = reader.readUnsigned<std::uint8_t>();
                auto reserved2 = reader.readUnsigned<std::uint8_t>();
                AssetId skeleton;
                if (!has_skeleton || !reserved0 || !reserved1 || !reserved2 || *has_skeleton > 1U ||
                    *reserved0 != 0U || *reserved1 != 0U || *reserved2 != 0U || !readId(reader, skeleton))
                {
                    return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
                }
                if (*has_skeleton != 0U)
                    model->skeleton = skeleton;
                else if (!skeleton.isNull())
                    return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
                auto animation_count = reader.readUnsigned<std::uint32_t>();
                if (!animation_count || !consumeBudget(
                        consumed,
                        *animation_count,
                        sizeof(AssetId),
                        max_decoded_bytes
                    ))
                {
                    return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::LIMIT_EXCEEDED));
                }
                model->animations.resize(*animation_count);
                for (auto& animation : model->animations)
                    if (!readId(reader, animation))
                        return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
                if (reader.remaining() != 0U || !validModel(*model))
                    return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
                return model;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::ALLOCATION_FAILURE));
            }
        }
    } // namespace

    ModelAsset::ModelAsset(
        AssetInfo info,
        std::shared_ptr<const lux::rdesc::ModelDescription> data,
        std::vector<AssetAuxiliaryPayload> auxiliary
    ) noexcept
        : TAsset(std::move(info), std::move(data), std::move(auxiliary))
    {
    }

    lux::cxx::expected<std::shared_ptr<const ModelAsset>, AssetDecodeFailure> ModelAsset::create(
        AssetInfo info,
        std::shared_ptr<const lux::rdesc::ModelDescription> data,
        std::vector<AssetAuxiliaryPayload> auxiliary
    ) noexcept
    {
        if (info.id.isNull() || !data || !validModel(*data) || !canonicalize(auxiliary))
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
        info.type = asset_type;
        try
        {
            return std::shared_ptr<const ModelAsset>(
                new ModelAsset(std::move(info), std::move(data), std::move(auxiliary))
            );
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::ALLOCATION_FAILURE));
        }
    }

    lux::cxx::expected<std::shared_ptr<const ModelAsset>, AssetDecodeFailure>
    TAssetSerDeser<ModelAsset>::decode(
        AssetId requested,
        lux::cxx::SharedBytes<> bytes,
        const AssetDecodeLimits& limits
    ) noexcept
    {
        auto image = inspectCookedAssetImage(requested, std::move(bytes), limits);
        if (!image) return lux::cxx::unexpected(image.error());
        if (image->magic() != ModelAsset::primary_magic)
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_MAGIC));
        if (image->metadata().legacy_type_tag != ModelAsset::legacy_type_tag)
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_TYPE));
        if (!image->data().empty() || image->information().empty())
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_LAYOUT));
        auto model = decodeModel(image->information().view(), limits.max_decoded_bytes);
        if (!model) return lux::cxx::unexpected(model.error());
        try
        {
            std::vector<AssetAuxiliaryPayload> auxiliary(
                image->auxiliaryPayloads().begin(), image->auxiliaryPayloads().end()
            );
            return ModelAsset::create(
                AssetInfo{
                    image->metadata().id,
                    ModelAsset::asset_type,
                    image->metadata().date,
                    image->metadata().display_name,
                    image->metadata().source_path,
                    image->metadata().source_mtime
                },
                std::move(*model),
                std::move(auxiliary)
            );
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::ALLOCATION_FAILURE));
        }
    }

    lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure>
    TAssetSerDeser<ModelAsset>::encode(const ModelAsset& asset, const AssetEncodeLimits& limits) noexcept
    {
        auto information = encodeModel(asset.data(), limits.max_encoded_bytes);
        if (!information) return lux::cxx::unexpected(information.error());
        return detail::encodeCookedAssetImage(
            detail::CookedAssetWriteRequest{
                ModelAsset::primary_magic,
                ModelAsset::legacy_type_tag,
                asset.info(),
                *information,
                {},
                asset.auxiliaryPayloads()
            },
            limits
        );
    }
} // namespace lux::asset
