#include <lux/engine/description/GBufferLayout.hpp>
#include <lux/engine/material/compiler/MaterialCompiler.hpp>
#include <lux/engine/material/compiler/SlangBackend.hpp>
#include <lux/engine/shader/ShaderCompiler.hpp>

#include <algorithm>
#include <new>

namespace lux::material
{
    MaterialDeriveResult deriveMaterial(
        const MaterialIR& ir,
        std::span<const asset::AssetId> textures,
        const MaterialDeriveOptions& options
    ) noexcept
    {
        const auto fail = [](EMaterialDeriveError code, std::string diagnostic)
        { return cxx::unexpected(MaterialDeriveFailure{code, std::move(diagnostic)}); };
        try
        {
            if (!validateMaterialIR(ir))
            {
                return fail(EMaterialDeriveError::INVALID_IR, "invalid material IR");
            }
            const bool is_invalid_request = textures.size() != ir.shader.textures.size() || options.purposes.empty() ||
                                            options.target > rdesc::EShaderTarget::DXIL;
            if (is_invalid_request)
            {
                return fail(EMaterialDeriveError::INVALID_REQUEST, "invalid target or asset bindings");
            }
            unsigned selected = 0;
            for (auto purpose : options.purposes)
            {
                const auto index = static_cast<unsigned>(purpose);
                if (purpose > rdesc::EMaterialPurpose::SHADOW || (selected & (1U << index)))
                {
                    return fail(EMaterialDeriveError::INVALID_REQUEST, "purposes must be valid and unique");
                }
                selected |= 1U << index;
            }
            MaterialDerivation result;
            result.program_cache_key = 1469598103934665603ULL;
            const auto key = [&](std::string_view bytes)
            {
                for (unsigned char byte : bytes)
                {
                    result.program_cache_key ^= byte;
                    result.program_cache_key *= 1099511628211ULL;
                }
                result.program_cache_key ^= bytes.size();
                result.program_cache_key *= 1099511628211ULL;
            };
            key(shader::compilerVersion());
            key(options.profile);
            key(options.target == rdesc::EShaderTarget::SPIRV ? "spirv" : "dxil");
            key("lux-material-ir-1");
            key(std::to_string(rdesc::kMaterialShaderAbi));
            auto& programs = result.material.programs;
            programs.target = options.target;
            programs.shading_model = ir.shading_model;
            programs.alpha_mode = ir.alpha_mode;
            programs.double_sided = ir.double_sided;
            programs.texture_count = static_cast<std::uint32_t>(textures.size());
            result.material.texture_slot_ids.assign(textures.begin(), textures.end());
            for (const auto& parameter : ir.shader.params)
            {
                auto& values = programs.parameter_defaults.emplace_back();
                std::ranges::copy(parameter.dflt, values.begin());
            }
            // Canonical purpose order keeps the code key independent of request ordering.
            for (unsigned index = 0; index < 3; ++index)
            {
                if ((selected & (1U << index)) == 0)
                {
                    continue;
                }
                const auto purpose = static_cast<rdesc::EMaterialPurpose>(index);
                auto source = detail::emitSlang(ir, purpose);
                if (!source)
                {
                    return cxx::unexpected(std::move(source.error()));
                }
                key(*source);
                auto artifact =
                    shader::compile({*source, "main", rdesc::EShaderType::FRAGMENT, options.target, options.profile});
                if (!artifact)
                {
                    return fail(EMaterialDeriveError::COMPILER_FAILURE, std::move(artifact.error().diagnostic));
                }
                for (const auto& set : artifact->interface.sets)
                {
                    for (const auto& binding : set.bindings)
                    {
                        if (binding.name != rdesc::kMaterialTableName)
                        {
                            continue;
                        }
                        const bool is_invalid_binding = binding.type != rdesc::EDescriptorType::STORAGE_BUFFER ||
                                                        binding.count != 0 || binding.writable || !binding.readable ||
                                                        !binding.block.runtime_records;
                        const bool is_layout_mismatch =
                            programs.parameters.size && programs.parameters != binding.block;
                        if (is_invalid_binding || is_layout_mismatch)
                        {
                            return fail(EMaterialDeriveError::INVALID_LAYOUT, "material target layouts disagree");
                        }
                        programs.parameters = binding.block;
                    }
                }
                const auto semantic =
                    purpose == rdesc::EMaterialPurpose::GBUFFER
                        ? rdesc::GBufferLayout::semantic
                        : (purpose == rdesc::EMaterialPurpose::SHADOW ? "lux.material.coverage.forward-z.v1"
                                                                      : "lux.material.forward.linear.v1");
                programs.programs.push_back({purpose, std::string(semantic), std::move(*artifact)});
            }
            return result;
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (const std::exception& error)
        {
            return fail(EMaterialDeriveError::COMPILER_FAILURE, error.what());
        }
        catch (...)
        {
            return fail(EMaterialDeriveError::COMPILER_FAILURE, "foreign material compiler failure");
        }
    }
} // namespace lux::material
