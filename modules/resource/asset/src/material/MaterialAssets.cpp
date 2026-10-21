#include <lux/engine/description/MaterialLayout.hpp>
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
        inline constexpr std::uint32_t kMaterialVersion =
            9U; // Cooked use collection, shared record layout and stage-interface ABI.
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

        [[nodiscard]] bool hasCookedLayouts(const lux::rdesc::ShaderInfo& info, std::size_t texture_count) noexcept
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
                    if (binding.name == rdesc::kMaterialTableName)
                    {
                        for (std::size_t index = 0; index < texture_count; ++index)
                        {
                            const auto path = std::string(rdesc::kMaterialTexturesName) + "[" + std::to_string(index) +
                                              "].sampler_index";
                            const auto field =
                                std::ranges::find(binding.block.fields, path, &rdesc::ShaderMember::path);
                            if (field == binding.block.fields.end() || field->scalar != rdesc::EDataScalar::UINT)
                            {
                                return false;
                            }
                        }
                    }
                }
            }
            return true;
        }

        [[nodiscard]] bool validMaterial(const lux::rdesc::MaterialDescription& data) noexcept
        {
            const auto& programs = data.programs;
            const bool invalid_counts = programs.parameter_defaults.size() > UINT32_MAX ||
                                        data.texture_slot_ids.size() != programs.texture_count ||
                                        programs.programs.empty();
            const bool invalid_state = programs.alpha_mode > rdesc::EAlphaMode::MASK ||
                                       programs.shading_model > rdesc::ELightingTechnique::STYLIZED;
            if (invalid_counts || invalid_state)
            {
                return false;
            }
            std::uint32_t purposes = 0;
            for (const auto& program : programs.programs)
            {
                const auto purpose = static_cast<std::uint32_t>(program.purpose);
                const bool invalid_use = purpose > static_cast<std::uint32_t>(rdesc::EMaterialPurpose::SHADOW) ||
                                         (purpose <= 2 && (purposes & (1u << purpose))) ||
                                         program.output_semantic.empty();
                if (invalid_use || program.shader.stages.size() != 1)
                {
                    return false;
                }
                purposes |= 1u << purpose;
                const auto& stage = program.shader.stages.front();
                if (stage.stage != rdesc::EShaderType::FRAGMENT || stage.entry.empty() ||
                    !detail::validSpirvWords(stage.words) ||
                    !hasCookedLayouts(program.shader.interface, programs.texture_count))
                {
                    return false;
                }
                for (const auto& set : program.shader.interface.sets)
                {
                    for (const auto& binding : set.bindings)
                    {
                        if (binding.name == rdesc::kMaterialTableName && binding.block != programs.parameters)
                        {
                            return false;
                        }
                    }
                }
            }
            for (const auto& parameter : programs.parameter_defaults)
            {
                for (float value : parameter)
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

            void text(std::string_view value)
            {
                pod(static_cast<std::uint32_t>(value.size()));
                raw(value.data(), value.size());
            }

            void layout(const rdesc::ShaderBlock& value)
            {
                pod(value.size);
                pod(value.alignment);
                pod(static_cast<std::uint8_t>(value.storage));
                pod(static_cast<std::uint8_t>(value.runtime_records));
                pod(static_cast<std::uint32_t>(value.fields.size()));
                for (const auto& field : value.fields)
                {
                    text(field.path);
                    pod(static_cast<std::uint8_t>(field.scalar));
                    pod(field.offset);
                    pod(field.rows);
                    pod(field.columns);
                    pod(field.array_stride);
                    pod(field.matrix_stride);
                }
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

            [[nodiscard]] bool text(std::string& value)
            {
                std::uint32_t size = 0;
                if (!pod(size) || size > bytes.size() - offset)
                {
                    return false;
                }
                value.assign(reinterpret_cast<const char*>(bytes.data() + offset), size);
                offset += size;
                return true;
            }

            [[nodiscard]] bool layout(rdesc::ShaderBlock& value)
            {
                std::uint8_t storage = 0, records = 0;
                std::uint32_t count = 0;
                if (!pod(value.size) || !pod(value.alignment) || !pod(storage) || !pod(records) || records > 1 ||
                    !pod(count) || count > (bytes.size() - offset) / 25)
                {
                    return false;
                }
                value.storage = static_cast<rdesc::EDataLayout>(storage);
                value.runtime_records = records != 0;
                value.fields.resize(count);
                for (auto& field : value.fields)
                {
                    std::uint8_t scalar = 0;
                    if (!text(field.path) || !pod(scalar) || !pod(field.offset) || !pod(field.rows) ||
                        !pod(field.columns) || !pod(field.array_stride) || !pod(field.matrix_stride))
                    {
                        return false;
                    }
                    field.scalar = static_cast<rdesc::EDataScalar>(scalar);
                }
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
            std::uint32_t version{}, shader_abi{}, parameter_count{}, texture_count{}, alpha_mode{};
            std::uint8_t double_sided{};
            if (!reader.pod(version) || version != kMaterialVersion || !reader.pod(shader_abi) ||
                shader_abi != rdesc::kMaterialShaderAbi || !reader.pod(parameter_count) ||
                parameter_count > (reader.bytes.size() - reader.offset) / sizeof(std::array<float, 4>))
            {
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
            }
            data->programs.parameter_defaults.resize(parameter_count);
            for (auto& parameter : data->programs.parameter_defaults)
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
            std::uint8_t model = 0;
            if (!reader.pod(model))
            {
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
            }
            data->programs.shading_model = static_cast<rdesc::ELightingTechnique>(model);
            data->programs.alpha_mode = static_cast<lux::rdesc::EAlphaMode>(alpha_mode);
            data->programs.double_sided = double_sided != 0U;
            data->texture_slot_ids.resize(texture_count);
            for (auto& id : data->texture_slot_ids)
            {
                if (!reader.id(id))
                {
                    return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
                }
            }
            auto& programs = data->programs;
            programs.texture_count = texture_count;
            std::uint32_t count = 0;
            if (!reader.layout(programs.parameters) || !reader.pod(count) || count > 3)
            {
                return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
            }
            programs.programs.resize(count);
            for (auto& program : programs.programs)
            {
                std::uint8_t purpose = 0;
                rdesc::SpirvStage stage{rdesc::EShaderType::FRAGMENT};
                std::vector<std::byte> info;
                if (!reader.pod(purpose) || !reader.text(program.output_semantic) || !reader.text(stage.entry) ||
                    !reader.words(stage.words) || !reader.block(info) ||
                    !rdesc::ShaderInfo::deserialize(info, program.shader.interface))
                {
                    return lux::cxx::unexpected(decodeFailure(EAssetDecodeError::INVALID_PAYLOAD));
                }
                program.purpose = static_cast<rdesc::EMaterialPurpose>(purpose);
                program.shader.stages.push_back(std::move(stage));
            }
            if (reader.offset != reader.bytes.size() || !validMaterial(*data))
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
            Writer writer;
            writer.pod(kMaterialVersion);
            writer.pod(rdesc::kMaterialShaderAbi);
            writer.pod(static_cast<std::uint32_t>(asset.data().programs.parameter_defaults.size()));
            for (std::size_t parameter = 0U; parameter < asset.data().programs.parameter_defaults.size(); ++parameter)
            {
                for (const float value : asset.data().programs.parameter_defaults[parameter])
                {
                    writer.pod(value);
                }
            }
            writer.pod(static_cast<std::uint32_t>(asset.data().programs.alpha_mode));
            writer.pod(static_cast<std::uint8_t>(asset.data().programs.double_sided ? 1U : 0U));
            writer.pod(static_cast<std::uint32_t>(asset.data().texture_slot_ids.size()));
            writer.pod(static_cast<std::uint8_t>(asset.data().programs.shading_model));
            for (const auto id : asset.data().texture_slot_ids)
            {
                writer.id(id);
            }
            writer.layout(asset.data().programs.parameters);
            writer.pod(static_cast<std::uint32_t>(asset.data().programs.programs.size()));
            for (const auto& program : asset.data().programs.programs)
            {
                const auto& stage = program.shader.stages.front();
                const auto info = rdesc::ShaderInfo::serialize(program.shader.interface);
                writer.pod(static_cast<std::uint8_t>(program.purpose));
                writer.text(program.output_semantic);
                writer.text(stage.entry);
                writer.pod(static_cast<std::uint32_t>(stage.words.size()));
                writer.raw(stage.words.data(), stage.words.size() * sizeof(std::uint32_t));
                writer.pod(static_cast<std::uint32_t>(info.size()));
                writer.raw(info.data(), info.size());
            }
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
