// ============================================================================
//  material_graph_codec_test.cpp — binary MaterialGraphCodec round-trip proof.
//
//  The material asset now holds a CONCRETE rdesc::MaterialGraph; binary exists
//  ONLY at the I/O seam (detail::encode/decodeMaterialGraph). This proves that
//  codec is faithful + deterministic — the data-level guarantee behind a faithful
//  re-bake (the SPIR-V-byte-identical half is proven by the clone round-trip in
//  material_graph_glsl/test, which needs the GLSL compiler this resource-tier test
//  cannot link).
//
//    1. build a representative PBR graph (Input/SampleTexture/Swizzle/Param/Math/
//       Constant/OutputSurface + a texture & param slot + Mask render-state);
//    2. encode -> blob; decode -> a fresh graph; STRUCTURAL equality;
//    3. re-encode the decoded graph -> BYTE-IDENTICAL blob (faithful + determinism);
//    4. bad-magic input is rejected.
//
//  Device-free, no compiler. Self-checking: 0 = PASS, 1 = FAIL.
// ============================================================================

#include <lux/engine/authoring/assets/MaterialGraphCodec.hpp>
#include <lux/engine/authoring/assets/FlowGraphSerDeser.hpp>
#include <lux/engine/authoring/assets/MaterialGraphDocument.hpp>
#include <lux/engine/core/serialization/ByteIO.hpp>
#include <lux/engine/authoring/assets/material/Nodes.hpp>   // rdesc graph data model
#include <lux/engine/resource/asset/material/MaterialAsset.hpp>

#include <cstdint>
#include <cstring>
#include <iostream>
#include <memory>
#include <string>
#include <type_traits>
#include <vector>

using namespace lux::rdesc;
using lux::authoring::detail::encodeMaterialGraph;
using lux::authoring::detail::decodeMaterialGraph;

namespace
{
    int fails = 0;
    void check(bool cond, const char* name)
    { std::cout << (cond ? "  [PASS] " : "  [FAIL] ") << name << "\n"; if (!cond) ++fails; }

    // A representative PBR graph (dense node ids -> byte-identical re-encode holds).
    MaterialGraph buildGraph()
    {
        MaterialGraph g;
        g.shading_model = ELightingTechnique::PbrMetallicRoughness;
        g.render_state.alpha_mode   = EAlphaMode::Mask;
        g.render_state.alpha_cutoff = 0.33f;
        g.render_state.double_sided = true;

        g.texture_slots.push_back(TextureSlotDecl{ "BaseColorTex" });
        ParamSlotDecl tint;
        tint.name = "Tint";
        tint.type = EMatValueType::Vec3;
        tint.dflt[0] = 0.8f; tint.dflt[1] = 0.7f; tint.dflt[2] = 0.6f;
        g.param_slots.push_back(tint);

        const node_id uv  = g.addNode(std::make_unique<InputNode>());
        const node_id tex = g.addNode(std::make_unique<SampleTextureNode>());
        g.connect(uv, 0, tex, 0);

        const node_id sw = g.addNode(std::make_unique<SwizzleNode>(EMatValueType::Vec4, EMatValueType::Vec3));
        g.connect(tex, 0, sw, 0);

        const node_id tintN = g.addNode(std::make_unique<ParamNode>(EMatValueType::Vec3));
        static_cast<ParamNode*>(g.node(tintN))->param_slot = 0;

        const node_id mul = g.addNode(std::make_unique<MathNode>(EMathOp::Mul));
        static_cast<MathNode*>(g.node(mul))->setOperandType(EMatValueType::Vec3);
        g.connect(sw, 0, mul, 0);
        g.connect(tintN, 0, mul, 1);

        const node_id rough = g.addNode(std::make_unique<ConstantNode>());
        { auto* c = static_cast<ConstantNode*>(g.node(rough)); c->setType(EMatValueType::Float); c->value[0] = 0.35f; }

        const node_id alpha = g.addNode(std::make_unique<SwizzleNode>(EMatValueType::Vec4, EMatValueType::Float));
        static_cast<SwizzleNode*>(g.node(alpha))->components[0] = 3;  // .a
        g.connect(tex, 0, alpha, 0);

        const node_id o = g.addNode(std::make_unique<OutputSurfaceNode>());
        g.connect(mul,   0, o, static_cast<std::uint32_t>(EMaterialAttribute::BaseColor));
        g.connect(rough, 0, o, static_cast<std::uint32_t>(EMaterialAttribute::Roughness));
        g.connect(alpha, 0, o, static_cast<std::uint32_t>(EMaterialAttribute::Opacity));

        // v2: editor canvas positions (deterministic per id; bit-copied floats
        // keep the byte-identical re-encode guarantee intact).
        for (const auto& [id, np] : g.nodes())
        {
            np->ui_placed = true;
            np->ui_pos[0] = static_cast<float>(id) * 130.0f;
            np->ui_pos[1] = static_cast<float>(id) * 55.0f + 0.25f;
        }
        return g;
    }

