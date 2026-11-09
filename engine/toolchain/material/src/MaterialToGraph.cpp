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

            NodeId math(EMathOp operation, NodeId a, NodeId b, EVT type)
            {
                const NodeId id = add(MaterialMath{operation, type}, "Math");
                connect(a, id);
                if (b != NodeId{})
                {
                    connect(b, id, 1);
                }
                return id;
            }

            NodeId mul(NodeId a, NodeId b, EVT type)
            {
                return math(EMathOp::MUL, a, b, type);
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

    // Default imported PBR has one topology. Resource identities and parameter values are data only.
    lux::cxx::expected<MaterialGraph, std::string>
    materialToGraph(const ::lux::material::ImportedMaterialDescription& d)
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

        // Fixed semantic slots. Missing inputs use Native's white texture; asset identity never changes topology.
        const auto slot = [&g](const std::optional<ImportedTextureReference>& texture, std::string name)
        {
            const auto index = static_cast<std::uint32_t>(g.texture_slots.size());
            g.texture_slots.push_back({std::move(name), texture ? texture->texture : asset::AssetId{}});
            return index;
        };
        const auto base = slot(d.base_color_texture, "base-color");
        const auto normal = slot(d.normal_texture, "normal");
        const auto metallic_roughness = slot(d.metallic_roughness_texture, "metallic-roughness");
        const auto occlusion = slot(d.occlusion_texture, "occlusion");
        const auto emissive = slot(d.emissive_texture, "emissive");
        const auto color = [&](std::string name, const Eigen::Vector3f& value, std::uint32_t texture)
        {
            return b.mul(b.param(name, EVT::VEC3, value.x(), value.y(), value.z()),
                b.swizzle(b.sample(texture), EVT::VEC3, 0, 1, 2), EVT::VEC3);
        };
        const auto scalar = [&](std::string name, float value, std::uint32_t texture, std::uint8_t channel)
        {
            return b.mul(b.param(name, EVT::FLOAT, value),
                b.swizzle(b.sample(texture), EVT::FLOAT, channel), EVT::FLOAT);
        };
        b.bind(EAttr::BASE_COLOR, color("BaseColor", d.base_color, base));
        // ModelCooker repacks glTF B/G into cooked R/G before TextureCooker creates RG8.
        b.bind(EAttr::METALLIC, scalar("Metallic", d.legacy_lit ? 0 : d.metallic, metallic_roughness, 0));
        b.bind(EAttr::ROUGHNESS, scalar("Roughness", d.legacy_lit ? 0.5F : d.roughness, metallic_roughness, 1));
        b.bind(EAttr::EMISSIVE, color("Emissive", (d.emissive * d.emissive_intensity).eval(), emissive));

        const auto one = b.constant(EVT::FLOAT, 1);
        const auto two = b.constant(EVT::FLOAT, 2);
        const auto strength = b.param("NormalScale", EVT::FLOAT, d.normal_texture ? d.normal_scale : 0);
        const auto normal_component = [&](std::uint8_t channel)
        {
            const auto sampled = b.swizzle(b.sample(normal), EVT::FLOAT, channel);
            const auto decoded = b.math(EMathOp::SUB, b.mul(sampled, two, EVT::FLOAT), one, EVT::FLOAT);
            return b.mul(decoded, strength, EVT::FLOAT);
        };
        // BC5 carries XY only. Reconstruct positive Z instead of reading the absent blue channel.
        const auto x = normal_component(0);
        const auto y = normal_component(1);
        const auto squared = b.math(EMathOp::ADD, b.mul(x, x, EVT::FLOAT), b.mul(y, y, EVT::FLOAT), EVT::FLOAT);
        const auto z_squared = b.math(EMathOp::SUB, one, squared, EVT::FLOAT);
        const auto clamped = b.math(EMathOp::MAX, z_squared, b.constant(EVT::FLOAT, 0), EVT::FLOAT);
        const auto z = b.math(EMathOp::SQRT, clamped, {}, EVT::FLOAT);
        const auto tangent_normal = b.add(MaterialConstruct{EVT::VEC3}, "Tangent Normal");
        b.connect(x, tangent_normal, 0);
        b.connect(y, tangent_normal, 1);
        b.connect(z, tangent_normal, 2);
        const auto world_normal = b.add(MaterialTbnTransform{}, "World Normal");
        b.connect(tangent_normal, world_normal);
        b.bind(EAttr::NORMAL_WORLD, world_normal);

        const auto ao = b.swizzle(b.sample(occlusion), EVT::FLOAT, 0);
        const auto difference = b.math(EMathOp::SUB, ao, one, EVT::FLOAT);
        const auto ao_strength = b.param("OcclusionStrength", EVT::FLOAT, d.occlusion_strength);
        b.bind(EAttr::AMBIENT_OCCLUSION,
            b.math(EMathOp::ADD, one, b.mul(difference, ao_strength, EVT::FLOAT), EVT::FLOAT));
        const auto opacity = scalar("Opacity", d.opacity, base, 3);
        const auto cutoff = b.param("AlphaCutoff", EVT::FLOAT, d.alpha_cutoff);
        b.bind(EAttr::OPACITY, b.math(EMathOp::ADD,
            b.math(EMathOp::SUB, opacity, cutoff, EVT::FLOAT), b.constant(EVT::FLOAT, 0.5F), EVT::FLOAT));
        g.render_state.alpha_mode = d.alpha_mode;
        g.render_state.alpha_cutoff = 0.5F;
        g.render_state.double_sided = d.double_sided;

        if (b.error)
        {
            return cxx::unexpected(std::move(*b.error));
        }
        return g;
    }

} // namespace lux::material::compiler
