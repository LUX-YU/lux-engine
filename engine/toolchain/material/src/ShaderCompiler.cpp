// =============================================================================
//  ShaderCompiler.cpp  --  emitGlsl() + libshaderc -> SPIR-V, with embedded includes
// -----------------------------------------------------------------------------
//  shaderc only appears here (PRIVATE, never leaks into Backend.hpp). Target env
//  is vulkan1.2, matching the engine's builtin shaders (glslc --target-env=vulkan1.2).
//
//  The shell includes the canonical build-time resources (gbuffer_encode.glsl,
//  lighting_common.glsl, etc.) from an immutable in-memory map. Installed
//  compilers never read the original source checkout or build directory.
// =============================================================================

#include <lux/engine/material/compiler/Backend.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/toolchain/shader/SpirvReflection.hpp>
#include <lux/engine/material/compiler/MaterialShaderIncludes.generated.hpp>

#include <shaderc/shaderc.hpp>

#include <algorithm>
#include <memory>
#include <string>
#include <string_view>

namespace lux::shadergen::glsl
{
    namespace
    {
        namespace resources = ::lux::material::compiler::generated;

        [[nodiscard]] bool validIncludeName(std::string_view name) noexcept
        {
            return !name.empty() && name.front() != '/' && name.front() != '\\' &&
                name.find(':') == std::string_view::npos && name.find("..") == std::string_view::npos &&
                name.find('\\') == std::string_view::npos;
        }

        class MemoryIncluder final : public shaderc::CompileOptions::IncluderInterface
        {
        public:
            shaderc_include_result* GetInclude(const char*          requested,
                                               shaderc_include_type /*type*/,
                                               const char*          /*requesting*/,
                                               size_t               /*depth*/) override
            {
                auto* data = new Payload{};
                const std::string_view requested_name = requested != nullptr ? requested : "";
                if (validIncludeName(requested_name))
                {
                    const auto resource = std::find_if(
                        resources::kMaterialShaderIncludes.begin(),
                        resources::kMaterialShaderIncludes.end(),
                        [requested_name](const resources::EmbeddedShaderInclude& candidate)
                        {
                            return candidate.name == requested_name;
                        }
                    );
                    if (resource != resources::kMaterialShaderIncludes.end())
                    {
                        data->name = resource->name;
                        data->content.assign(
                            reinterpret_cast<const char*>(resource->data),
                            resource->size
                        );
                        return make(data);
                    }
                }
                data->name.clear();
                data->content = "shadergen: embedded include not found or invalid: ";
                data->content += requested_name;
                return make(data);
            }

            void ReleaseInclude(shaderc_include_result* result) override
            {
                delete static_cast<Payload*>(result->user_data);
                delete result;
            }

        private:
            struct Payload { std::string name; std::string content; };

            static shaderc_include_result* make(Payload* p)
            {
                auto* r = new shaderc_include_result{};
                r->source_name        = p->name.c_str();
                r->source_name_length = p->name.size();
                r->content            = p->content.c_str();
                r->content_length     = p->content.size();
                r->user_data          = p;
                return r;
            }

        };
    } // namespace

    lux::cxx::expected<CompiledShader, std::string>
    compileToSpirv(const ShaderIR& ir, const EmitParams& params)
    {
        auto source_exp = emitGlsl(ir, params);
        if (!source_exp)
            return lux::cxx::unexpected(std::move(source_exp.error()));
        const std::string& source = *source_exp;

        shaderc::Compiler       compiler;
        shaderc::CompileOptions options;
        options.SetSourceLanguage(shaderc_source_language_glsl);
        options.SetTargetEnvironment(shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_2);
        options.SetOptimizationLevel(shaderc_optimization_level_zero);
        options.SetIncluder(std::make_unique<MemoryIncluder>());

        const shaderc::SpvCompilationResult result =
            compiler.CompileGlslToSpv(source, shaderc_fragment_shader, "shadergen.frag", options);

        if (result.GetCompilationStatus() != shaderc_compilation_status_success)
            return lux::cxx::unexpected(
                "shadergen: shaderc failed: " + result.GetErrorMessage() +
                "\n--- generated GLSL ---\n" + source);

        CompiledShader out;
        out.spirv.assign(result.cbegin(), result.cend());

        // Reflection is extracted in full from the SPIR-V we just compiled
        // (via spirv-cross), no more hand-written approximation.
        //
        // The old implementation hand-stuffed uTex/uMats entries based on
        // whether the IR contained SampleTexture/Param -- but the forward
        // variant's shader pulls in the entire lighting/shadow family via
        // #include (uViews, uSpotLights..., uShadowSlices..., a dozen-plus
        // bindings), so that hand-rolled reflection only ever covered a
        // subset of what the module actually uses. The render side builds
        // its layout from reflection, so a partial reflection means some
        // referenced sets are missing from the layout ("uses set #N but not
        // bound"), which addMergedLayoutVariant's completeness check already
        // catches. The sole source of reflection is the Toolchain adapter.
        if (!lux::toolchain::reflectSpirv(
                out.spirv.data(), out.spirv.size() * sizeof(uint32_t), out.info))
            return lux::cxx::unexpected(std::string(
                "shadergen: SPIR-V reflection failed"));
        return out;
    }

} // namespace lux::shadergen::glsl
