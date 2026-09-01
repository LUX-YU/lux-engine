#include <lux/engine/toolchain/asset/material/MaterialCooker.hpp>

#include <lux/engine/material/graph/MaterialGraph.hpp>
#include <lux/engine/toolchain/asset/material/MaterialLowering.hpp>
#include <lux/engine/toolchain/asset/material/MaterialToGraph.hpp>
#include <lux/engine/toolchain/shader/Backend.hpp>

#include <algorithm>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace lux::toolchain
{
    namespace
    {
        [[nodiscard]] MaterialCookFailure failure(
            EMaterialCookError code,
            std::string detail
        ) noexcept
        {
            return MaterialCookFailure{code, std::move(detail)};
        }
    } // namespace

    lux::cxx::expected<std::shared_ptr<const lux::asset::MaterialAsset>, MaterialCookFailure>
    cookMaterial(
        lux::asset::AssetInfo info,
        const lux::material::MaterialGraph& graph
    ) noexcept
    {
        if (info.id.isNull() || graph.param_slots.size() > lux::rdesc::MaterialDescription::kMaxParams ||
            graph.texture_slots.size() > lux::rdesc::MaterialDescription::kMaxTextures)
        {
            return lux::cxx::unexpected(failure(EMaterialCookError::INVALID_INPUT, "invalid material metadata"));
        }
        try
        {
            auto lowered = lux::shadergen::material::lowerMaterial(graph);
            if (!lowered)
            {
                return lux::cxx::unexpected(failure(
                    EMaterialCookError::LOWERING_FAILED,
                    std::move(lowered.error())
                ));
            }

            const std::vector<std::string> include_directories{
                LUX_MATERIAL_SHADER_EMITTED_DIR,
                LUX_MATERIAL_SHADER_SOURCE_DIR
            };
            const auto compile_pass = [&](lux::shadergen::glsl::EMaterialPass pass)
                -> lux::cxx::expected<lux::shadergen::glsl::CompiledShader, std::string>
            {
                lux::shadergen::glsl::EmitParams parameters;
                parameters.pass = pass;
                parameters.shading_model = lowered->shading_model;
                parameters.alpha_mode = lowered->alpha_mode;
                parameters.alpha_cutoff = lowered->alpha_cutoff;
                return lux::shadergen::glsl::compileToSpirv(
                    lowered->ir,
                    parameters,
                    include_directories
                );
            };

            auto gbuffer = compile_pass(lux::shadergen::glsl::EMaterialPass::GBUFFER);
            if (!gbuffer)
            {
                return lux::cxx::unexpected(failure(
                    EMaterialCookError::SHADER_COMPILE_FAILED,
                    "gbuffer: " + gbuffer.error()
                ));
            }
            auto forward = compile_pass(lux::shadergen::glsl::EMaterialPass::FORWARD);
            if (!forward)
            {
                return lux::cxx::unexpected(failure(
                    EMaterialCookError::SHADER_COMPILE_FAILED,
                    "forward: " + forward.error()
                ));
            }

            auto description = std::make_shared<lux::rdesc::MaterialDescription>();
            description->parameter_count = static_cast<std::uint32_t>(graph.param_slots.size());
            for (std::uint32_t parameter = 0U; parameter < description->parameter_count; ++parameter)
            {
                std::copy_n(
                    graph.param_slots[parameter].dflt,
                    description->parameter_defaults[parameter].size(),
                    description->parameter_defaults[parameter].begin()
                );
            }
            description->alpha_mode = graph.render_state.alpha_mode;
            description->double_sided = graph.render_state.double_sided;
            description->gbuffer_spirv = std::move(gbuffer->spirv);
            description->gbuffer_info = std::move(gbuffer->info);
            description->forward_spirv = std::move(forward->spirv);
            description->forward_info = std::move(forward->info);
            for (std::uint32_t slot = 0U; slot < graph.texture_slots.size(); ++slot)
            {
                if (graph.texture_slots[slot].texture.isNull())
                {
                    return lux::cxx::unexpected(failure(
                        EMaterialCookError::INVALID_INPUT,
                        "material texture slot has a null AssetId"
                    ));
                }
                description->texture_slot_ids[slot] = graph.texture_slots[slot].texture;
            }

            auto asset = lux::asset::MaterialAsset::create(
                std::move(info),
                std::move(description)
            );
            if (!asset)
            {
                return lux::cxx::unexpected(failure(
                    EMaterialCookError::INVALID_ASSET,
                    "compiled MaterialDescription failed typed Asset validation"
                ));
            }
            return *asset;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EMaterialCookError::ALLOCATION_FAILURE, "allocation failure"));
        }
        catch (...)
        {
            return lux::cxx::unexpected(failure(EMaterialCookError::INVALID_INPUT, "foreign compiler failure"));
        }
    }

    lux::cxx::expected<std::shared_ptr<const lux::asset::MaterialAsset>, MaterialCookFailure>
    cookImportedMaterial(
        lux::asset::AssetInfo info,
        const ImportedMaterialDescription& imported
    ) noexcept
    {
        try
        {
            auto graph = lux::shadergen::material::materialToGraph(imported);
            if (!graph)
            {
                return lux::cxx::unexpected(failure(
                    EMaterialCookError::INVALID_INPUT,
                    std::move(graph.error())
                ));
            }
            return cookMaterial(std::move(info), *graph);
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EMaterialCookError::ALLOCATION_FAILURE, "allocation failure"));
        }
    }
} // namespace lux::toolchain
