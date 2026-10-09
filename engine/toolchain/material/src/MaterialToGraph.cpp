// =============================================================================
//  MaterialToGraph.cpp — ImportedMaterialDesc -> MaterialGraph conversion
//  (migrated into ShaderGen from the retired material_graph neutral core;
//  namespace lux::material::compiler)
// =============================================================================

#include <lux/engine/material/BuiltinMaterialNodes.hpp>
#include <lux/engine/material/graph/MaterialGraph.hpp>
#include <lux/engine/material/import/MaterialToGraph.hpp>

#include <lux/engine/material/ImportedMaterialDescription.hpp>

#include <cmath>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>

namespace lux::material::compiler
{
    using namespace ::lux::material;

    namespace
    {
        using EVT = ::lux::material::EValueType;
        using EAttr = ::lux::material::EMaterialAttribute;

        constexpr float kEps = 1e-6f;

        bool isOne(float v) noexcept
        {
            return v > 1.f - kEps && v < 1.f + kEps;
        } // occlusion.strength only

        // A small helper set for building all the SampleTexture / weaving
        // nodes into a single graph.
        struct Builder
        {
            MaterialGraph& g;
            NodeId uv;                                         // the shared Input(UV0)
            NodeId out;                                        // the single OutputSurface
            std::unordered_map<uint32_t, NodeId> sample_cache; // texture_index -> SampleTexture node

            MaterialNodeCatalog catalog;
            std::optional<std::string> error;

            explicit Builder(MaterialGraph& graph) : g(graph)
            {
                const auto registrations = materialBuiltinRegistrations();
                if (!catalog.add(registrations))
                {
                    std::terminate();
                }
                uv = add(MaterialInput{}, "Input");
                out = add(MaterialOutputSurface{}, "Output Surface");
            }

            template <class T> NodeId add(T value, std::string name)
            {
                if (error)
                {
                    return {};
                }
                auto definition = catalog.find(lux::graph::nodeTypeId(T::TypeName));
                auto payload = definition->create();
                if (!payload)
                {
                    error = std::move(payload.error().message);
                    return {};
                }
                *payload->get<T>() = std::move(value);
                auto inserted = g.addNode(MaterialNode{std::move(definition), std::move(name), std::move(*payload)});
                if (!inserted)
                {
                    error = std::visit(
                        [](const auto& failure) -> std::string
                        {
                            if constexpr (std::is_same_v<std::decay_t<decltype(failure)>, MaterialCompileFailure>)
                            {
                                return failure.message;
                            }
                            else
                            {
                                return "imported material topology admission failed: " +
                                       std::to_string(static_cast<unsigned>(failure.code));
                            }
                        },
                        inserted.error()
                    );
                    return {};
                }
                return *inserted;
            }

            void connect(NodeId from, NodeId to, std::uint32_t input = 0)
            {
                if (error)
                {
                    return;
                }
                constexpr lux::graph::PinSemanticId Output{(std::uint64_t{1} << 63) | 1};
                const auto source = g.pinId(from, Output);
                const auto target = g.pinId(to, lux::graph::PinSemanticId{input + 1});
                if (!g.connect(source, target))
                {
                    error = "imported material pin connection failed";
                }
            }

            NodeId constant(EVT type, float x, float y = 0, float z = 0, float w = 0)
            {
                return add(MaterialConstant{{x, y, z, w}, type}, "Constant");
            }

            // W4: declare an OVERRIDABLE factor as a PARAM (-> set-4 SSBO lane) instead
            // of a Constant baked into the SPIR-V. With FIXED names + a stable slot order
            // (the call sites below), two materials of the same payload type that differ
            // only in factor VALUES produce BYTE-IDENTICAL SPIR-V (the value lives in the
            // SSBO, not the shader) -> dedup to ONE shared PSO, and a Material Instance can
            // override the value per-instance. Structural literals stay Constant.
            NodeId param(const std::string& name, EVT t, float x, float y = 0, float z = 0, float w = 0)
            {
                ParamSlotDecl decl;
                decl.name = name;
                decl.type = t;
                decl.dflt[0] = x;
                decl.dflt[1] = y;
                decl.dflt[2] = z;
                decl.dflt[3] = w;
                const uint32_t slot = static_cast<uint32_t>(g.param_slots.size());
                g.param_slots.push_back(std::move(decl));
                return add(MaterialParameter{slot, t}, "Param");
            }

