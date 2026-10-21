#include <lux/engine/material/Compiler.hpp>

#include <lux/engine/description/GBufferLayout.hpp>
#include <lux/engine/material/MaterialIR.hpp>
#include <lux/engine/material/compiler/Backend.hpp>

#include <algorithm>
#include <exception>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace lux::material
{
    namespace
    {
        [[nodiscard]] MaterialCompileFailure failure(
            EMaterialCompileError code,
            std::string message,
            NodeId node_id = {},
            std::uint32_t pin_index = ~std::uint32_t{0}
        ) noexcept
        {
            return MaterialCompileFailure{code, std::move(message), node_id, pin_index};
        }

        [[nodiscard]] bool validSpirv(const std::vector<std::uint32_t>& words) noexcept
        {
            return words.size() >= 5U && words.front() == 0x07230203U;
        }

        [[nodiscard]] EMaterialCompileError shaderFailureCode(const std::string& message) noexcept
        {
            const bool is_compile_failure = message.find("shaderc failed") != std::string::npos ||
                                            message.find("SPIR-V reflection failed") != std::string::npos;
            return is_compile_failure ? EMaterialCompileError::SHADER_COMPILATION_FAILURE
                                      : EMaterialCompileError::SHADER_EMISSION_FAILURE;
        }

    } // namespace

    MaterialCompileResult compileMaterial(
        const MaterialGraph& graph,
        std::span<const rdesc::EMaterialPurpose> purposes
    ) noexcept
    {
        try
        {
            std::uint32_t selected = 0;
            for (auto purpose : purposes)
            {
                const auto index = static_cast<std::uint32_t>(purpose);
                const bool invalid_purpose = index > static_cast<std::uint32_t>(rdesc::EMaterialPurpose::SHADOW);
                if (invalid_purpose || (selected & (1u << index)))
                {
                    return cxx::unexpected(
                        failure(EMaterialCompileError::INVALID_PURPOSES, "material purposes must be valid and distinct")
                    );
                }
                selected |= 1u << index;
            }
            if (selected == 0)
            {
                return cxx::unexpected(failure(EMaterialCompileError::INVALID_PURPOSES, "no material purpose requested")
                );
            }
            auto lowered = lowerMaterial(graph);
            if (!lowered)
            {
                return lux::cxx::unexpected(std::move(lowered.error()));
            }

            using ShaderCompileResult = cxx::expected<shadergen::glsl::CompiledShader, MaterialCompileFailure>;
            const auto compile_pass = [&](rdesc::EMaterialPurpose purpose) -> ShaderCompileResult
            {
                shadergen::glsl::EmitParams parameters;
                parameters.purpose = purpose;
                parameters.shading_model = lowered->shading_model;
                parameters.alpha_mode = lowered->alpha_mode;
                parameters.alpha_cutoff = lowered->alpha_cutoff;
                auto compiled = shadergen::glsl::compileToSpirv(lowered->shader, parameters);
                if (!compiled)
                {
                    auto message = std::move(compiled.error());
                    const auto code = shaderFailureCode(message);
                    return lux::cxx::unexpected(failure(code, std::move(message)));
                }
                return std::move(*compiled);
            };

            rdesc::MaterialDescription description;
            auto& programs = description.programs;
            programs.parameter_defaults.resize(graph.param_slots.size());
            description.texture_slot_ids.resize(graph.texture_slots.size());
            for (std::uint32_t parameter = 0; parameter < programs.parameter_defaults.size(); ++parameter)
            {
                std::copy_n(graph.param_slots[parameter].dflt, 4, programs.parameter_defaults[parameter].begin());
            }
            programs.texture_count = static_cast<std::uint32_t>(graph.texture_slots.size());
            programs.shading_model = lowered->shading_model;
            programs.alpha_mode = graph.render_state.alpha_mode;
            programs.double_sided = graph.render_state.double_sided;
            for (auto purpose : purposes)
            {
                auto compiled = compile_pass(purpose);
                if (!compiled)
                {
                    return lux::cxx::unexpected(std::move(compiled.error()));
                }
                if (!validSpirv(compiled->spirv))
                {
                    return lux::cxx::unexpected(failure(EMaterialCompileError::INVALID_RESULT, "invalid Shader code"));
                }
                for (const auto& set : compiled->info.sets)
                {
                    for (const auto& binding : set.bindings)
                    {
                        if (binding.name != rdesc::kMaterialTableName)
                        {
                            continue;
                        }
                        if (programs.parameters.size && programs.parameters != binding.block)
                        {
                            return lux::cxx::unexpected(failure(
                                EMaterialCompileError::INVALID_RESULT,
                                "material uses disagree on the compiled record layout"
                            ));
                        }
                        programs.parameters = binding.block;
                    }
                }
                const std::string_view semantic =
                    purpose == rdesc::EMaterialPurpose::GBUFFER  ? rdesc::GBufferLayout::semantic
                    : purpose == rdesc::EMaterialPurpose::SHADOW ? "lux.material.coverage.forward-z.v1"
                                                                 : "lux.material.forward.linear.v1";
                rdesc::SpirvArtifact shader;
                shader.stages.push_back({rdesc::EShaderType::FRAGMENT, "main", std::move(compiled->spirv)});
                shader.interface = std::move(compiled->info);
                programs.programs.push_back({purpose, std::string(semantic), std::move(shader)});
            }
            for (std::uint32_t slot = 0; slot < graph.texture_slots.size(); ++slot)
            {
                description.texture_slot_ids[slot] = graph.texture_slots[slot].texture;
            }
            return description;
        }
        catch (const std::bad_alloc&)
        {
            std::terminate();
        }
        catch (...)
        {
            return lux::cxx::unexpected(
                failure(EMaterialCompileError::INVALID_RESULT, "foreign material compiler failure")
            );
        }
    }
} // namespace lux::material
