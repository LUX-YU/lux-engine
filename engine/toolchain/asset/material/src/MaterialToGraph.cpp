// =============================================================================
//  MaterialToGraph.cpp — ImportedMaterialDesc -> MaterialGraph conversion
//  (migrated into ShaderGen from the retired material_graph neutral core;
//  namespace lux::shadergen::material)
// =============================================================================

#include <lux/engine/toolchain/asset/material/MaterialToGraph.hpp>
#include <lux/engine/authoring/assets/material/MaterialGraph.hpp>
#include <lux/engine/authoring/assets/material/Nodes.hpp>

#include <lux/engine/description/ImportedMaterialDesc.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>

namespace lux::shadergen::material
{
    using namespace ::lux::rdesc;  // the data model (MaterialGraph/Node/node types/slot declarations) lives in description

    namespace
    {
        namespace rdesc = ::lux::rdesc;
        using EVT  = rdesc::EMatValueType;
        using EAttr = rdesc::EMaterialAttribute;

        constexpr float kEps = 1e-6f;
        bool isOne(float v)  noexcept { return v > 1.f - kEps && v < 1.f + kEps; }  // occlusion.strength only

        // A small helper set for building all the SampleTexture / weaving
        // nodes into a single graph.
        struct Builder
        {
            MaterialGraph& g;
            node_id        uv;   // the shared Input(UV0)
            node_id        out;  // the single OutputSurface
            std::unordered_map<uint32_t, node_id> sample_cache;  // texture_index -> SampleTexture node

            explicit Builder(MaterialGraph& graph) : g(graph)
            {
                uv  = g.addNode(std::make_unique<InputNode>());                // defaults to UV0
                out = g.addNode(std::make_unique<OutputSurfaceNode>());
            }

            // Ensures the graph has a texture slot covering texture_index
            // (graph slot index == texture_index).
            void ensureSlot(uint32_t idx)
            {
                while (g.texture_slots.size() <= idx)
                    g.texture_slots.push_back(TextureSlotDecl{ "tex" + std::to_string(g.texture_slots.size()) });
            }

            node_id constant(EVT t, float x, float y = 0, float z = 0, float w = 0)
            {
                auto n = std::make_unique<ConstantNode>();
                n->setType(t);
                n->value[0] = x; n->value[1] = y; n->value[2] = z; n->value[3] = w;
                return g.addNode(std::move(n));
            }

            // W4: declare an OVERRIDABLE factor as a PARAM (-> set-4 SSBO lane) instead
            // of a Constant baked into the SPIR-V. With FIXED names + a stable slot order
            // (the call sites below), two materials of the same payload type that differ
            // only in factor VALUES produce BYTE-IDENTICAL SPIR-V (the value lives in the
            // SSBO, not the shader) -> dedup to ONE shared PSO, and a Material Instance can
            // override the value per-instance. Structural literals stay Constant.
            node_id param(const std::string& name, EVT t, float x, float y = 0, float z = 0, float w = 0)
            {
                ParamSlotDecl decl;
                decl.name    = name;
                decl.type    = t;
                decl.dflt[0] = x; decl.dflt[1] = y; decl.dflt[2] = z; decl.dflt[3] = w;
                const uint32_t slot = static_cast<uint32_t>(g.param_slots.size());
                g.param_slots.push_back(std::move(decl));
                auto n = std::make_unique<ParamNode>(t);
                n->param_slot = slot;
                return g.addNode(std::move(n));
            }

            // Samples a given texture_index (the same texture shares one
            // SampleTexture node), returning a vec4 node.
            node_id sample(uint32_t tex_index)
            {
                if (auto it = sample_cache.find(tex_index); it != sample_cache.end())
                    return it->second;
                ensureSlot(tex_index);
                auto n = std::make_unique<SampleTextureNode>();
                n->texture_slot = tex_index;
                const node_id id = g.addNode(std::move(n));
                g.connect(uv, 0, id, 0);   // uv -> sample
                sample_cache.emplace(tex_index, id);
                return id;
            }

            // Swizzles `out_type` out of a vec4 source; components specifies
            // which source component each output channel takes.
            node_id swizzle(node_id src, EVT out_type, uint8_t c0, uint8_t c1 = 0, uint8_t c2 = 0, uint8_t c3 = 0)
            {
                auto n = std::make_unique<SwizzleNode>(EVT::Vec4, out_type);
                n->components[0] = c0; n->components[1] = c1; n->components[2] = c2; n->components[3] = c3;
                const node_id id = g.addNode(std::move(n));
                g.connect(src, 0, id, 0);
                return id;
            }

            node_id mul(node_id a, node_id b, EVT t)
            {
                auto n = std::make_unique<MathNode>(EMathOp::Mul);
                n->setOperandType(t);
                const node_id id = g.addNode(std::move(n));
                g.connect(a, 0, id, 0);
                g.connect(b, 0, id, 1);
                return id;
            }

            void bind(EAttr attr, node_id v) { g.connect(v, 0, out, static_cast<uint32_t>(attr)); }
            // (The rdesc-closure helpers color3/scalar/bindNormal/bindEmissive/bindOpacity
            //  + the materialToGraph(rdesc::Material) overload were retired in W5c — the
            //  closure rdesc::Material is gone; the ImportedMaterialDesc overload below
            //  is the sole import path, built from the low-level Builder primitives.)
        };
    } // namespace

