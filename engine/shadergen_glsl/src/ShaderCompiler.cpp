// =============================================================================
//  ShaderCompiler.cpp  —  emitGlsl() + libshaderc -> SPIR-V，配 IncluderInterface
// -----------------------------------------------------------------------------
//  shaderc 只在此出现（PRIVATE，不泄漏进 Backend.hpp）。target env = vulkan1.2，
//  与引擎 builtin shader（glslc --target-env=vulkan1.2）一致。
//
//  关键差异（相对旧 material_graph_glsl）：配了一个文件系统 IncluderInterface，
//  外壳因此能 #include 真 SSOT（gbuffer_encode.glsl 等）而非内联手抄。搜索路径由
//  调用方经 include_dirs 传入 —— 本组件不依赖 render、不硬编码其资产路径。
// =============================================================================

#include <lux/engine/shadergen/glsl/Backend.hpp>
#include <lux/engine/description/ShaderInfo.hpp>

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
        // 文件系统 #include 解析：在 include_dirs 里按名查找并读入。shaderc 通过
        // user_data 持有内容串的生命周期，ReleaseInclude 释放。
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
                // 未找到：返回空 source_name（shaderc 据此报 include 错误），content 带诊断。
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

        namespace rdesc = ::lux::rdesc;
        out.info.entry_points.push_back(
            rdesc::EntryPointInfo{ "main", rdesc::EShaderType::FRAGMENT });

        bool tex = false, param = false;
        for (const auto& v : ir.values)
        {
            if (v.op == EOp::SampleTexture) tex   = true;
            if (v.op == EOp::Param)         param = true;
        }

        if (tex)
        {
            // set 2, binding 0 — bindless combined-image-sampler array (uTex[]).
            rdesc::EDescriptorBindingInfo b2;
            b2.set = 2; b2.binding = 0; b2.count = 1;
            b2.type = rdesc::EDescriptorType::COMBINED_IMAGE_SAMPLER;
            b2.name = "uTex";
            rdesc::DescriptorSetLayoutInfo s2; s2.set = 2; s2.bindings.push_back(b2);
            out.info.sets.push_back(std::move(s2));
        }
        if (tex || param)
        {
            // set 4, binding 4 — Graph-family per-material SSBO (uMats). 统一 binding 4
            // （任何 tex/param 都是 Graph family）—— 与 emitter 的 uMats 绑定一致，且修了
            // 旧 Compile.cpp 用 graph_mode=param 路由导致 texture-only 图 binding 漂移的
            // latent bug（GLSL 写 binding 4、ShaderInfo 却报 shading_model 序号）。
            rdesc::EDescriptorBindingInfo b4;
            b4.set = 4; b4.binding = 4; b4.count = 1;
            b4.type = rdesc::EDescriptorType::STORAGE_BUFFER;
            b4.name = "uMats"; b4.writable = false;
            rdesc::DescriptorSetLayoutInfo s4; s4.set = 4; s4.bindings.push_back(b4);
            out.info.sets.push_back(std::move(s4));
        }
        return out;
    }

} // namespace lux::shadergen::glsl
