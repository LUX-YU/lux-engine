// ============================================================================
//  graph_shader_asset_test.cpp  —  material-graph shader persistence
//
//  Proves a material-graph compiled shader can be BAKED to a .luxasset and
//  LOADED back, faithfully round-tripping both the SPIR-V and the reflection
//  (ShaderInfo):
//
//    graph -> compileToSpirv(GBuffer, +ShaderInfo)
//          -> ShaderAsset(spirv, info) -> ShaderSerDeser::exportAsLuxAsset(path)
//          -> (fresh AssetManager) ShaderSerDeser::fromLuxAsset(path)
//          -> assert SPIR-V is byte-identical + ShaderInfo bindings survive.
//
//  Device-free (file I/O + serialization only). The loaded SPIR-V is exactly
//  what the live tests feed to compileShader, so a baked shader is a drop-in for
//  the session-time compile (cook-time packing). Self-checking: 0 = PASS.
// ============================================================================

#include <lux/engine/authoring/assets/material/MaterialGraph.hpp>
#include <lux/engine/authoring/assets/material/Nodes.hpp>
#include <lux/engine/description/MaterialGraphContract.hpp>
#include <lux/engine/description/ShaderInfo.hpp>
#include <lux/engine/description/Shader.hpp>

#include "graph_test_helpers.hpp"

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/ShaderAsset.hpp>
#include <lux/engine/resource/asset/ShaderSerDeser.hpp>
#include <lux/engine/resource/asset/Asset.hpp>

#include <uuid.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

using namespace lux::rdesc;
namespace rdesc = lux::rdesc;
namespace fs = std::filesystem;

static int g_fails = 0;
static void check(bool cond, const char* name)
{
    std::printf(cond ? "  [PASS] %s\n" : "  [FAIL] %s\n", name);
    if (!cond) ++g_fails;
}

static std::unique_ptr<lux::asset::AssetInfo>
makeInfo(lux::asset::asset_id_t id, lux::asset::EAssetType type)
{
    auto info  = std::make_unique<lux::asset::AssetInfo>();
    info->id   = id;
    info->type = type;
    info->date = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    return info;
}

int main()
{
    std::setbuf(stdout, nullptr);
    std::printf("=== graph_shader_asset_test (shader persistence) ===\n");

    // ---- 1. Build a param-using graph -> gbuffer SPIR-V + ShaderInfo ----
    std::vector<uint32_t> spirv;
    rdesc::ShaderInfo     info;
    {
        MaterialGraph g;  // PBR
        g.param_slots.push_back(ParamSlotDecl{ "tint", rdesc::EMatValueType::Vec3, { 0.8f, 0.2f, 0.2f, 0 } });
        node_id tint = g.addNode(std::make_unique<ParamNode>(rdesc::EMatValueType::Vec3));
        static_cast<ParamNode*>(g.node(tint))->param_slot = 0;
        node_id o = g.addNode(std::make_unique<OutputSurfaceNode>());
        g.connect(tint, 0, o, static_cast<uint32_t>(rdesc::EMaterialAttribute::BaseColor));

        auto cs = lux::mgtest::compileGraph(g, rdesc::EMaterialPass::GBuffer);
        check(static_cast<bool>(cs), "compileToSpirv(+ShaderInfo)");
        if (cs) { spirv = std::move(cs->spirv); info = std::move(cs->info); }
        else    std::printf("  (compile err) %s\n", cs.error().c_str());
    }
    check(!spirv.empty() && spirv[0] == 0x07230203u, "source SPIR-V magic ok");
    check(info.sets.size() == 1 && info.sets[0].set == 4, "source ShaderInfo has the set-4 graph SSBO");
    const std::size_t spirv_bytes = spirv.size() * sizeof(uint32_t);

    // ---- 2. Bake to a .luxasset ----
    const auto id = uuids::uuid::from_string("a17e7a5e-0000-4000-8000-00000000ab12").value();
    const fs::path out = fs::temp_directory_path() / "lux_graph_shader_asset_test.luxasset";

    {
        std::vector<std::byte> bytes(spirv_bytes);
        std::memcpy(bytes.data(), spirv.data(), spirv_bytes);

        auto mgr = std::make_shared<lux::asset::AssetManager>(
            lux::asset::runtimeAssetCodecCatalog());
        auto asset = std::make_unique<lux::asset::ShaderAsset>(
            makeInfo(id, lux::asset::EAssetType::SHADER), info);
        asset->assignData(std::move(bytes));
        check(mgr->registerAsset(std::move(asset)), "register ShaderAsset");

        lux::asset::ShaderSerDeser ser(mgr);
        const auto err = ser.exportAsLuxAsset(id, out);
        check(err == lux::asset::EAssetError::SUCCESS, "exportAsLuxAsset -> .luxasset");
    }
    check(fs::exists(out) && fs::file_size(out) > spirv_bytes, "baked .luxasset on disk (> spirv size)");
    std::printf("  baked: %s (%llu bytes)\n", out.string().c_str(),
                (unsigned long long)fs::file_size(out));

    // ---- 3. Load it back with a FRESH AssetManager ----
    {
        auto mgr = std::make_shared<lux::asset::AssetManager>(
            lux::asset::runtimeAssetCodecCatalog());
        lux::asset::ShaderSerDeser ser(mgr);
        auto res = ser.fromLuxAsset(out);
        check(static_cast<bool>(res), "fromLuxAsset -> asset");
        if (res)
        {
            auto* sa = res.value().first->as<lux::asset::ShaderAsset>();
            check(sa != nullptr, "loaded asset is a ShaderAsset");
            if (sa)
            {
                const auto* sh = sa->data();
                const auto& li = sa->shaderInfo();

                // SPIR-V byte-identical round-trip.
                check(sh && sh->size() == spirv_bytes, "loaded SPIR-V size matches");
                bool identical = sh && sh->size() == spirv_bytes &&
                                 std::memcmp(sh->data(), spirv.data(), spirv_bytes) == 0;
                check(identical, "loaded SPIR-V is byte-identical to the original");

                // Reflection (ShaderInfo) survives the round-trip.
                check(li.entry_points.size() == info.entry_points.size(),
                      "loaded ShaderInfo entry-point count matches");
                check(li.sets.size() == info.sets.size() &&
                      !li.sets.empty() && li.sets[0].set == 4,
                      "loaded ShaderInfo keeps the set-4 graph SSBO binding");
            }
        }
    }

    std::error_code ec; fs::remove(out, ec);

    std::printf("=== graph_shader_asset_test %s (fails=%d) ===\n",
                g_fails == 0 ? "PASSED" : "FAILED", g_fails);
    return g_fails == 0 ? 0 : 1;
}
