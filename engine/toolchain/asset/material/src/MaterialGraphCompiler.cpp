#include <lux/engine/toolchain/asset/material/MaterialGraphCompiler.hpp>

#include <lux/engine/authoring/assets/material/MaterialGraph.hpp>
#include <lux/engine/toolchain/asset/material/MaterialLowering.hpp>
#include <lux/engine/authoring/assets/material/Nodes.hpp>
#include <lux/engine/toolchain/shader/Backend.hpp>
#include <lux/engine/toolchain/asset/material/MaterialShaderPaths.hpp>
#include <lux/engine/resource/asset/material/MaterialAsset.hpp>
#include <lux/engine/resource/asset/material/MaterialSerDeser.hpp>
#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/authoring/assets/MaterialGraphDocument.hpp>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace lux::toolchain
{
    namespace sgm   = lux::shadergen::material;
    namespace sgg   = lux::shadergen::glsl;
    namespace rdesc = lux::rdesc;

    // ── Author a neutral PBR graph in memory (no rdesc::Material) ────────────
    rdesc::MaterialGraph makeNeutralPbrGraph(float r, float g, float b,
                                          float metallic, float roughness)
    {
        using EVT   = rdesc::EMatValueType;
        using EAttr = rdesc::EMaterialAttribute;

        rdesc::MaterialGraph graph;
        graph.shading_model = rdesc::EMaterialShadingModel::PbrMetallicRoughness;

        const rdesc::node_id out = graph.addNode(std::make_unique<rdesc::OutputSurfaceNode>());

        // Declare each overridable factor as a PARAM lane (mirrors MaterialToGraph:
        // fixed names + stable slot order => structurally identical graphs share a
        // shader fingerprint / PSO, and an instance can override the value).
        const auto addParam = [&](const char* name, EVT type,
                                  float x, float y, float z) -> rdesc::node_id
        {
            rdesc::ParamSlotDecl decl;
            decl.name    = name;
            decl.type    = type;
            decl.dflt[0] = x; decl.dflt[1] = y; decl.dflt[2] = z; decl.dflt[3] = 0.f;
            const std::uint32_t slot = static_cast<std::uint32_t>(graph.param_slots.size());
            graph.param_slots.push_back(std::move(decl));
            auto n = std::make_unique<rdesc::ParamNode>(type);
            n->param_slot = slot;
            return graph.addNode(std::move(n));
        };

        const rdesc::node_id base = addParam("BaseColor", EVT::Vec3,  r, g, b);
        const rdesc::node_id met  = addParam("Metallic",  EVT::Float, metallic, 0.f, 0.f);
        const rdesc::node_id rgh  = addParam("Roughness", EVT::Float, roughness, 0.f, 0.f);

        graph.connect(base, 0, out, static_cast<std::uint32_t>(EAttr::BaseColor));
        graph.connect(met,  0, out, static_cast<std::uint32_t>(EAttr::Metallic));
        graph.connect(rgh,  0, out, static_cast<std::uint32_t>(EAttr::Roughness));
        // Opacity / normal / emissive / AO stay unbound -> contract defaults.
        return graph;
    }

    // ── Author an emissive PBR graph in memory (glowing "bulb" material) ─────
    rdesc::MaterialGraph makeEmissivePbrGraph(float r, float g, float b)
    {
        using EVT   = rdesc::EMatValueType;
        using EAttr = rdesc::EMaterialAttribute;

        rdesc::MaterialGraph graph;
        graph.shading_model = rdesc::EMaterialShadingModel::PbrMetallicRoughness;

        const rdesc::node_id out = graph.addNode(std::make_unique<rdesc::OutputSurfaceNode>());

        const auto addParam = [&](const char* name, EVT type,
                                  float x, float y, float z) -> rdesc::node_id
        {
            rdesc::ParamSlotDecl decl;
            decl.name    = name;
            decl.type    = type;
            decl.dflt[0] = x; decl.dflt[1] = y; decl.dflt[2] = z; decl.dflt[3] = 0.f;
            const std::uint32_t slot = static_cast<std::uint32_t>(graph.param_slots.size());
            graph.param_slots.push_back(std::move(decl));
            auto n = std::make_unique<rdesc::ParamNode>(type);
            n->param_slot = slot;
            return graph.addNode(std::move(n));
        };

        // Near-black base + metallic 0 / roughness 1 so direct lighting barely
        // touches it; the Emissive lane is what the eye reads. HDR-boost the
        // emission (×4) so the bulb stays bright + saturated through tonemapping
        // and clearly reads as a light SOURCE, not a dim grey ball.
        constexpr float kEmissiveBoost = 4.f;
        const rdesc::node_id base = addParam("BaseColor", EVT::Vec3,  0.02f, 0.02f, 0.02f);
        const rdesc::node_id met  = addParam("Metallic",  EVT::Float, 0.f, 0.f, 0.f);
        const rdesc::node_id rgh  = addParam("Roughness", EVT::Float, 1.f, 0.f, 0.f);
        const rdesc::node_id emi  = addParam("Emissive",  EVT::Vec3,
                                             r * kEmissiveBoost,
                                             g * kEmissiveBoost,
                                             b * kEmissiveBoost);

        graph.connect(base, 0, out, static_cast<std::uint32_t>(EAttr::BaseColor));
        graph.connect(met,  0, out, static_cast<std::uint32_t>(EAttr::Metallic));
        graph.connect(rgh,  0, out, static_cast<std::uint32_t>(EAttr::Roughness));
        graph.connect(emi,  0, out, static_cast<std::uint32_t>(EAttr::Emissive));
        return graph;
    }

    void flattenMaterialRuntimeValues(
        const rdesc::MaterialGraph& graph,
        lux::asset::MaterialData&   payload
    ) noexcept
    {
        payload.parameter_count = static_cast<std::uint32_t>(std::min<std::size_t>(
            graph.param_slots.size(),
            lux::asset::MaterialData::kMaxParams
        ));
        for (std::uint32_t i = 0; i < payload.parameter_count; ++i)
            std::copy_n(
                graph.param_slots[i].dflt,
                payload.parameter_defaults[i].size(),
                payload.parameter_defaults[i].begin()
            );
        payload.alpha_mode = static_cast<std::uint32_t>(
            graph.render_state.alpha_mode
        );
        payload.double_sided = graph.render_state.double_sided;
    }

    // ── Lower + compile a graph into a ready-to-use payload (no asset/disk) ──
    lux::cxx::expected<lux::asset::MaterialData, std::string> compileGraphToPayload(
        const rdesc::MaterialGraph&                graph,
        const std::vector<lux::asset::asset_id_t>& slot_texture_ids)
    {
        // 1) lower + compile BOTH passes the engine drives a graph family through.
        //    ShaderGen #includes the REAL render SSOT (gbuffer_encode / lighting_common
        //    / brdf), resolved by its shaderc Includer against render_shaders_dir — so
        //    the bake depends on the render shader source tree (vs the old backend's
        //    inlined self-contained copies).
        auto lr = sgm::lowerMaterial(graph);
        if (!lr) return lux::cxx::unexpected("lower: " + lr.error());

        const std::vector<std::string> inc = {
            material_shaders_emitted_dir,
            material_shaders_source_dir};
        const auto compilePass = [&](rdesc::EMaterialPass pass)
            -> lux::cxx::expected<sgg::CompiledShader, std::string>
        {
            sgg::EmitParams p;
            p.pass          = pass;
            p.shading_model = lr->shading_model;
            p.alpha_mode    = lr->alpha_mode;
            p.alpha_cutoff  = lr->alpha_cutoff;
            return sgg::compileToSpirv(lr->ir, p, inc);
        };

        lux::asset::MaterialData payload;
        auto gb = compilePass(rdesc::EMaterialPass::GBuffer);
        if (!gb) return lux::cxx::unexpected("gbuffer spirv: " + gb.error());
        payload.gbuffer_spirv = std::move(gb->spirv);
        payload.gbuffer_info  = std::move(gb->info);
        auto fwd = compilePass(rdesc::EMaterialPass::Forward);
        if (!fwd) return lux::cxx::unexpected("forward spirv: " + fwd.error());
        payload.forward_spirv = std::move(fwd->spirv);
        payload.forward_info  = std::move(fwd->info);

        // 2) flatten the runtime values.  The authored graph is stored separately
        //    by the authoring layer and is never part of MaterialData/Player.
        flattenMaterialRuntimeValues(graph, payload);

        // 3) texture slot bindings as ASSET UUIDs (slot i == graph texture slot i).
        //    Re-resolved to bindless on load — never persist transient bindless indices.
        for (std::uint32_t s = 0;
             s < lux::asset::MaterialData::kMaxTextures && s < slot_texture_ids.size();
             ++s)
            payload.texture_slot_ids[s] = slot_texture_ids[s];
        return payload;
    }

    lux::cxx::expected<lux::asset::asset_id_t, std::string> bakeGraphMaterial(
        const std::shared_ptr<lux::asset::AssetManager>& mgr,
        const rdesc::MaterialGraph&                      graph,
        const std::vector<lux::asset::asset_id_t>&       slot_texture_ids,
        std::string_view                                 display_name,
        const std::filesystem::path&                     dest,
        std::string_view                                 seed)
    {
        if (!mgr) return lux::cxx::unexpected(std::string("asset manager is null"));

        // 1-4) lower + compile + fill the payload (shared with the producers that
        //      need a ready payload without an on-disk asset).
        auto payload_exp = compileGraphToPayload(graph, slot_texture_ids);
        if (!payload_exp) return lux::cxx::unexpected(std::move(payload_exp.error()));
        auto payload = std::make_unique<lux::asset::MaterialData>(std::move(*payload_exp));

        // 5) create (seeded => deterministic; empty seed => random) + name + register + export.
        auto asset = mgr->createAssetSeeded<lux::asset::MaterialAsset>(seed, std::move(payload));
        lux::authoring::attachMaterialGraph(*asset, graph);
        if (auto* mi = asset->mutableInfo(); mi && !display_name.empty())
        {
            const std::size_t n = std::min(display_name.size(), sizeof(mi->display_name) - 1);
            std::memcpy(mi->display_name, display_name.data(), n);
            mi->display_name[n] = '\0';
        }
        const auto id = asset->id();
        if (!mgr->registerAsset(std::move(asset)))
            return lux::cxx::unexpected(std::string("registerAsset failed (id collision?)"));

        lux::asset::MaterialSerDeser ser(mgr);
        if (const auto ec = ser.exportAsLuxAsset(id, dest);
            ec != lux::asset::EAssetError::SUCCESS)
            return lux::cxx::unexpected("write failed (err=" + std::to_string(static_cast<int>(ec)) + ")");

        return id;
    }

} // namespace lux::toolchain
