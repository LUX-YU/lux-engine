#include <algorithm>
#include <lux/engine/toolchain/shader/ShaderAssets.hpp>
#include <lux/engine/toolchain/shader/SpirvRelocation.hpp>
#include <utility>

namespace lux::toolchain
{
    std::vector<std::uint8_t> shaderSchemaIdentity(const rdesc::PassShaderContract& schema) noexcept
    {
        std::vector<std::uint8_t> result;
        const auto number = [&](std::uint64_t value)
        {
            for (unsigned int byte = 0; byte != 8; ++byte)
            {
                result.push_back(static_cast<std::uint8_t>(value >> (byte * 8)));
            }
        };
        const auto text = [&](std::string_view value)
        {
            number(value.size());
            result.insert(result.end(), value.begin(), value.end());
        };
        number(1); // Encoding revision, not a second schema revision authority.
        text(schema.canonical_name);
        number(schema.parameter_size);
        number(schema.parameter_alignment);
        text(schema.declarations);
        for (auto stage : schema.stage_declarations)
        {
            text(stage);
        }
        number(schema.resources.size());
        for (const auto& field : schema.resources)
        {
            text(field.path);
            text(field.shader_name);
            number(static_cast<std::uint32_t>(field.role));
            number(static_cast<std::uint32_t>(field.owner));
            number(static_cast<std::uint32_t>(field.frequency));
            number(field.required);
            number(field.array_count);
            number(field.element_stride);
            text(field.semantic);
            text(field.paired_texture);
            text(field.dimension);
            text(field.image_format);
            number(field.stages);
            number(field.descriptor_array);
            number(field.element_alignment);
        }
        number(schema.scalars.size());
        for (const auto& field : schema.scalars)
        {
            text(field.path);
            number(static_cast<std::uint32_t>(field.kind));
            number(field.offset);
            number(field.size);
            number(field.array_stride);
            number(field.array_count);
            number(static_cast<std::uint32_t>(field.owner));
            number(static_cast<std::uint32_t>(field.frequency));
            number(field.stages);
        }
        return result;
    }

    cxx::expected<CompiledShaderVariant, std::string> makeCompiledShaderVariant(
        ShaderBuildInputs inputs,
        std::vector<ShaderStageBinary> stages,
        const rdesc::PassShaderContract& schema
    ) noexcept
    {
        const bool invalid_identity =
            inputs.asset_name.empty() || inputs.variant_name.empty() ||
            inputs.asset_name.find('#') != std::string::npos || inputs.variant_name.find('#') != std::string::npos ||
            inputs.compiler_identity.empty() || inputs.target_environment != "vulkan1.3" || inputs.sources.empty();
        if (invalid_identity)
        {
            return cxx::unexpected(std::string("Incomplete Shader Asset/Variant build identity"));
        }
        std::sort(
            inputs.sources.begin(),
            inputs.sources.end(),
            [](const auto& a, const auto& b) { return a.canonical_path < b.canonical_path; }
        );
        for (std::size_t i = 0; i < inputs.sources.size(); ++i)
        {
            const bool invalid_source =
                inputs.sources[i].canonical_path.empty() ||
                (i != 0 && inputs.sources[i - 1].canonical_path == inputs.sources[i].canonical_path);
            if (invalid_source)
            {
                return cxx::unexpected(std::string("Invalid/duplicate source dependency identity"));
            }
        }
        std::sort(
            inputs.defines.begin(),
            inputs.defines.end(),
            [](const auto& a, const auto& b) { return a.name < b.name; }
        );
        for (std::size_t i = 0; i < inputs.defines.size(); ++i)
        {
            const bool invalid_define =
                inputs.defines[i].name.empty() || (i != 0 && inputs.defines[i - 1].name == inputs.defines[i].name);
            if (invalid_define)
            {
                return cxx::unexpected(std::string("Invalid/duplicate Shader define"));
            }
        }
        std::sort(stages.begin(), stages.end(), [](const auto& a, const auto& b) { return a.stage < b.stage; });
        std::uint32_t mask = 0;
        std::vector<PassShaderModule> modules;
        std::vector<PassDescriptorLocation> original_locations;
        std::uint32_t ordinal = 0;
        for (std::uint32_t i = 0; i < schema.resources.size(); ++i)
        {
            if (rdesc::isShaderDescriptorRole(schema.resources[i].role))
            {
                original_locations.push_back({i, 0, ordinal++});
            }
        }
        for (const auto& stage : stages)
        {
            const bool invalid_stage =
                (stage.stage != 1 && stage.stage != 2 && stage.stage != 4) || (mask & stage.stage) != 0;
            if (invalid_stage)
            {
                return cxx::unexpected(std::string("Invalid/duplicate Shader stage"));
            }
            mask |= stage.stage;
            // Identity relocation validates direct decorations and binary as well as schema. No second validator.
            auto validated = relocatePassSpirv(stage.words, schema, original_locations, stage.stage);
            if (!validated)
            {
                return cxx::unexpected(std::move(validated.error()));
            }
            modules.push_back({stage.words});
        }
        if (mask != 3 && mask != 4)
        {
            return cxx::unexpected(std::string("Shader Variant requires VS+FS or Compute"));
        }
        auto reconciled = validatePassShaders(modules, schema, mask);
        if (!reconciled)
        {
            return cxx::unexpected(std::move(reconciled.error()));
        }
        return CompiledShaderVariant{{std::move(inputs), shaderSchemaIdentity(schema), std::move(stages)}};
    }

