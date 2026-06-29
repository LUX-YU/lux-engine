#include <lux/engine/editor/content/EditorBuiltins.hpp>
#include <lux/engine/editor/content/BuiltinGeometry.hpp>
#include <lux/engine/editor/content/EngineContentPath.hpp>
#include <lux/engine/editor/content/RuntimeAssetPath.hpp>

#include <lux/engine/asset/AssetManager.hpp>
#include <lux/engine/asset/MaterialAsset.hpp>
#include <lux/engine/asset/MeshAsset.hpp>
#include <lux/engine/asset/MeshSerDeser.hpp>
#include <lux/engine/asset/TextureSerDeser.hpp>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/description/Vertex.hpp>
#include <lux/engine/math/AABB.hpp>

#include "import/MaterialGraphBake.hpp"   // makeNeutralPbrGraph + compileGraphToPayload (graph = sole material path)
#include <lux/engine/description/material_graph/MaterialGraph.hpp>  // complete type (makeNeutralPbrGraph returns by value)

#include <Eigen/Core>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace lux::editor
{
    namespace
    {
        // ── Asset-construction helpers ─────────────────────────────────

        std::unique_ptr<lux::asset::AssetInfo>
        makeInfo(lux::asset::asset_id_t id, lux::asset::EAssetType type)
        {
            auto info  = std::make_unique<lux::asset::AssetInfo>();
            info->id   = id;
            info->type = type;
            info->date = std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count();
            return info;
        }

        bool parseUuid(const char* s, lux::asset::asset_id_t& out) noexcept
        {
            auto parsed = uuids::uuid::from_string(s);
            if (!parsed) return false;
            out = *parsed;
            return true;
        }

        /// Build an aliasing shared_ptr that lets the SerDeser hold the
        /// editor's AssetManager without ever deleting it (the real owner
        /// is `LuxEditor`).
        std::shared_ptr<lux::asset::AssetManager>
        aliasManagerPtr(lux::asset::AssetManager& mgr)
        {
            return std::shared_ptr<lux::asset::AssetManager>(
                std::shared_ptr<void>{}, &mgr);
        }

        // ── Fallback path: programmatic mesh construction.
        //    Used only when the on-disk engine content is missing. Keeps
        //    the editor bootable in tree-corrupted environments. ─────────

        bool registerCubeFallback(lux::asset::AssetManager& mgr,
                                  lux::asset::asset_id_t&   out_id)
        {
            if (!parseUuid(kBuiltinCubeMeshIdStr, out_id))
                return false;

            std::vector<lux::rdesc::Vertex> verts;
            std::vector<std::uint32_t>      indices;
            buildCubeGeometry(verts, indices, 0.5f);

            lux::rdesc::Mesh mesh{
                std::move(verts),
                std::move(indices),
                lux::math::AABB(Eigen::Vector3f(-0.5f, -0.5f, -0.5f),
                                Eigen::Vector3f( 0.5f,  0.5f,  0.5f))
            };
            auto asset = std::make_unique<lux::asset::MeshAsset>(
                makeInfo(out_id, lux::asset::EAssetType::MESH));
            asset->setData(std::make_unique<lux::rdesc::Mesh>(std::move(mesh)));
            return mgr.registerAsset(std::move(asset));
        }

        bool registerPlaneFallback(lux::asset::AssetManager& mgr,
                                   lux::asset::asset_id_t&   out_id)
        {
            if (!parseUuid(kBuiltinPlaneMeshIdStr, out_id))
                return false;

            std::vector<lux::rdesc::Vertex> verts;
            std::vector<std::uint32_t>      indices;
            buildFloorPlaneGeometry(verts, indices, 5.0f);

            lux::rdesc::Mesh mesh{
                std::move(verts),
                std::move(indices),
                lux::math::AABB(Eigen::Vector3f(-5.0f, -0.01f, -5.0f),
                                Eigen::Vector3f( 5.0f,  0.01f,  5.0f))
            };
            auto asset = std::make_unique<lux::asset::MeshAsset>(
                makeInfo(out_id, lux::asset::EAssetType::MESH));
            asset->setData(std::make_unique<lux::rdesc::Mesh>(std::move(mesh)));
            return mgr.registerAsset(std::move(asset));
        }

        bool registerSphereFallback(lux::asset::AssetManager& mgr,
                                    lux::asset::asset_id_t&   out_id)
        {
            if (!parseUuid(kBuiltinSphereMeshIdStr, out_id))
                return false;

            std::vector<lux::rdesc::Vertex> verts;
            std::vector<std::uint32_t>      indices;
            buildSphereGeometry(verts, indices, 0.5f);

            lux::rdesc::Mesh mesh{
                std::move(verts),
                std::move(indices),
                lux::math::AABB(Eigen::Vector3f(-0.5f, -0.5f, -0.5f),
                                Eigen::Vector3f( 0.5f,  0.5f,  0.5f))
            };
            auto asset = std::make_unique<lux::asset::MeshAsset>(
                makeInfo(out_id, lux::asset::EAssetType::MESH));
            asset->setData(std::make_unique<lux::rdesc::Mesh>(std::move(mesh)));
            return mgr.registerAsset(std::move(asset));
        }

        // White built-in material — a GRAPH material now (W5 retired rdesc::Material;
        // the editor's sole material path is the node graph). Authored in memory as a
        // neutral white PBR graph + baked to a MaterialData payload, registered
        // under the SAME stable UUID so demo/saved scenes that reference it keep
        // resolving (the bridge dispatches MATERIAL -> ensureGraphMaterial).
        bool registerWhiteGraphMaterial(lux::asset::AssetManager& mgr,
                                        lux::asset::asset_id_t&   out_id)
        {
            if (!parseUuid(kBuiltinWhitePbrMaterialIdStr, out_id))
                return false;

            auto payload_exp = compileGraphToPayload(
                makeNeutralPbrGraph(1.f, 1.f, 1.f, /*metallic=*/0.f, /*roughness=*/0.6f),
                /*slot_texture_ids=*/{});
            if (!payload_exp)
            {
                std::fprintf(stderr,
                    "[EditorBuiltins] white graph material bake failed: %s\n",
                    payload_exp.error().c_str());
                return false;
            }
            auto payload = std::make_unique<lux::asset::MaterialData>(
                std::move(*payload_exp));
            auto asset = std::make_unique<lux::asset::MaterialAsset>(
                makeInfo(out_id, lux::asset::EAssetType::MATERIAL));
            asset->setData(std::move(payload));
            return mgr.registerAsset(std::move(asset));
        }

        // Emissive "bulb" palette — bright unlit-looking glow materials, one per
        // EditorBuiltins palette slot, registered under the shared stable UUIDs so
        // a generated demo scene's light bulbs resolve on load.
        bool registerEmissivePalette(lux::asset::AssetManager& mgr)
        {
            for (int i = 0; i < kBuiltinEmissiveCount; ++i)
            {
                lux::asset::asset_id_t id{};
                if (!parseUuid(kBuiltinEmissiveIdStrs[i], id))
                    return false;

                const float* c = kBuiltinEmissiveColors[i];
                auto payload_exp = compileGraphToPayload(
                    makeEmissivePbrGraph(c[0], c[1], c[2]), /*slot_texture_ids=*/{});
                if (!payload_exp)
                {
                    std::fprintf(stderr,
                        "[EditorBuiltins] emissive material %d bake failed: %s\n",
                        i, payload_exp.error().c_str());
                    return false;
                }
                auto asset = std::make_unique<lux::asset::MaterialAsset>(
                    makeInfo(id, lux::asset::EAssetType::MATERIAL));
                asset->setData(std::make_unique<lux::asset::MaterialData>(
                    std::move(*payload_exp)));
                if (!mgr.registerAsset(std::move(asset)))
                {
                    std::fprintf(stderr,
                        "[EditorBuiltins] emissive material %d register failed\n", i);
                    return false;
                }
            }
            return true;
        }

        // ── Primary path: load a .luxasset from disk via SerDeser. ──────

        template <class SerDeser>
        bool tryLoadFromDisk(SerDeser&                     ser,
                             const std::filesystem::path&  path,
                             lux::asset::asset_id_t&       out_id,
                             const char*                   label)
        {
            namespace fs = std::filesystem;
            if (!fs::exists(path))
            {
                std::fprintf(stderr,
                    "[EditorBuiltins] %s missing at '%s' - using fallback.\n",
                    label, path.string().c_str());
                return false;
            }
            auto res = ser.fromLuxAsset(path);
            if (!res)
            {
                std::fprintf(stderr,
                    "[EditorBuiltins] %s load failed (code=%d) - using fallback.\n",
                    label, static_cast<int>(res.error()));
                return false;
            }
            out_id = res.value().second;
            return true;
        }
    } // namespace

    bool EditorBuiltins::registerInto(lux::asset::AssetManager& mgr)
    {
        if (ready_) return true;

        namespace fs = std::filesystem;
        const auto content_root = engine_content_path;

        auto mgr_ref = aliasManagerPtr(mgr);
        lux::asset::MeshSerDeser mesh_ser(mgr_ref);

        // ── Cube ───────────────────────────────────────────────────────
        if (!tryLoadFromDisk(mesh_ser,
                             content_root / "Meshes" / "SM_Cube.luxasset",
                             cube_mesh_id_,
                             "cube"))
        {
            if (!registerCubeFallback(mgr, cube_mesh_id_))
            {
                std::fprintf(stderr, "[EditorBuiltins] cube fallback failed\n");
                return false;
            }
        }

        // ── Plane ──────────────────────────────────────────────────────
        if (!tryLoadFromDisk(mesh_ser,
                             content_root / "Meshes" / "SM_Plane.luxasset",
                             plane_mesh_id_,
                             "plane"))
        {
            if (!registerPlaneFallback(mgr, plane_mesh_id_))
            {
                std::fprintf(stderr, "[EditorBuiltins] plane fallback failed\n");
                return false;
            }
        }

        // ── Sphere (material-preview ball) ─────────────────────────────
        if (!tryLoadFromDisk(mesh_ser,
                             content_root / "Meshes" / "SM_Sphere.luxasset",
                             sphere_mesh_id_,
                             "sphere"))
        {
            if (!registerSphereFallback(mgr, sphere_mesh_id_))
            {
                std::fprintf(stderr, "[EditorBuiltins] sphere fallback failed\n");
                return false;
            }
        }

        // ── White (graph) material ─────────────────────────────────────
        //
        // Baked in memory from a neutral white PBR graph (W5: rdesc::Material is
        // retired, so the built-in white material is a GRAPH material like every
        // other). Disk-load is intentionally absent — it is cheap to author + bake
        // at startup, and the stable UUID keeps demo/saved scenes resolving.
        if (!registerWhiteGraphMaterial(mgr, white_pbr_id_))
        {
            std::fprintf(stderr, "[EditorBuiltins] white graph material registration failed\n");
            return false;
        }

        // ── Emissive bulb palette (visible light sources for demo lights) ──
        if (!registerEmissivePalette(mgr))
        {
            std::fprintf(stderr, "[EditorBuiltins] emissive palette registration failed\n");
            return false;
        }

        // ── Skybox texture ─────────────────────────────────────────────
        //
        // Lives in the render module's runtime-asset tree (unchanged from
        // M1.5). The UUID inside the file is whatever was baked there; we
        // record what the SerDeser reports. Missing file is non-fatal —
        // the Skybox bridge sees a nil UUID and pushes nothing.
        {
            const fs::path sky_path =
                fs::path(editor_runtime_asset_path) / "textures" / "blue_nebulae_1.luxasset";

            if (fs::exists(sky_path))
            {
                lux::asset::TextureSerDeser tex_ser(mgr_ref);
                auto tex_res = tex_ser.fromLuxAsset(sky_path);
                if (!tex_res)
                {
                    std::fprintf(stderr,
                        "[EditorBuiltins] skybox texture load failed (code=%d)\n",
                        static_cast<int>(tex_res.error()));
                }
                else
                {
                    skybox_tex_id_ = tex_res.value().second;
                }
            }
            else
            {
                std::fprintf(stderr,
                    "[EditorBuiltins] skybox texture missing at %s - sky will be empty\n",
                    sky_path.string().c_str());
            }
        }

        ready_ = true;
        return true;
    }

} // namespace lux::editor
