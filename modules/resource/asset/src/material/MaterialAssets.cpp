#include <lux/engine/resource/asset/material/MaterialAssets.hpp>

#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <lux/engine/resource/asset/detail/CookedAssetWriter.hpp>
#include <lux/engine/resource/asset/detail/ShaderValidation.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace lux::asset
{
    namespace
    {
        inline constexpr std::uint32_t kMaterialVersion = 7U; // Cooked shaders use separate texture/sampler arrays.
        inline constexpr std::uint32_t kMaterialInstanceVersion = 2U;

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
            std::sort(
                values.begin(),
                values.end(),
                [](const auto& left, const auto& right) noexcept { return left.tag < right.tag; }
            );
            for (std::size_t index = 0U; index < values.size(); ++index)
            {
                if (values[index].tag == 0U || values[index].bytes.empty() ||
                    (index != 0U && values[index - 1U].tag == values[index].tag))
                {
                    return false;
                }
            }
            return true;
        }

        [[nodiscard]] bool hasCookedLayouts(const lux::rdesc::ShaderInfo& info) noexcept
        {
            if (info.entry_points.empty())
            {
                return false;
            }
            for (const auto& set : info.sets)
            {
                for (const auto& binding : set.bindings)
                {
                    if (binding.name == "uMats" && (!binding.block.runtime_records || binding.block.fields.empty()))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        [[nodiscard]] bool validMaterial(const lux::rdesc::MaterialDescription& data) noexcept
        {
            const auto alpha_mode = static_cast<std::uint8_t>(data.alpha_mode);
            if (data.parameter_defaults.size() > UINT32_MAX || data.texture_slot_ids.size() > UINT32_MAX ||
                alpha_mode > static_cast<std::uint8_t>(lux::rdesc::EAlphaMode::BLEND) ||
                !detail::validSpirvWords(data.gbuffer_spirv) || !detail::validSpirvWords(data.forward_spirv) ||
                !hasCookedLayouts(data.gbuffer_info) || !hasCookedLayouts(data.forward_info))
            {
                return false;
            }
            for (std::size_t parameter = 0U; parameter < data.parameter_defaults.size(); ++parameter)
            {
                for (const float value : data.parameter_defaults[parameter])
                {
                    if (!std::isfinite(value))
                    {
                        return false;
                    }
                }
            }
            return true;
        }

        [[nodiscard]] bool validMaterialInstance(const lux::rdesc::MaterialInstanceDescription& data) noexcept
        {
            const auto alpha_mode = static_cast<std::uint8_t>(data.alpha_mode);
            const bool invalid_count = data.parameters.size() > UINT32_MAX || data.textures.size() > UINT32_MAX;
            const bool invalid_state = data.parent_material_id.isNull() || data.render_state_override > 1U ||
                                       alpha_mode > static_cast<std::uint8_t>(lux::rdesc::EAlphaMode::BLEND);
            if (invalid_count || invalid_state)
            {
                return false;
            }
            for (std::size_t i = 0; i < data.parameters.size(); ++i)
            {
                const auto& entry = data.parameters[i];
                if (i && data.parameters[i - 1].slot >= entry.slot)
                {
                    return false;
                }
                for (float value : entry.value)
                {
                    if (!std::isfinite(value))
                    {
                        return false;
                    }
                }
            }
            for (std::size_t i = 0; i < data.textures.size(); ++i)
            {
                const auto& entry = data.textures[i];
                if (i && data.textures[i - 1].slot >= entry.slot)
                {
                    return false;
                }
            }
            return true;
        }

        struct Writer final
        {
            std::vector<std::byte> bytes;

            template <class Type> void pod(const Type& value)
            {
                const auto offset = bytes.size();
                bytes.resize(offset + sizeof(Type));
                std::memcpy(bytes.data() + offset, &value, sizeof(Type));
            }

            void raw(const void* source, std::size_t size)
            {
                if (size == 0U)
                {
                    return;
                }
                const auto* first = static_cast<const std::byte*>(source);
                bytes.insert(bytes.end(), first, first + size);
            }

            void id(AssetId value)
            {
                const auto raw = value.bytes();
                rawBytes(raw);
            }

            void rawBytes(std::span<const std::byte> value)
            {
                bytes.insert(bytes.end(), value.begin(), value.end());
            }
        };

        struct Reader final
        {
            std::span<const std::byte> bytes;
            std::size_t offset{};

            template <class Type> [[nodiscard]] bool pod(Type& value) noexcept
            {
                if (sizeof(Type) > bytes.size() - offset)
                {
                    return false;
                }
                std::memcpy(&value, bytes.data() + offset, sizeof(Type));
                offset += sizeof(Type);
                return true;
            }

            [[nodiscard]] bool id(AssetId& value) noexcept
            {
                if (16U > bytes.size() - offset)
                {
                    return false;
                }
                std::array<std::uint8_t, 16U> raw{};
                for (std::size_t index = 0U; index < raw.size(); ++index)
                {
                    raw[index] = std::to_integer<std::uint8_t>(bytes[offset + index]);
                }
                offset += raw.size();
                value = AssetId{raw};
                return true;
            }

            [[nodiscard]] bool words(std::vector<std::uint32_t>& values) noexcept
            {
                std::uint32_t count{};
                if (!pod(count) || count > (bytes.size() - offset) / sizeof(std::uint32_t))
                {
                    return false;
                }
                values.resize(count);
                const auto size = static_cast<std::size_t>(count) * sizeof(std::uint32_t);
                if (size != 0U)
                {
                    std::memcpy(values.data(), bytes.data() + offset, size);
                }
                offset += size;
                return true;
            }

            [[nodiscard]] bool block(std::vector<std::byte>& values) noexcept
            {
                std::uint32_t size{};
                if (!pod(size) || size > bytes.size() - offset)
                {
                    return false;
                }
                values.assign(bytes.begin() + offset, bytes.begin() + offset + size);
                offset += size;
                return true;
            }
        };

        template <class Asset>
        [[nodiscard]] lux::cxx::expected<CookedAssetImage, AssetDecodeFailure> inspect(
            AssetId requested,
            lux::cxx::SharedBytes<> bytes,
            const AssetDecodeLimits& limits
        ) noexcept
        {
            auto image = inspectCookedAssetImage(requested, std::move(bytes), limits);
            if (!image)
            {
                return lux::cxx::unexpected(image.error());
            }
            if (image->magic() != Asset::primary_magic)
            {
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_MAGIC));
            }
            if (image->metadata().legacy_type_tag != Asset::legacy_type_tag)
            {
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_TYPE));
            }
            if (!image->data().empty() || image->information().empty())
            {
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_LAYOUT));
            }
            return image;
        }

        template <class Asset>
        [[nodiscard]] lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure> wrap(
            const Asset& asset,
            std::vector<std::byte> information,
            const AssetEncodeLimits& limits
        ) noexcept
        {
            return detail::encodeCookedAssetImage(
                detail::CookedAssetWriteRequest{
                    Asset::primary_magic,
                    Asset::legacy_type_tag,
                    asset.info(),
                    information,
                    {},
                    asset.auxiliaryPayloads()
                },
                limits
            );
        }
    } // namespace

    MaterialAsset::MaterialAsset(
        AssetInfo info,
        std::shared_ptr<const lux::rdesc::MaterialDescription> data,
        std::vector<AssetAuxiliaryPayload> auxiliary
    ) noexcept
        : TAsset(std::move(info), std::move(data), std::move(auxiliary))
    {
    }

    lux::cxx::expected<std::shared_ptr<const MaterialAsset>, AssetDecodeFailure> MaterialAsset::create(
        AssetInfo info,
        std::shared_ptr<const lux::rdesc::MaterialDescription> data,
        std::vector<AssetAuxiliaryPayload> auxiliary
    ) noexcept
    {
        if (info.id.isNull() || !data || !validMaterial(*data) || !canonicalize(auxiliary))
        {
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
        }
        info.type = asset_type;
        {
            return std::shared_ptr<const MaterialAsset>(
                new MaterialAsset(std::move(info), std::move(data), std::move(auxiliary))
            );
        }
    }

    MaterialInstanceAsset::MaterialInstanceAsset(
        AssetInfo info,
        std::shared_ptr<const lux::rdesc::MaterialInstanceDescription> data,
        std::vector<AssetAuxiliaryPayload> auxiliary
    ) noexcept
        : TAsset(std::move(info), std::move(data), std::move(auxiliary))
    {
    }

    lux::cxx::expected<std::shared_ptr<const MaterialInstanceAsset>, AssetDecodeFailure> MaterialInstanceAsset::create(
        AssetInfo info,
        std::shared_ptr<const lux::rdesc::MaterialInstanceDescription> data,
        std::vector<AssetAuxiliaryPayload> auxiliary
    ) noexcept
    {
        if (info.id.isNull() || !data || !validMaterialInstance(*data) || !canonicalize(auxiliary))
        {
            return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
        }
        info.type = asset_type;
        {
            return std::shared_ptr<const MaterialInstanceAsset>(
                new MaterialInstanceAsset(std::move(info), std::move(data), std::move(auxiliary))
            );
        }
    }

    lux::cxx::expected<std::shared_ptr<const MaterialAsset>, AssetDecodeFailure> TAssetSerDeser<MaterialAsset>::decode(
        AssetId requested,
        lux::cxx::SharedBytes<> bytes,
        const AssetDecodeLimits& limits
    ) noexcept
    {
        auto image = inspect<MaterialAsset>(requested, std::move(bytes), limits);
        if (!image)
        {
            return lux::cxx::unexpected(image.error());
        }
        {
            Reader reader{image->information().view()};
            auto data = std::make_shared<lux::rdesc::MaterialDescription>();
            std::uint32_t version{}, parameter_count{}, texture_count{}, alpha_mode{};
            std::uint8_t double_sided{};
            if (!reader.pod(version) || version != kMaterialVersion || !reader.pod(parameter_count) ||
                parameter_count > (reader.bytes.size() - reader.offset) / sizeof(std::array<float, 4>))
            {
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
            }
            data->parameter_defaults.resize(parameter_count);
            for (auto& parameter : data->parameter_defaults)
            {
                for (auto& value : parameter)
                {
                    if (!reader.pod(value))
                    {
                        return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
                    }
                }
            }
            if (!reader.pod(alpha_mode) || alpha_mode > static_cast<std::uint32_t>(lux::rdesc::EAlphaMode::BLEND) ||
                !reader.pod(double_sided) || double_sided > 1U || !reader.pod(texture_count) ||
                texture_count > (reader.bytes.size() - reader.offset) / 16)
            {
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
            }
            data->alpha_mode = static_cast<lux::rdesc::EAlphaMode>(alpha_mode);
            data->double_sided = double_sided != 0U;
            data->texture_slot_ids.resize(texture_count);
            for (auto& id : data->texture_slot_ids)
            {
                if (!reader.id(id))
                {
                    return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
                }
            }
            std::vector<std::byte> gbuffer_info;
            std::vector<std::byte> forward_info;
            std::string error;
            if (!reader.words(data->gbuffer_spirv) || !reader.block(gbuffer_info) ||
                (!gbuffer_info.empty() && !lux::rdesc::ShaderInfo::deserialize(gbuffer_info, data->gbuffer_info, &error)
                ) ||
                !reader.words(data->forward_spirv) || !reader.block(forward_info) ||
                (!forward_info.empty() && !lux::rdesc::ShaderInfo::deserialize(forward_info, data->forward_info, &error)
                ) ||
                reader.offset != reader.bytes.size() || !validMaterial(*data))
            {
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
            }
            std::vector<AssetAuxiliaryPayload> auxiliary(
                image->auxiliaryPayloads().begin(),
                image->auxiliaryPayloads().end()
            );
            return MaterialAsset::create(
                AssetInfo{
                    image->metadata().id,
                    MaterialAsset::asset_type,
                    image->metadata().date,
                    image->metadata().display_name,
                    image->metadata().source_path,
                    image->metadata().source_mtime
                },
                std::move(data),
                std::move(auxiliary)
            );
        }
    }

    lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure> TAssetSerDeser<MaterialAsset>::encode(
        const MaterialAsset& asset,
        const AssetEncodeLimits& limits
    ) noexcept
    {
        if (!validMaterial(asset.data()))
        {
            return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::INVALID_ASSET));
        }
        {
            const auto gbuffer_info = lux::rdesc::ShaderInfo::serialize(asset.data().gbuffer_info);
            const auto forward_info = lux::rdesc::ShaderInfo::serialize(asset.data().forward_info);
            Writer writer;
            writer.pod(kMaterialVersion);
            writer.pod(static_cast<std::uint32_t>(asset.data().parameter_defaults.size()));
            for (std::size_t parameter = 0U; parameter < asset.data().parameter_defaults.size(); ++parameter)
            {
                for (const float value : asset.data().parameter_defaults[parameter])
                {
                    writer.pod(value);
                }
            }
            writer.pod(static_cast<std::uint32_t>(asset.data().alpha_mode));
            writer.pod(static_cast<std::uint8_t>(asset.data().double_sided ? 1U : 0U));
            writer.pod(static_cast<std::uint32_t>(asset.data().texture_slot_ids.size()));
            for (const auto id : asset.data().texture_slot_ids)
            {
                writer.id(id);
            }
            writer.pod(static_cast<std::uint32_t>(asset.data().gbuffer_spirv.size()));
            writer.raw(asset.data().gbuffer_spirv.data(), asset.data().gbuffer_spirv.size() * sizeof(std::uint32_t));
            writer.pod(static_cast<std::uint32_t>(gbuffer_info.size()));
            writer.raw(gbuffer_info.data(), gbuffer_info.size());
            writer.pod(static_cast<std::uint32_t>(asset.data().forward_spirv.size()));
            writer.raw(asset.data().forward_spirv.data(), asset.data().forward_spirv.size() * sizeof(std::uint32_t));
            writer.pod(static_cast<std::uint32_t>(forward_info.size()));
            writer.raw(forward_info.data(), forward_info.size());
            return wrap(asset, std::move(writer.bytes), limits);
        }
    }

    lux::cxx::expected<std::shared_ptr<const MaterialInstanceAsset>, AssetDecodeFailure> TAssetSerDeser<
        MaterialInstanceAsset>::
        decode(AssetId requested, lux::cxx::SharedBytes<> bytes, const AssetDecodeLimits& limits) noexcept
    {
        auto image = inspect<MaterialInstanceAsset>(requested, std::move(bytes), limits);
        if (!image)
        {
            return lux::cxx::unexpected(image.error());
        }
        {
            Reader reader{image->information().view()};
            auto data = std::make_shared<lux::rdesc::MaterialInstanceDescription>();
            std::uint32_t version{}, double_sided{}, alpha_mode{}, count{};
            if (!reader.pod(version) || version != kMaterialInstanceVersion || !reader.id(data->parent_material_id) ||
                !reader.pod(count) || count > (reader.bytes.size() - reader.offset) / 20)
            {
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
            }
            data->parameters.resize(count);
            for (auto& entry : data->parameters)
            {
                if (!reader.pod(entry.slot))
                {
                    return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
                }
                for (auto& value : entry.value)
                {
                    if (!reader.pod(value))
                    {
                        return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
                    }
                }
            }
            if (!reader.pod(count) || count > (reader.bytes.size() - reader.offset) / 20)
            {
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
            }
            data->textures.resize(count);
            for (auto& entry : data->textures)
            {
                if (!reader.pod(entry.slot) || !reader.id(entry.texture))
                {
                    return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
                }
            }
            if (!reader.pod(data->render_state_override) || !reader.pod(alpha_mode) ||
                alpha_mode > static_cast<std::uint32_t>(lux::rdesc::EAlphaMode::BLEND) || !reader.pod(double_sided) ||
                double_sided > 1U || reader.offset != reader.bytes.size())
            {
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
            }
            data->alpha_mode = static_cast<lux::rdesc::EAlphaMode>(alpha_mode);
            data->double_sided = double_sided != 0U;
            if (!validMaterialInstance(*data))
            {
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
            }
            std::vector<AssetAuxiliaryPayload> auxiliary(
                image->auxiliaryPayloads().begin(),
                image->auxiliaryPayloads().end()
            );
            return MaterialInstanceAsset::create(
                AssetInfo{
                    image->metadata().id,
                    MaterialInstanceAsset::asset_type,
                    image->metadata().date,
                    image->metadata().display_name,
                    image->metadata().source_path,
                    image->metadata().source_mtime
                },
                std::move(data),
                std::move(auxiliary)
            );
        }
    }

    lux::cxx::expected<std::vector<std::byte>, AssetEncodeFailure> TAssetSerDeser<MaterialInstanceAsset>::encode(
        const MaterialInstanceAsset& asset,
        const AssetEncodeLimits& limits
    ) noexcept
    {
        if (!validMaterialInstance(asset.data()))
        {
            return lux::cxx::unexpected(encodeFailure(EAssetEncodeError::INVALID_ASSET));
        }
        {
            Writer writer;
            writer.pod(kMaterialInstanceVersion);
            writer.id(asset.data().parent_material_id);
            writer.pod(static_cast<std::uint32_t>(asset.data().parameters.size()));
            for (const auto& entry : asset.data().parameters)
            {
                writer.pod(entry.slot);
                for (float value : entry.value)
                {
                    writer.pod(value);
                }
            }
            writer.pod(static_cast<std::uint32_t>(asset.data().textures.size()));
            for (const auto& entry : asset.data().textures)
            {
                writer.pod(entry.slot);
                writer.id(entry.texture);
            }
            writer.pod(asset.data().render_state_override);
            writer.pod(static_cast<std::uint32_t>(asset.data().alpha_mode));
            writer.pod(static_cast<std::uint32_t>(asset.data().double_sided ? 1U : 0U));
            return wrap(asset, std::move(writer.bytes), limits);
        }
    }
} // namespace lux::asset
