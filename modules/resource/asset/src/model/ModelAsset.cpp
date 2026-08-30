#include <lux/engine/resource/asset/model/ModelAsset.hpp>

#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/resource/asset/detail/CookedAssetWriter.hpp>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <new>
#include <utility>

namespace lux::asset
{
    namespace
    {
        inline constexpr std::uint32_t kModelVersion = 1U;

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
                if (values[index].tag == 0U || values[index].bytes.empty() ||
                    (index != 0U && values[index - 1U].tag == values[index].tag)) return false;
            return true;
        }

        [[nodiscard]] bool validIds(std::span<const AssetId> ids) noexcept
        {
            return std::all_of(ids.begin(), ids.end(), [](AssetId id) noexcept { return !id.isNull(); });
        }

        [[nodiscard]] bool validModel(const ModelAssetData& model) noexcept
        {
            const bool valid_counts = model.mesh_assets.size() <= std::numeric_limits<std::uint32_t>::max() &&
                model.material_assets.size() <= std::numeric_limits<std::uint32_t>::max() &&
                model.animation_assets.size() <= std::numeric_limits<std::uint32_t>::max();
            return valid_counts && validIds(model.mesh_assets) && validIds(model.material_assets) &&
                validIds(model.animation_assets) && (!model.skeleton_asset || !model.skeleton_asset->isNull());
        }

        template <class Type>
        void append(std::vector<std::byte>& output, const Type& value)
        {
            const auto offset = output.size();
            output.resize(offset + sizeof(Type));
            std::memcpy(output.data() + offset, &value, sizeof(Type));
        }

        void appendId(std::vector<std::byte>& output, AssetId id)
        {
            const auto bytes = id.bytes();
            output.insert(output.end(), bytes.begin(), bytes.end());
        }

        void appendIds(std::vector<std::byte>& output, std::span<const AssetId> ids)
        {
            append(output, static_cast<std::uint32_t>(ids.size()));
            for (const auto id : ids) appendId(output, id);
        }

        struct Reader final
        {
            std::span<const std::byte> bytes;
            std::size_t offset{};

            template <class Type>
            [[nodiscard]] bool read(Type& value) noexcept
            {
                if (sizeof(Type) > bytes.size() - offset) return false;
                std::memcpy(&value, bytes.data() + offset, sizeof(Type));
                offset += sizeof(Type);
                return true;
            }

            [[nodiscard]] bool readId(AssetId& value) noexcept
            {
                if (16U > bytes.size() - offset) return false;
                std::array<std::uint8_t, 16U> raw{};
                for (std::size_t index = 0U; index < raw.size(); ++index)
                    raw[index] = std::to_integer<std::uint8_t>(bytes[offset + index]);
                offset += raw.size();
                value = AssetId{raw};
                return true;
            }

            [[nodiscard]] bool readIds(std::vector<AssetId>& values, std::size_t limit) noexcept
            {
                std::uint32_t count{};
                if (!read(count) || count > (bytes.size() - offset) / 16U || count > limit / sizeof(AssetId))
                    return false;
                values.resize(count);
                for (auto& value : values) if (!readId(value) || value.isNull()) return false;
                return true;
            }
        };
    } // namespace

    ModelAsset::ModelAsset(
        AssetInfo info,
        std::shared_ptr<const ModelAssetData> data,
        std::vector<AssetAuxiliaryPayload> auxiliary
    ) noexcept
        : TAsset(std::move(info), std::move(data), std::move(auxiliary))
    {
    }

    lux::cxx::expected<std::shared_ptr<const ModelAsset>, AssetDecodeFailure> ModelAsset::create(
        AssetInfo info,
        std::shared_ptr<const ModelAssetData> data,
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
        try
        {
            Reader reader{image->information().view()};
            auto data = std::make_shared<ModelAssetData>();
            std::uint32_t version{};
            std::uint8_t has_skeleton{};
            AssetId skeleton;
            std::vector<AssetId> retired_textures;
            if (!reader.read(version) || version != kModelVersion ||
                !reader.read(has_skeleton) || has_skeleton > 1U || !reader.readId(skeleton) ||
                !reader.readIds(data->mesh_assets, limits.max_decoded_bytes) ||
                !reader.readIds(data->material_assets, limits.max_decoded_bytes) ||
                !reader.readIds(retired_textures, limits.max_decoded_bytes) ||
                !retired_textures.empty() ||
                !reader.readIds(data->animation_assets, limits.max_decoded_bytes) ||
                reader.offset != reader.bytes.size())
            {
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
            }
            if (has_skeleton != 0U)
            {
                if (skeleton.isNull())
                    return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
                data->skeleton_asset = skeleton;
            }
            else if (!skeleton.isNull())
            {
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
            }
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
                std::move(data),
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
        if (!validModel(asset.data()))
            return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::INVALID_ASSET));
        try
        {
            std::vector<std::byte> information;
            append(information, kModelVersion);
            append(information, asset.data().skeleton_asset ? std::uint8_t{1U} : std::uint8_t{0U});
            appendId(information, asset.data().skeleton_asset.value_or(AssetId{}));
            appendIds(information, asset.data().mesh_assets);
            appendIds(information, asset.data().material_assets);
            appendIds(information, {});
            appendIds(information, asset.data().animation_assets);
            return detail::encodeCookedAssetImage(
                detail::CookedAssetWriteRequest{
                    ModelAsset::primary_magic,
                    ModelAsset::legacy_type_tag,
                    asset.info(),
                    information,
                    {},
                    asset.auxiliaryPayloads()
                },
                limits
            );
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::ALLOCATION_FAILURE));
        }
    }
} // namespace lux::asset