    ShaderVariantCatalog::ShaderVariantCatalog(std::size_t capacity) noexcept : capacity_(capacity)
    {
        entries_.reserve(capacity);
    }

    cxx::expected<std::unique_ptr<const CompiledShaderVariant>, std::string> ShaderVariantCatalog::publish(
        CompiledShaderVariant candidate
    ) noexcept
    {
        const auto& incoming = candidate.identity().inputs;
        for (auto& entry : entries_)
        {
            const auto& existing = entry->identity().inputs;
            if (existing.asset_name == incoming.asset_name && existing.variant_name == incoming.variant_name)
            {
                auto replacement = std::make_unique<const CompiledShaderVariant>(std::move(candidate));
                return std::exchange(entry, std::move(replacement));
            }
        }
        if (entries_.size() == capacity_)
        {
            return cxx::unexpected(std::string("Shader Variant catalog capacity exhausted"));
        }
        entries_.push_back(std::make_unique<const CompiledShaderVariant>(std::move(candidate)));
        return std::unique_ptr<const CompiledShaderVariant>{};
    }

    const CompiledShaderVariant* ShaderVariantCatalog::find(const ShaderVariantIdentity& identity) const& noexcept
    {
        const auto* result = current(identity.inputs.asset_name, identity.inputs.variant_name);
        return result != nullptr && result->identity() == identity ? result : nullptr;
    }

    const CompiledShaderVariant* ShaderVariantCatalog::current(std::string_view asset, std::string_view variant)
        const& noexcept
    {
        for (const auto& entry : entries_)
        {
            const auto& inputs = entry->identity().inputs;
            if (inputs.asset_name == asset && inputs.variant_name == variant)
            {
                return entry.get();
            }
        }
        return nullptr;
    }

    cxx::expected<std::unique_ptr<const CompiledShaderVariant>, std::string> ShaderVariantCatalog::remove(
        std::string_view asset,
        std::string_view variant
    ) noexcept
    {
        for (auto it = entries_.begin(); it != entries_.end(); ++it)
        {
            const auto& inputs = (*it)->identity().inputs;
            if (inputs.asset_name == asset && inputs.variant_name == variant)
            {
                auto result = std::move(*it);
                entries_.erase(it);
                return result;
            }
        }
        return cxx::unexpected(std::string("Shader Asset/Variant not found"));
    }
} // namespace lux::toolchain
