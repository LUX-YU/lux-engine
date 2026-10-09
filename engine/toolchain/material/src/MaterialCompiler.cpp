#include <lux/engine/material/Compiler.hpp>

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
            std::uint32_t pin_index = invalid_pin
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

    lux::cxx::expected<rdesc::MaterialDescription, MaterialCompileFailure> compileMaterial(const MaterialGraph& graph
    ) noexcept
    {
        try
        {
            auto lowered = lowerMaterial(graph);
            if (!lowered)
            {
                return lux::cxx::unexpected(std::move(lowered.error()));
            }

            const auto compile_pass = [&](shadergen::glsl::EMaterialPass pass
                                      ) -> lux::cxx::expected<shadergen::glsl::CompiledShader, MaterialCompileFailure>
            {
                shadergen::glsl::EmitParams parameters;
                parameters.pass = pass;
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

            auto gbuffer = compile_pass(shadergen::glsl::EMaterialPass::GBUFFER);
            if (!gbuffer)
            {
                return lux::cxx::unexpected(std::move(gbuffer.error()));
            }
            auto forward = compile_pass(shadergen::glsl::EMaterialPass::FORWARD);
            if (!forward)
            {
                return lux::cxx::unexpected(std::move(forward.error()));
            }

            rdesc::MaterialDescription description;
            description.parameter_count = static_cast<std::uint32_t>(graph.param_slots.size());
            for (std::uint32_t parameter = 0U; parameter < description.parameter_count; ++parameter)
            {
                std::copy_n(
                    graph.param_slots[parameter].dflt,
                    description.parameter_defaults[parameter].size(),
                    description.parameter_defaults[parameter].begin()
                );
            }
            description.alpha_mode = graph.render_state.alpha_mode;
            description.double_sided = graph.render_state.double_sided;
            description.gbuffer_spirv = std::move(gbuffer->spirv);
            description.gbuffer_info = std::move(gbuffer->info);
            description.forward_spirv = std::move(forward->spirv);
            description.forward_info = std::move(forward->info);
            for (std::uint32_t slot = 0U; slot < graph.texture_slots.size(); ++slot)
            {
                description.texture_slot_ids[slot] = graph.texture_slots[slot].texture;
            }

            if (!validSpirv(description.gbuffer_spirv) || !validSpirv(description.forward_spirv))
            {
                return lux::cxx::unexpected(
                    failure(EMaterialCompileError::INVALID_RESULT, "compiler produced an invalid MaterialDescription")
                );
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
