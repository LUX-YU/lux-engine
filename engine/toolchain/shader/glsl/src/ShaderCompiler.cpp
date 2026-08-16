// =============================================================================
//  ShaderCompiler.cpp  --  emitGlsl() + libshaderc -> SPIR-V, wired up with an IncluderInterface
// -----------------------------------------------------------------------------
//  shaderc only appears here (PRIVATE, never leaks into Backend.hpp). Target env
//  is vulkan1.2, matching the engine's builtin shaders (glslc --target-env=vulkan1.2).
//
//  Key difference from the old material_graph_glsl: this wires up a filesystem
//  IncluderInterface, so the shell can #include the real single-source-of-truth
//  files (gbuffer_encode.glsl, etc.) instead of hand-copying them inline. The
//  search path is supplied by the caller via include_dirs -- this component has
//  no dependency on render and does not hard-code its asset paths.
// =============================================================================

#include <lux/engine/toolchain/shader/Backend.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/toolchain/shader/SpirvReflection.hpp>

#include <shaderc/shaderc.hpp>

#include <fstream>
#include <iterator>
#include <memory>
#include <string>
#include <vector>

namespace lux::shadergen::glsl
{
    namespace
    {
        // Filesystem #include resolution: looks up the requested name across
        // include_dirs and reads it in. shaderc keeps the content string alive
        // via user_data; ReleaseInclude frees it.
        class FileIncluder final : public shaderc::CompileOptions::IncluderInterface
        {
        public:
            explicit FileIncluder(std::vector<std::string> dirs) : dirs_(std::move(dirs)) {}

            shaderc_include_result* GetInclude(const char*          requested,
                                               shaderc_include_type /*type*/,
                                               const char*          /*requesting*/,
                                               size_t               /*depth*/) override
            {
                auto* data = new Payload{};
                for (const std::string& dir : dirs_)
                {
                    std::ifstream f(dir + "/" + requested, std::ios::binary);
                    if (!f) continue;
                    data->name    = requested;
                    data->content.assign(std::istreambuf_iterator<char>(f),
                                         std::istreambuf_iterator<char>());
                    return make(data);
                }
                // Not found: return an empty source_name (shaderc reports this
                // as an include error), with content carrying a diagnostic message.
                data->name.clear();
                data->content = std::string("shadergen: include not found: ") + requested;
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

            std::vector<std::string> dirs_;
        };
    } // namespace

    lux::cxx::expected<CompiledShader, std::string>
    compileToSpirv(const ShaderIR&                 ir,
                   const EmitParams&               params,
                   const std::vector<std::string>& include_dirs)
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
        options.SetIncluder(std::make_unique<FileIncluder>(include_dirs));

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
