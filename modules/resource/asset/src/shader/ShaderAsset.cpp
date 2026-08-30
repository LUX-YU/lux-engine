#include <lux/engine/resource/asset/shader/ShaderAsset.hpp>

#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/resource/asset/detail/CookedAssetWriter.hpp>
#include <lux/engine/resource/asset/detail/ShaderValidation.hpp>

#include <algorithm>
#include <cstring>
#include <new>
#include <string>
#include <utility>

namespace lux::asset
{
    namespace
    {
        [[nodiscard]] AssetDecodeFailure decodeFailure(
            EAssetDecodeError code,
            std::size_t offset = 0U
        ) noexcept
        {
            return AssetDecodeFailure{code, offset};
        }

        [[nodiscard]] AssetEncodeFailure encodeFailure(
            EAssetEncodeError code,
            std::size_t offset = 0U
        ) noexcept
        {
            return AssetEncodeFailure{code, offset};
        }

        [[nodiscard]] bool validSpirv(const lux::rdesc::Shader& shader) noexcept
        {
            if (shader.data() == nullptr) return false;
            return detail::validSpirvBytes({
                static_cast<const std::byte*>(shader.data()),
                shader.size()
            });
        }

        [[nodiscard]] bool canonicalizeAuxiliary(
            std::vector<AssetAuxiliaryPayload>& auxiliary
        ) noexcept
        {
            std::sort(
                auxiliary.begin(),
                auxiliary.end(),
                [](const AssetAuxiliaryPayload& left, const AssetAuxiliaryPayload& right) noexcept {
                    return left.tag < right.tag;
                }
            );
            for (std::size_t index = 0U; index < auxiliary.size(); ++index)
            {
                const bool invalid = auxiliary[index].tag == 0U || auxiliary[index].bytes.empty();
                const bool duplicate = index != 0U && auxiliary[index - 1U].tag == auxiliary[index].tag;
                if (invalid || duplicate)
                    return false;
            }
            return true;
        }
    } // namespace

    ShaderAsset::ShaderAsset(
        AssetInfo info,
        std::shared_ptr<const ShaderAssetData> data,
        std::vector<AssetAuxiliaryPayload> auxiliary
    ) noexcept
        : TAsset(std::move(info), std::move(data), std::move(auxiliary))
    {
    }

    lux::cxx::expected<std::shared_ptr<const ShaderAsset>, AssetDecodeFailure> ShaderAsset::create(
        AssetInfo info,
        std::shared_ptr<const ShaderAssetData> data,
        std::vector<AssetAuxiliaryPayload> auxiliary
    ) noexcept
    {
        if (info.id.isNull() || !data || !validSpirv(data->shader))
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
        info.type = asset_type;
        try
        {
            if (!canonicalizeAuxiliary(auxiliary))
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
            return std::shared_ptr<const ShaderAsset>(
                new ShaderAsset(std::move(info), std::move(data), std::move(auxiliary))
            );
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::ALLOCATION_FAILURE));
        }
        catch (...)
        {
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
        }
    }

    lux::cxx::expected<std::shared_ptr<const ShaderAsset>, AssetDecodeFailure>
    TAssetSerDeser<ShaderAsset>::decode(
        AssetId requested,
        lux::cxx::SharedBytes<> cooked_image,
        const AssetDecodeLimits& limits
    ) noexcept
    {
        auto image = inspectCookedAssetImage(requested, std::move(cooked_image), limits);
        if (!image)
            return lux::cxx::unexpected(image.error());
        if (image->magic() != ShaderAsset::primary_magic)
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_MAGIC));
        if (image->metadata().legacy_type_tag != ShaderAsset::legacy_type_tag)
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_TYPE));
        try
        {
            lux::rdesc::ShaderInfo shader_info{};
            std::string error;
            if (!lux::rdesc::ShaderInfo::deserialize(image->information().view(), shader_info, &error))
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
            auto data = std::make_shared<const ShaderAssetData>(ShaderAssetData{
                lux::rdesc::Shader{image->data().data(), image->data().size()},
                std::move(shader_info)
            });
            std::vector<AssetAuxiliaryPayload> auxiliary(
                image->auxiliaryPayloads().begin(),
                image->auxiliaryPayloads().end()
            );
            return ShaderAsset::create(
                AssetInfo{
                    image->metadata().id,
                    ShaderAsset::asset_type,
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
        catch (...)
        {
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
        }
    }

    lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure> TAssetSerDeser<ShaderAsset>::encode(
        const ShaderAsset& asset,
        const AssetEncodeLimits& limits
    ) noexcept
    {
        if (asset.id().isNull() || asset.type() != ShaderAsset::asset_type || !validSpirv(asset.data().shader))
            return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::INVALID_ASSET));
        try
        {
            const auto information = lux::rdesc::ShaderInfo::serialize(asset.data().info);
            if (information.empty())
                return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::INVALID_PAYLOAD));
            const auto shader = std::span<const std::byte>{
                static_cast<const std::byte*>(asset.data().shader.data()),
                asset.data().shader.size()
            };
            return detail::encodeCookedAssetImage(
                detail::CookedAssetWriteRequest{
                    ShaderAsset::primary_magic,
                    ShaderAsset::legacy_type_tag,
                    asset.info(),
                    information,
                    shader,
                    asset.auxiliaryPayloads()
                },
                limits
            );
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::ALLOCATION_FAILURE));
        }
        catch (...)
        {
            return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::INVALID_PAYLOAD));
        }
    }
} // namespace lux::asset