    // Find the (kind-unique) node of @p k, or nullptr.
    const Node* findKind(const MaterialGraph& g, EMatNodeKind k)
    {
        for (const auto& [id, np] : g.nodes())
            if (np && np->kind() == k) return np.get();
        return nullptr;
    }

    // Count nodes of a kind (decode remaps ids, so compare by structure not id).
    std::size_t countKind(const MaterialGraph& g, EMatNodeKind k)
    {
        std::size_t n = 0;
        for (const auto& [id, np] : g.nodes()) if (np && np->kind() == k) ++n;
        return n;
    }

    template <class T>
    void appendPod(std::vector<std::byte>& bytes, const T& value)
    {
        static_assert(std::is_trivially_copyable_v<T>);
        const std::size_t offset = bytes.size();
        bytes.resize(offset + sizeof(value));
        std::memcpy(bytes.data() + offset, &value, sizeof(value));
    }

    std::vector<std::byte> makeMaterialImage(
        const lux::asset::AssetInfo&       asset_info,
        std::vector<std::byte>             material_info,
        const std::vector<lux::asset::Payload>& payloads = {})
    {
        const lux::asset::AssetFileHeader header{
            .magic_number = lux::asset::asset_magic_number_of<
                lux::asset::EAssetType::MATERIAL>::value,
            .version = lux::asset::current_asset_version,
            .info_offset = sizeof(lux::asset::AssetFileHeader),
            .info_size = material_info.size(),
            .data_offset = sizeof(lux::asset::AssetFileHeader) +
                material_info.size(),
            .data_size = 0u,
            .info = asset_info
        };
        std::vector<std::byte> image;
        appendPod(image, header);
        image.insert(image.end(), material_info.begin(), material_info.end());
        for (const auto& payload : payloads)
        {
            appendPod(
                image,
                lux::asset::PayloadBlockHeader{
                    payload.tag,
                    static_cast<std::uint64_t>(payload.data.size())
                }
            );
            image.insert(
                image.end(),
                payload.data.begin(),
                payload.data.end()
            );
        }
        return image;
    }
}

