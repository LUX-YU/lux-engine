#include <lux/engine/description/GBufferLayout.hpp>
#include <lux/engine/material/compiler/MaterialCompiler.hpp>
#include <lux/engine/shader/ShaderCompiler.hpp>

#include <algorithm>
#include <charconv>
#include <new>

namespace lux::material
{
    MaterialDeriveResult
    deriveMaterial(
        shader::ShaderCompiler& compiler,
        const MaterialGraph& graph,
        const MaterialDeriveOptions& options
    ) noexcept
    {
        const auto fail = [](EMaterialDeriveError code, std::string diagnostic)
        { return cxx::unexpected(MaterialDeriveFailure{code, std::move(diagnostic)}); };
        try
        {
            const bool is_invalid_request = options.purposes.empty() || options.target > rdesc::EShaderTarget::DXIL;
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
            key("lux-material-slang-direct-3");
            key(std::to_string(rdesc::kMaterialShaderAbi));
            auto& programs = result.material.programs;
            programs.target = options.target;
            programs.shading_model = graph.shading_model;
            programs.alpha_mode = graph.render_state.alpha_mode;
            programs.double_sided = graph.render_state.double_sided;
            programs.texture_count = static_cast<std::uint32_t>(graph.texture_slots.size());
            for (const auto& texture : graph.texture_slots)
            {
                result.material.texture_slot_ids.push_back(texture.texture);
            }
            for (const auto& parameter : graph.param_slots)
            {
                auto& values = programs.parameter_defaults.emplace_back();
                std::ranges::copy(parameter.dflt, values.begin());
            }
            // One immutable batch shares the target Session, including imported common modules.
            std::vector<std::string> sources;
            std::vector<rdesc::EMaterialPurpose> purposes;
            sources.reserve(3);
            for (unsigned index = 0; index < 3; ++index)
            {
                if ((selected & (1U << index)) == 0)
                {
                    continue;
                }
                const auto purpose = static_cast<rdesc::EMaterialPurpose>(index);
                auto source = generateMaterialSlang(graph, purpose);
                if (!source)
                {
                    return cxx::unexpected(MaterialDeriveFailure{
                        EMaterialDeriveError::INVALID_GRAPH,
                        source.error().message,
                        source.error().node_id,
                        source.error().pin_index
                    });
                }
                key(*source);
                sources.push_back(std::move(*source));
                purposes.push_back(purpose);
            }
            std::vector<shader::ShaderCompileReport> reports(sources.size());
            std::vector<shader::ShaderCompileRequest> requests;
            for (std::size_t index = 0; index < sources.size(); ++index)
            {
                requests.push_back(
                    {sources[index],
                     "main",
                     rdesc::EShaderType::FRAGMENT,
                     options.target,
                     options.profile,
                     "lux_material.slang",
                     {},
                     &reports[index]
                    }
                );
            }
            auto artifacts = compiler.compileBatch(requests);
            if (!artifacts)
            {
                auto diagnostic = std::move(artifacts.error().diagnostic);
                graph::NodeId node;
                const auto marker = diagnostic.find("node_");
                if (marker != std::string::npos)
                {
                    std::from_chars(diagnostic.data() + marker + 5, diagnostic.data() + diagnostic.size(), node.value);
                }
                return cxx::unexpected(
                    MaterialDeriveFailure{EMaterialDeriveError::COMPILER_FAILURE, std::move(diagnostic), node}
                );
            }
            for (std::size_t index = 0; index < artifacts->size(); ++index)
            {
                auto* artifact = &(*artifacts)[index];
                const auto purpose = purposes[index];
                result.diagnostic += reports[index].diagnostic;
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
            if (programs.parameters.size == 0)
            {
                programs.parameter_defaults.clear();
                programs.texture_count = 0;
                result.material.texture_slot_ids.clear();
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
