#pragma once
// ============================================================================
//  graph_test_helpers.hpp — shared test helpers for the shadergen render tests
//  (migrated off material_graph_glsl in milestone ⑤). Every test material is a
//  GRAPH material: author a small neutral graph, compile ONE pass to SPIR-V +
//  reflected ShaderInfo (CPU only — no RenderSession), then the test does its own
//  compileShader + uploadGraphMaterial(data, gbuffer_shader, forward_shader) via
//  its existing await() loop (the canonical R1 per-material-PSO route).
//
//  CPU-only by design: links only shadergen (neutral core) + shadergen_glsl (GLSL
//  backend), so it carries no RenderSession/window coupling. ShaderGen #includes
//  the real render SSOT, so compileGraph passes LUX_RENDER_SHADERS_DIR (cmake-
//  injected) as the shaderc include search root.
// ============================================================================

#include <lux/engine/description/material_graph/MaterialGraph.hpp>
#include <lux/engine/shadergen/material/MaterialLowering.hpp>
#include <lux/engine/description/material_graph/Nodes.hpp>
#include <lux/engine/shadergen/glsl/Backend.hpp>
#include <lux/engine/description/MaterialGraphContract.hpp>
#include <lux/engine/description/ShaderInfo.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lux::mgtest
{
    /// Author a neutral material graph baking CONSTANT surface attributes:
    ///   - BaseColor (r,g,b);
    ///   - for the PBR shading model, Metallic + Roughness constants;
    ///   - optionally Emissive = BaseColor * @p emissive_scale (light-marker / unlit glow);
    ///   - optionally an Input(WorldNormal) node driving the surface normal (so a lit
    ///     material shows a continuous GGX gradient instead of a flat shade).
    /// The colour/params are baked into the SHADER (Constants), so distinct colours =>
    /// distinct SPIR-V => distinct per-material PSOs under R1.
    inline lux::rdesc::MaterialGraph makeColorGraph(
        float r, float g, float b,
        lux::rdesc::EMaterialShadingModel sm = lux::rdesc::EMaterialShadingModel::PbrMetallicRoughness,
        float metallic           = 0.0f,
        float roughness          = 0.6f,
        bool  world_normal_input = false,
        float emissive_scale     = 0.0f)
    {
        using namespace lux::rdesc;
        namespace rd = lux::rdesc;

        MaterialGraph graph;
        graph.shading_model = sm;

        const auto konst = [&](rd::EMatValueType t, float x, float y = 0.f, float z = 0.f) -> node_id
        {
            auto n = std::make_unique<ConstantNode>();
            n->value_type = t;
            n->value[0] = x; n->value[1] = y; n->value[2] = z; n->value[3] = 0.f;
            return graph.addNode(std::move(n));
        };

        const node_id o = graph.addNode(std::make_unique<OutputSurfaceNode>());
        graph.connect(konst(rd::EMatValueType::Vec3, r, g, b), 0, o,
                      static_cast<std::uint32_t>(rd::EMaterialAttribute::BaseColor));

        if (sm == rd::EMaterialShadingModel::PbrMetallicRoughness)
        {
            graph.connect(konst(rd::EMatValueType::Float, metallic), 0, o,
                          static_cast<std::uint32_t>(rd::EMaterialAttribute::Metallic));
            graph.connect(konst(rd::EMatValueType::Float, roughness), 0, o,
                          static_cast<std::uint32_t>(rd::EMaterialAttribute::Roughness));
        }
        if (emissive_scale > 0.f)
            graph.connect(konst(rd::EMatValueType::Vec3, r * emissive_scale,
                                g * emissive_scale, b * emissive_scale), 0, o,
                          static_cast<std::uint32_t>(rd::EMaterialAttribute::Emissive));
        if (world_normal_input)
        {
            auto in = std::make_unique<InputNode>();
            in->input = rd::EMaterialInput::WorldNormal;
            const node_id n = graph.addNode(std::move(in));
            graph.connect(n, 0, o,
                          static_cast<std::uint32_t>(rd::EMaterialAttribute::NormalTS));
        }
        return graph;
    }

    /// Lower + compile ONE pass of @p graph to SPIR-V words + (optional) reflected
    /// ShaderInfo via ShaderGen. CPU only (no device). Returns false + fills @p err
    /// on failure. ShaderGen #includes the real render SSOT, resolved against the
    /// cmake-injected LUX_RENDER_SHADERS_DIR.
    inline lux::cxx::expected<lux::shadergen::glsl::CompiledShader, std::string>
    compileGraph(const lux::rdesc::MaterialGraph& graph, lux::rdesc::EMaterialPass pass)
    {
        namespace sgm = lux::shadergen::material;
        namespace sgg = lux::shadergen::glsl;
        auto lr = sgm::lowerMaterial(graph);
        if (!lr) return lux::cxx::unexpected(lr.error());
        sgg::EmitParams p;
        p.pass          = pass;
        p.shading_model = lr->shading_model;
        p.alpha_mode    = lr->alpha_mode;
        p.alpha_cutoff  = lr->alpha_cutoff;
        return sgg::compileToSpirv(lr->ir, p, { LUX_RENDER_SHADERS_DIR });
    }

    /// Convenience: compile + serialize ShaderInfo to bytes (the form the render
    /// tests feed to compileShader). Thin wrapper over compileGraph.
    /// 编译产物的字节形式（render 测试喂给 compileShader 的形态）。
    struct CompiledBytes
    {
        std::vector<std::uint32_t> spirv;
        std::vector<std::byte>     info_bytes;
    };

    inline lux::cxx::expected<CompiledBytes, std::string>
    compileGraphPass(const lux::rdesc::MaterialGraph& graph, lux::rdesc::EMaterialPass pass)
    {
        auto cs = compileGraph(graph, pass);
        if (!cs) return lux::cxx::unexpected(cs.error());
        return CompiledBytes{ std::move(cs->spirv), lux::rdesc::ShaderInfo::serialize(cs->info) };
    }

} // namespace lux::mgtest