            // Samples a given texture_index (the same texture shares one
            // SampleTexture node), returning a vec4 node.
            NodeId sample(uint32_t tex_index)
            {
                if (auto it = sample_cache.find(tex_index); it != sample_cache.end())
                {
                    return it->second;
                }
                const NodeId id = add(MaterialSampleTexture{tex_index}, "Sample Texture");
                connect(uv, id); // uv -> sample
                sample_cache.emplace(tex_index, id);
                return id;
            }

            // Swizzles `out_type` out of a vec4 source; components specifies
            // which source component each output channel takes.
            NodeId swizzle(NodeId src, EVT out_type, uint8_t c0, uint8_t c1 = 0, uint8_t c2 = 0, uint8_t c3 = 0)
            {
                const NodeId id = add(MaterialSwizzle{EVT::VEC4, out_type, {c0, c1, c2, c3}}, "Swizzle");
                connect(src, id);
                return id;
            }

            NodeId mul(NodeId a, NodeId b, EVT t)
            {
                const NodeId id = add(MaterialMath{EMathOp::MUL, t}, "Math");
                connect(a, id);
                connect(b, id, 1);
                return id;
            }

            void bind(EAttr attr, NodeId v)
            {
                connect(v, out, static_cast<uint32_t>(attr));
            }

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
    lux::cxx::expected<MaterialGraph, std::string> materialToGraph(const ::lux::material::ImportedMaterialDescription& d
    )
    {
        const auto invalid_reference = [](const auto& reference) noexcept
        { return reference && reference->texture.isNull(); };
        const bool invalid_texture = invalid_reference(d.base_color_texture) || invalid_reference(d.normal_texture) ||
                                     invalid_reference(d.metallic_roughness_texture) ||
                                     invalid_reference(d.occlusion_texture) || invalid_reference(d.emissive_texture);
        const bool invalid_factor =
            !d.base_color.allFinite() || !std::isfinite(d.opacity) || !std::isfinite(d.metallic) ||
            !std::isfinite(d.roughness) || !std::isfinite(d.normal_scale) || !std::isfinite(d.occlusion_strength) ||
            !d.emissive.allFinite() || !std::isfinite(d.emissive_intensity) || !std::isfinite(d.alpha_cutoff);
        if (invalid_texture || invalid_factor)
        {
            return lux::cxx::unexpected(std::string{"invalid imported material"});
        }

        MaterialGraph g;
        Builder b(g);
        g.shading_model = ::lux::rdesc::ELightingTechnique::PBR_METALLIC_ROUGHNESS;

        const auto slot = [&g](
                              const std::optional<::lux::material::ImportedTextureReference>& texture,
                              std::string_view name
                          ) -> std::optional<std::uint32_t>
        {
            if (!texture)
            {
                return std::nullopt;
            }
            if (texture->texture.isNull())
            {
                return std::nullopt;
            }
            for (std::uint32_t index = 0U; index < g.texture_slots.size(); ++index)
            {
                if (g.texture_slots[index].texture == texture->texture)
                {
                    return index;
                }
            }
            const auto index = static_cast<std::uint32_t>(g.texture_slots.size());
            g.texture_slots.push_back(TextureSlotDecl{std::string{name}, texture->texture});
            return index;
        };

        const auto base_color_texture = slot(d.base_color_texture, "base-color");
        const auto normal_texture = slot(d.normal_texture, "normal");
        const auto metallic_roughness_texture = slot(d.metallic_roughness_texture, "metallic-roughness");
        const auto occlusion_texture = slot(d.occlusion_texture, "occlusion");
        const auto emissive_texture = slot(d.emissive_texture, "emissive");

        // color3 factor [* tex.rgb] as a Param (mirrors Builder::color3).
        const auto color3 = [&](const std::string& name, const Eigen::Vector3f& v, std::optional<uint32_t> tex
                            ) -> NodeId
        {
            const NodeId factor = b.param(name, EVT::VEC3, v.x(), v.y(), v.z());
            if (tex)
            {
                return b.mul(factor, b.swizzle(b.sample(*tex), EVT::VEC3, 0, 1, 2), EVT::VEC3);
            }
            return factor;
        };
        // scalar factor [* tex[channel]] as a Param (mirrors Builder::scalar).
        const auto scalar = [&](const std::string& name, float val, std::optional<uint32_t> tex, uint8_t channel
                            ) -> NodeId
        {
            const NodeId factor = b.param(name, EVT::FLOAT, val);
            if (tex)
            {
                return b.mul(factor, b.swizzle(b.sample(*tex), EVT::FLOAT, channel), EVT::FLOAT);
            }
            return factor;
        };

        b.bind(EAttr::BASE_COLOR, color3("BaseColor", d.base_color, base_color_texture));

        // Legacy (Phong) imports collapse to PBR with metallic=0 / roughness=0.5.
        const float metallic = d.legacy_lit ? 0.0f : d.metallic;
        const float roughness = d.legacy_lit ? 0.5f : d.roughness;
        b.bind(EAttr::METALLIC, scalar("Metallic", metallic, metallic_roughness_texture, 0));
        b.bind(EAttr::ROUGHNESS, scalar("Roughness", roughness, metallic_roughness_texture, 1));

        if (normal_texture)
        {
            // normal map: sample -> rgb -> DecodeNormal (2*rgb-1 -> tangent normal) ->
            // TbnTransform (tangent -> WORLD via T/B/N) -> NormalTS. The TbnTransform was
            // MISSING here, so the tangent-space normal was bound directly as the world
            // normal -> ~constant world normal -> flat/unlit shading on every imported
            // normal-mapped material. (engine path mirrors lighting_tbn::calculateWorldNormal)
            const NodeId rgb = b.swizzle(b.sample(*normal_texture), EVT::VEC3, 0, 1, 2);
            const NodeId decn = b.add(MaterialDecodeNormal{}, "Decode Normal");
            b.connect(rgb, decn);
            const NodeId tbn = b.add(MaterialTbnTransform{}, "TBN Transform");
            b.connect(decn, tbn);
            b.bind(EAttr::NORMAL_TS, tbn);
        }
        if (occlusion_texture)
        {
            const NodeId ao = b.swizzle(b.sample(*occlusion_texture), EVT::FLOAT, 0);
            b.bind(
                EAttr::AMBIENT_OCCLUSION,
                isOne(d.occlusion_strength) ? ao : b.mul(b.constant(EVT::FLOAT, d.occlusion_strength), ao, EVT::FLOAT)
            );
        }

        // emissive = factor(=value*intensity) [* tex.rgb], Param (always bound).
        {
            const Eigen::Vector3f scaled = d.emissive * d.emissive_intensity;
            const NodeId factor = b.param("Emissive", EVT::VEC3, scaled.x(), scaled.y(), scaled.z());
            if (emissive_texture)
            {
                b.bind(
                    EAttr::EMISSIVE,
                    b.mul(factor, b.swizzle(b.sample(*emissive_texture), EVT::VEC3, 0, 1, 2), EVT::VEC3)
                );
            }
            else
            {
                b.bind(EAttr::EMISSIVE, factor);
            }
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
        if (d.alpha_mode != ::lux::rdesc::EAlphaMode::OPAQUE_SURFACE && base_color_texture)
        {
            b.bind(EAttr::OPACITY, scalar("Opacity", d.opacity, base_color_texture, 3));
        }
        else
        {
            b.bind(EAttr::OPACITY, b.param("Opacity", EVT::FLOAT, d.opacity));
        }

        // render state (W3a): alpha mode / cutoff / double-sided 1:1.
        g.render_state.alpha_mode = d.alpha_mode;
        g.render_state.alpha_cutoff = d.alpha_cutoff;
        g.render_state.double_sided = d.double_sided;

        if (b.error)
        {
            return cxx::unexpected(std::move(*b.error));
        }
        return g;
    }

} // namespace lux::material::compiler
