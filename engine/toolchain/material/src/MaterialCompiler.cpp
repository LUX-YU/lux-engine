#include <lux/engine/material/Compiler.hpp>

#include <lux/engine/material/compiler/Backend.hpp>
#include <lux/engine/material/compiler/Lowering.hpp>

#include <algorithm>
#include <cmath>
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
            std::uint64_t node_id = invalid_node,
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

        [[nodiscard]] lux::cxx::expected<void, MaterialCompileFailure>
        validateGraph(const MaterialGraph& graph) noexcept
        {
            if (graph.nodes().empty() || graph.param_slots.size() > rdesc::MaterialDescription::kMaxParams ||
                graph.texture_slots.size() > rdesc::MaterialDescription::kMaxTextures)
                return lux::cxx::unexpected(failure(EMaterialCompileError::INVALID_GRAPH,
                                                     "invalid material graph capacity"));

            const auto alpha_mode = static_cast<std::uint8_t>(graph.render_state.alpha_mode);
            const auto shading_model = static_cast<std::uint8_t>(graph.shading_model);
            if (alpha_mode > static_cast<std::uint8_t>(rdesc::EAlphaMode::Blend) ||
                shading_model > static_cast<std::uint8_t>(rdesc::ELightingTechnique::Graph) ||
                !std::isfinite(graph.render_state.alpha_cutoff))
                return lux::cxx::unexpected(failure(EMaterialCompileError::INVALID_GRAPH,
                                                     "invalid material render state"));

            for (const auto& texture : graph.texture_slots)
                if (texture.texture.isNull())
                    return lux::cxx::unexpected(failure(EMaterialCompileError::INVALID_GRAPH,
                                                         "material texture slot has a null AssetId"));
            for (const auto& parameter : graph.param_slots)
                for (const float value : parameter.dflt)
                    if (!std::isfinite(value))
                        return lux::cxx::unexpected(failure(EMaterialCompileError::INVALID_GRAPH,
                                                             "material parameter default is not finite"));
            return {};
        }
    } // namespace

    lux::cxx::expected<rdesc::MaterialDescription, MaterialCompileFailure>
    compileMaterial(const MaterialGraph& graph) noexcept
    {
        if (auto validation = validateGraph(graph); !validation)
            return lux::cxx::unexpected(std::move(validation.error()));

        try
        {
            auto lowered = compiler::lowerMaterial(graph);
            if (!lowered)
                return lux::cxx::unexpected(std::move(lowered.error()));

            const std::vector<std::string> include_directories{
                LUX_MATERIAL_SHADER_EMITTED_DIR,
                LUX_MATERIAL_SHADER_SOURCE_DIR
            };
            const auto compile_pass = [&](shadergen::glsl::EMaterialPass pass)
                -> lux::cxx::expected<shadergen::glsl::CompiledShader, MaterialCompileFailure>
            {
                shadergen::glsl::EmitParams parameters;
                parameters.pass = pass;
                parameters.shading_model = lowered->shading_model;
                parameters.alpha_mode = lowered->alpha_mode;
                parameters.alpha_cutoff = lowered->alpha_cutoff;
                auto compiled = shadergen::glsl::compileToSpirv(lowered->shader, parameters, include_directories);
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
                return lux::cxx::unexpected(std::move(gbuffer.error()));
            auto forward = compile_pass(shadergen::glsl::EMaterialPass::FORWARD);
            if (!forward)
                return lux::cxx::unexpected(std::move(forward.error()));

            rdesc::MaterialDescription description;
            description.parameter_count = static_cast<std::uint32_t>(graph.param_slots.size());
            for (std::uint32_t parameter = 0U; parameter < description.parameter_count; ++parameter)
                std::copy_n(graph.param_slots[parameter].dflt,
                            description.parameter_defaults[parameter].size(),
                            description.parameter_defaults[parameter].begin());
            description.alpha_mode = graph.render_state.alpha_mode;
            description.double_sided = graph.render_state.double_sided;
            description.gbuffer_spirv = std::move(gbuffer->spirv);
            description.gbuffer_info = std::move(gbuffer->info);
            description.forward_spirv = std::move(forward->spirv);
            description.forward_info = std::move(forward->info);
            for (std::uint32_t slot = 0U; slot < graph.texture_slots.size(); ++slot)
                description.texture_slot_ids[slot] = graph.texture_slots[slot].texture;

            if (!validSpirv(description.gbuffer_spirv) || !validSpirv(description.forward_spirv))
                return lux::cxx::unexpected(failure(EMaterialCompileError::INVALID_RESULT,
                                                     "compiler produced an invalid MaterialDescription"));
            return description;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EMaterialCompileError::ALLOCATION_FAILURE,
                                                 "allocation failure"));
        }
        catch (...)
        {
            return lux::cxx::unexpected(failure(EMaterialCompileError::INVALID_RESULT,
                                                 "foreign material compiler failure"));
        }
    }
} // namespace lux::material