    // W5b: ImportedMaterialDesc (flat POD, closure-free) -> MaterialGraph. Produces
    // a STRUCTURALLY IDENTICAL graph to the rdesc::Material PBR path above (same Param
    // names + slot order + texture-slot indices), so an imported material dedups
    // byte-for-byte against an equivalent hand-authored one.
    lux::cxx::expected<MaterialGraph, std::string>
    materialToGraph(const rdesc::ImportedMaterialDesc& d)
    {
        MaterialGraph g;
        Builder b(g);
        g.shading_model = rdesc::ELightingTechnique::PbrMetallicRoughness;

        const auto idx = [](const std::optional<rdesc::ImportedTextureRef>& t)
            -> std::optional<uint32_t> { return t ? std::optional<uint32_t>(t->texture_index) : std::nullopt; };

        // color3 factor [* tex.rgb] as a Param (mirrors Builder::color3).
        const auto color3 = [&](const std::string& name, const Eigen::Vector3f& v,
                                std::optional<uint32_t> tex) -> node_id
        {
            const node_id factor = b.param(name, EVT::Vec3, v.x(), v.y(), v.z());
            if (tex)
                return b.mul(factor, b.swizzle(b.sample(*tex), EVT::Vec3, 0, 1, 2), EVT::Vec3);
            return factor;
        };
        // scalar factor [* tex[channel]] as a Param (mirrors Builder::scalar).
        const auto scalar = [&](const std::string& name, float val,
                                std::optional<uint32_t> tex, uint8_t channel) -> node_id
        {
            const node_id factor = b.param(name, EVT::Float, val);
            if (tex)
                return b.mul(factor, b.swizzle(b.sample(*tex), EVT::Float, channel), EVT::Float);
            return factor;
        };

        b.bind(EAttr::BaseColor, color3("BaseColor", d.base_color, idx(d.base_color_tex)));

        // Legacy (Phong) imports collapse to PBR with metallic=0 / roughness=0.5.
        const float metallic  = d.is_legacy ? 0.0f : d.metallic;
        const float roughness = d.is_legacy ? 0.5f : d.roughness;
        b.bind(EAttr::Metallic,  scalar("Metallic",  metallic,  idx(d.metallic_roughness_tex), 2)); // glTF ORM .b
        b.bind(EAttr::Roughness, scalar("Roughness", roughness, idx(d.metallic_roughness_tex), 1)); // .g

        if (d.normal_tex)
        {
            // normal map: sample -> rgb -> DecodeNormal (2*rgb-1 -> tangent normal) ->
            // TbnTransform (tangent -> WORLD via T/B/N) -> NormalTS. The TbnTransform was
            // MISSING here, so the tangent-space normal was bound directly as the world
            // normal -> ~constant world normal -> flat/unlit shading on every imported
            // normal-mapped material. (engine path mirrors lighting_tbn::calculateWorldNormal)
            const node_id rgb  = b.swizzle(b.sample(d.normal_tex->texture_index), EVT::Vec3, 0, 1, 2);
            const node_id decn = g.addNode(std::make_unique<DecodeNormalNode>());
            g.connect(rgb, 0, decn, 0);
            const node_id tbn  = g.addNode(std::make_unique<TbnTransformNode>());
            g.connect(decn, 0, tbn, 0);
            b.bind(EAttr::NormalTS, tbn);
        }
        if (d.occlusion_tex)
        {
            const node_id ao = b.swizzle(b.sample(d.occlusion_tex->texture_index), EVT::Float, 0); // .r
            b.bind(EAttr::AmbientOcclusion,
                   isOne(d.occlusion_strength)
                       ? ao
                       : b.mul(b.constant(EVT::Float, d.occlusion_strength), ao, EVT::Float));
        }

        // emissive = factor(=value*intensity) [* tex.rgb], Param (always bound).
        {
            const Eigen::Vector3f scaled = d.emissive * d.emissive_intensity;
            const node_id factor = b.param("Emissive", EVT::Vec3, scaled.x(), scaled.y(), scaled.z());
            if (d.emissive_tex)
                b.bind(EAttr::Emissive,
                       b.mul(factor, b.swizzle(b.sample(d.emissive_tex->texture_index), EVT::Vec3, 0, 1, 2), EVT::Vec3));
            else
                b.bind(EAttr::Emissive, factor);
        }

        // opacity: glTF alpha-test/blend takes the silhouette from the BASE-COLOR
        // texture's ALPHA channel. Route factor * baseColorTex.a for non-opaque
        // materials so the baked `if (opacity < cutoff) discard` (ShaderEmitter)
        // can actually cut the shape — without this, masked hair/eyelashes only
        // ever see the constant factor (never discard) and render as solid cards.
        // `scalar` reuses the SAME cached BaseColor SampleTexture node (no extra
        // sample). Opaque materials keep the plain factor (opacity is unused in
        // the opaque deferred path) so their graph/SPIR-V stays byte-identical and
        // dedups exactly as before.
        if (d.alpha_mode != rdesc::EAlphaMode::Opaque && d.base_color_tex)
            b.bind(EAttr::Opacity, scalar("Opacity", d.opacity, idx(d.base_color_tex), 3)); // .a
        else
            b.bind(EAttr::Opacity, b.param("Opacity", EVT::Float, d.opacity));

        // render state (W3a): alpha mode / cutoff / double-sided 1:1.
        g.render_state.alpha_mode   = d.alpha_mode;
        g.render_state.alpha_cutoff = d.alpha_cutoff;
        g.render_state.double_sided = d.double_sided;

        return g;
    }

} // namespace lux::shadergen::material
