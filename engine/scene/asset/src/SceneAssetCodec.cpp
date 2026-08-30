#include <lux/engine/scene/SceneAssetCodec.hpp>

#include <lux/engine/resource/asset/AssetCodecSet.hpp>
#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/resource/asset/detail/CookedAssetWriter.hpp>
#include <lux/engine/serialization/BinaryReader.hpp>
#include <lux/engine/serialization/BinaryWriter.hpp>

#include <array>
#include <memory>
#include <new>
#include <span>
#include <utility>
#include <vector>

namespace lux::scene
{
    namespace detail
    {
        inline constexpr std::uint32_t kVersion = 1U;
        inline constexpr std::size_t kWireSize = 40U;

        [[nodiscard]] lux::cxx::expected<std::vector<std::byte>, lux::asset::EAssetCodecError> encode(
            const SceneDescription& scene,
            std::size_t max_encoded_bytes
        ) noexcept
        {
            if (max_encoded_bytes < kWireSize)
                return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
            if (scene.world.isNull() || scene.simulation.isNull())
                return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
            try
            {
                std::vector<std::byte> bytes;
                bytes.reserve(kWireSize);
                lux::serialization::BinaryWriter writer(bytes);
                if (!writer.writeUnsigned(SceneAssetPrimaryMagic) ||
                    !writer.writeUnsigned(kVersion) ||
                    !writer.writeBytes(scene.world.bytes()) ||
                    !writer.writeBytes(scene.simulation.bytes()))
                {
                    return lux::cxx::unexpected(lux::asset::EAssetCodecError::OUT_OF_MEMORY);
                }
                return bytes;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(lux::asset::EAssetCodecError::OUT_OF_MEMORY);
            }
        }

        [[nodiscard]] lux::cxx::expected<std::shared_ptr<const SceneDescription>, lux::asset::EAssetCodecError>
        decode(
            std::span<const std::byte> input,
            std::size_t max_input_bytes,
            std::size_t max_decoded_bytes
        ) noexcept
        {
            if (input.size() != kWireSize || input.size() > max_input_bytes ||
                sizeof(SceneDescription) > max_decoded_bytes)
            {
                return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
            }
            lux::serialization::BinaryReader reader(input);
            auto magic = reader.readUnsigned<std::uint32_t>();
            auto version = reader.readUnsigned<std::uint32_t>();
            std::array<std::uint8_t, 16U> world{};
            std::array<std::uint8_t, 16U> simulation{};
            if (!magic || !version || *magic != SceneAssetPrimaryMagic || *version != kVersion ||
                !reader.readBytes(std::as_writable_bytes(std::span(world))) ||
                !reader.readBytes(std::as_writable_bytes(std::span(simulation))) ||
                reader.remaining() != 0U)
            {
                return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
            }
            auto scene = std::make_shared<SceneDescription>(
                SceneDescription{lux::asset::AssetId(world), lux::asset::AssetId(simulation)}
            );
            if (scene->world.isNull() || scene->simulation.isNull())
                return lux::cxx::unexpected(lux::asset::EAssetCodecError::CODEC_FAILURE);
            return scene;
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

    lux::cxx::expected<std::shared_ptr<const SceneAsset>, lux::asset::AssetDecodeFailure>
    SceneAsset::create(
        lux::asset::AssetInfo info,
        std::shared_ptr<const SceneDescription> data,
        std::vector<lux::asset::AssetAuxiliaryPayload> auxiliary
    ) noexcept
    {
        const bool invalid = !data || data->world.isNull() || data->simulation.isNull();
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
            std::sort(
                auxiliary.begin(),
                auxiliary.end(),
                [](const auto& left, const auto& right) noexcept { return left.tag < right.tag; }
            );
            for (std::size_t index = 0U; index < auxiliary.size(); ++index)
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
        if (!image)
            return lux::cxx::unexpected(image.error());
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
#include <algorithm>