int main()
{
    std::cout << std::unitbuf;
    std::cout << "=== material_graph_codec_test ===\n";

    const MaterialGraph g0 = buildGraph();

    const std::vector<std::byte> blob1 = encodeMaterialGraph(g0);
    check(!blob1.empty(), "encode produces a non-empty blob");
    std::cout << "  blob: " << blob1.size() << " bytes, " << g0.nodes().size() << " nodes\n";

    MaterialGraph g1;
    std::string err;
    const bool ok = decodeMaterialGraph(std::span<const std::byte>(blob1), g1, &err);
    check(ok, "decode succeeds");
    if (!ok) { std::cout << "  decode error: " << err << "\n"; return 1; }

    // STRUCTURAL equality.
    check(g1.shading_model == g0.shading_model, "shading model preserved");
    check(g1.render_state.alpha_mode   == EAlphaMode::Mask
       && g1.render_state.double_sided == true
       && g1.render_state.alpha_cutoff == g0.render_state.alpha_cutoff,
          "render_state (alpha mode / cutoff / double-sided) preserved");
    check(g1.texture_slots.size() == 1 && g1.texture_slots[0].name == "BaseColorTex",
          "texture slot preserved (count + name)");
    check(g1.param_slots.size() == 1 && g1.param_slots[0].name == "Tint"
       && g1.param_slots[0].type == EMatValueType::Vec3
       && g1.param_slots[0].dflt[0] == 0.8f,
          "param slot preserved (name + type + default)");
    check(g1.nodes().size() == g0.nodes().size(), "node count preserved");
    check(countKind(g1, EMatNodeKind::SampleTexture) == 1
       && countKind(g1, EMatNodeKind::Swizzle) == 2
       && countKind(g1, EMatNodeKind::Math) == 1
       && countKind(g1, EMatNodeKind::OutputSurface) == 1,
          "node kinds preserved");

    // Connections survive (the OutputSurface's BaseColor pin links to something).
    {
        const Node* out = nullptr;
        for (const auto& [id, np] : g1.nodes()) if (np->kind() == EMatNodeKind::OutputSurface) out = np.get();
        const auto bc = static_cast<std::size_t>(EMaterialAttribute::BaseColor);
        check(out && bc < out->inputs().size() && out->inputs()[bc].source.valid(),
              "OutputSurface BaseColor connection preserved");
    }

    // v2: canvas positions survive the round-trip (ids remap, so match the
    // kind-unique nodes structurally).
    {
        const Node* tex0 = findKind(g0, EMatNodeKind::SampleTexture);
        const Node* tex1 = findKind(g1, EMatNodeKind::SampleTexture);
        const Node* out0 = findKind(g0, EMatNodeKind::OutputSurface);
        const Node* out1 = findKind(g1, EMatNodeKind::OutputSurface);
        check(tex1 && tex1->ui_placed
                  && tex1->ui_pos[0] == tex0->ui_pos[0]
                  && tex1->ui_pos[1] == tex0->ui_pos[1]
                  && out1 && out1->ui_placed
                  && out1->ui_pos[0] == out0->ui_pos[0]
                  && out1->ui_pos[1] == out0->ui_pos[1],
              "v2 canvas positions preserved through decode");
    }

    // BYTE-IDENTICAL re-encode (faithful structure + deterministic ordering).
    const std::vector<std::byte> blob2 = encodeMaterialGraph(g1);
    check(blob1 == blob2, "re-encode is byte-identical (faithful + deterministic)");

    // Bad magic is rejected.
    {
        std::vector<std::byte> bad = blob1;
        bad[0] = static_cast<std::byte>(0xFF);
        MaterialGraph gx;
        std::string e2;
        check(!decodeMaterialGraph(std::span<const std::byte>(bad), gx, &e2),
              "bad-magic input is rejected");
    }

    // LENIENT v1 DECODE: a hand-built v1 blob (no per-node ui fields) must
    // still decode, with every node unplaced (the grid-layout fallback case).
    {
using lux::core::serialization::ByteWriter;
        ByteWriter w;
        w.u32(lux::authoring::detail::kMatGraphMagic);
        w.u32(lux::authoring::detail::kMatGraphEndianTag);
        w.u32(1u);                              // schema_version = 1
        w.u8(static_cast<std::uint8_t>(ELightingTechnique::Unlit));
        w.u8(static_cast<std::uint8_t>(EAlphaMode::Opaque));
        w.f32(0.5f);
        w.u8(0u);                               // double_sided
        w.u32(0u);                              // texture slots
        w.u32(0u);                              // param slots
        w.u32(1u);                              // node count
        w.u64(1u);                              // node id
        w.u16(static_cast<std::uint16_t>(EMatNodeKind::Constant));
        w.str("c");
        // v1 record: NO ui fields — payload follows the name directly.
        w.f32(1.0f); w.f32(2.0f); w.f32(3.0f); w.f32(4.0f);
        w.u8(static_cast<std::uint8_t>(EMatValueType::Vec4));
        w.u32(0u);                              // input pin count
        w.u32(lux::authoring::detail::kMatGraphTrailer);
        const auto v1_blob = std::move(w).take();

        MaterialGraph gv1;
        std::string e3;
        const bool v1_ok = decodeMaterialGraph(
            std::span<const std::byte>(v1_blob), gv1, &e3);
        check(v1_ok, "v1 blob decodes leniently");
        if (!v1_ok) std::cout << "  v1 decode error: " << e3 << "\n";
        const Node* c1 = v1_ok ? findKind(gv1, EMatNodeKind::Constant) : nullptr;
        check(c1 && !c1->ui_placed, "v1 nodes decode UNPLACED (grid fallback)");
        check(c1 && static_cast<const ConstantNode*>(c1)->value[3] == 4.0f,
              "v1 payload decoded correctly after the version split");
    }

    // Product-composed lazy decode: Authoring accepts the retired material-v3
    // layout, flattens its runtime lanes, and promotes the inline graph to the
    // current tagged payload. Runtime's material codec intentionally remains
    // strict-v4; this compatibility belongs to Editor/Toolchain only.
    {
        lux::asset::AssetInfo asset_info{};
        asset_info.type = lux::asset::EAssetType::MATERIAL;

        std::vector<std::byte> info;
        appendPod<std::uint32_t>(info, 3u);
        appendPod<std::uint32_t>(
            info,
            static_cast<std::uint32_t>(blob1.size())
        );
        info.insert(info.end(), blob1.begin(), blob1.end());
        for (int field = 0; field < 5; ++field)
            appendPod<std::uint32_t>(info, 0u); // tex + 2x(spv/info)

        const auto image = makeMaterialImage(asset_info, std::move(info));
        const auto catalog = lux::authoring::authoringAssetCodecCatalog();
        auto decoded = catalog->decodeAsset(
            lux::cxx::SharedBytes<>::copyOf(image)
        );
        check(decoded.has_value(), "authoring catalog accepts material v3");

        const auto* material = decoded
            ? (*decoded)->as<lux::asset::MaterialAsset>()
            : nullptr;
        check(material && material->data(),
              "material v3 lazy decode injects cooked runtime data");
        check(material && material->data() &&
                  material->data()->parameter_count == 1u &&
                  material->data()->parameter_defaults[0][0] == 0.8f &&
                  material->data()->alpha_mode ==
                      static_cast<std::uint32_t>(EAlphaMode::Mask) &&
                  material->data()->double_sided,
              "material v3 graph defaults flatten into runtime lanes");
        MaterialGraph migrated;
        std::string migrated_error;
        check(material && lux::authoring::readMaterialGraph(
                  *material,
                  migrated,
                  &migrated_error),
              "material v3 inline graph is promoted to authoring payload");
        check(migrated.nodes().size() == g0.nodes().size(),
              "promoted material v3 graph remains editable");
    }

    // Current v4 lazy decode must also restore its auxiliary graph payload.
    // The per-type runtime decoder only produces MaterialData, so the
    // authoring catalog is the product seam that reattaches document bytes.
    {
        lux::asset::AssetInfo asset_info{};
        asset_info.type = lux::asset::EAssetType::MATERIAL;

        std::vector<std::byte> info;
        appendPod<std::uint32_t>(info, 4u); // material format
        appendPod<std::uint32_t>(info, 1u); // parameter count
        appendPod<float>(info, 0.25f);
        appendPod<float>(info, 0.5f);
        appendPod<float>(info, 0.75f);
        appendPod<float>(info, 1.0f);
        appendPod<std::uint32_t>(
            info,
            static_cast<std::uint32_t>(EAlphaMode::Opaque)
        );
        appendPod<std::uint8_t>(info, 0u); // double sided
        for (int field = 0; field < 5; ++field)
            appendPod<std::uint32_t>(info, 0u); // tex + 2x(spv/info)

        const auto image = makeMaterialImage(
            asset_info,
            std::move(info),
            { lux::asset::Payload{
                lux::authoring::kMaterialGraphPayloadTag,
                blob1
            } }
        );
        const auto catalog = lux::authoring::authoringAssetCodecCatalog();
        auto decoded = catalog->decodeAsset(
            lux::cxx::SharedBytes<>::copyOf(image)
        );
        check(decoded.has_value(), "authoring catalog accepts material v4");
        const auto* material = decoded
            ? (*decoded)->as<lux::asset::MaterialAsset>()
            : nullptr;
        check(material && material->data() &&
                  material->data()->parameter_count == 1u,
              "material v4 lazy decode injects cooked runtime data");
        MaterialGraph restored;
        std::string restored_error;
        check(material && lux::authoring::readMaterialGraph(
                  *material,
                  restored,
                  &restored_error),
              "material v4 lazy decode restores authoring payload tail");
    }

    std::cout << "=== material_graph_codec_test " << (fails == 0 ? "PASSED" : "FAILED")
              << " (fails=" << fails << ") ===\n";
    return fails == 0 ? 0 : 1;
}
