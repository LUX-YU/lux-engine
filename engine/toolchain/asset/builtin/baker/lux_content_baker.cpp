/**
 * @file lux_content_baker.cpp
 * @brief Build-time tool that pre-generates the editor's built-in
 *        `.luxasset` files.
 *
 * Usage: `lux_content_baker <output_dir>`
 *
 *   Writes:
 *     <output_dir>/Meshes/SM_Cube.luxasset
 *     <output_dir>/Meshes/SM_Plane.luxasset
 *     <output_dir>/Meshes/SM_Sphere.luxasset
 *
 * Each asset is wrapped under a frozen Runtime Asset UUID. At runtime the
 * editor adoption layer loads
 * the files back into the running `AssetManager`; in dev builds the
 * editor may fall back to the same Toolchain geometry recipes if files are
 * missing.
 *
 * Built-in MATERIALS (white PBR + the emissive palette) are baked too,
 * through the Toolchain material compiler, then exported with
 * MaterialSerDeser. Historical note: this baker originally skipped them
 * because MaterialSerDeser::export* was a stub; the exporter has since
 * become real, and a game host (lux_player) resolves these SAME UUIDs
 * through the VFS — without the files, every mesh that names the white
 * material waits for it forever (create-with-material discipline).
 *
 * The skybox texture is still not baked — it already lives on disk under
 * the render module's runtime-assets tree, and EditorBuiltins continues
 * to load it from there directly.
 */

#include <lux/engine/toolchain/asset/builtin/BuiltinGeometry.hpp>
#include <lux/engine/toolchain/asset/material/MaterialGraphCompiler.hpp>
#include <lux/cxx/core/Format.hpp>

#include <lux/engine/resource/asset/AssetManager.hpp>
#include <lux/engine/resource/asset/BuiltinAssetIds.hpp>
#include <lux/engine/resource/asset/MaterialAsset.hpp>
#include <lux/engine/resource/asset/MeshAsset.hpp>
#include <lux/engine/resource/asset/MeshSerDeser.hpp>
#include <lux/engine/resource/asset/MaterialSerDeser.hpp>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/description/Vertex.hpp>
#include <lux/engine/authoring/assets/material/MaterialGraph.hpp>
#include <lux/engine/math/AABB.hpp>

#include <uuid.h>

#include <Eigen/Core>

#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>

namespace
{
    using namespace lux::asset;
    using namespace lux::toolchain;

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

    bool ensureDir(const std::filesystem::path& p)
    {
        std::error_code ec;
        std::filesystem::create_directories(p, ec);
        if (ec)
        {
            std::fprintf(stderr, "[content_baker] mkdir failed '%s': %s\n",
                         p.string().c_str(), ec.message().c_str());
            return false;
        }
        return true;
    }

    int bakeCube(lux::asset::AssetManager&         mgr,
                 std::shared_ptr<lux::asset::AssetManager> mgr_ref,
                 const std::filesystem::path&      out_dir)
    {
        lux::asset::asset_id_t id;
        if (!parseUuid(kBuiltinCubeMeshIdStr, id))
        {
            std::fprintf(stderr, "[content_baker] bad cube UUID literal\n");
            return 1;
        }

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
            makeInfo(id, lux::asset::EAssetType::MESH));
        asset->setData(std::make_unique<lux::rdesc::Mesh>(std::move(mesh)));
        if (!mgr.registerAsset(std::move(asset)))
        {
            std::fprintf(stderr, "[content_baker] register cube failed\n");
            return 1;
        }

        const auto out = out_dir / "Meshes" / "SM_Cube.luxasset";
        if (!ensureDir(out.parent_path())) return 1;

        lux::asset::MeshSerDeser ser(mgr_ref);
        if (auto err = ser.exportAsLuxAsset(id, out);
            err != lux::asset::EAssetError::SUCCESS)
        {
            std::fprintf(stderr, "[content_baker] export cube failed (code=%d)\n",
                         static_cast<int>(err));
            return 1;
        }
        std::printf("  Wrote %s\n", out.string().c_str());
        return 0;
    }

    int bakePlane(lux::asset::AssetManager&         mgr,
                  std::shared_ptr<lux::asset::AssetManager> mgr_ref,
                  const std::filesystem::path&      out_dir)
    {
        lux::asset::asset_id_t id;
        if (!parseUuid(kBuiltinPlaneMeshIdStr, id))
        {
            std::fprintf(stderr, "[content_baker] bad plane UUID literal\n");
            return 1;
        }

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
            makeInfo(id, lux::asset::EAssetType::MESH));
        asset->setData(std::make_unique<lux::rdesc::Mesh>(std::move(mesh)));
        if (!mgr.registerAsset(std::move(asset)))
        {
            std::fprintf(stderr, "[content_baker] register plane failed\n");
            return 1;
        }

        const auto out = out_dir / "Meshes" / "SM_Plane.luxasset";
        if (!ensureDir(out.parent_path())) return 1;

        lux::asset::MeshSerDeser ser(mgr_ref);
        if (auto err = ser.exportAsLuxAsset(id, out);
            err != lux::asset::EAssetError::SUCCESS)
        {
            std::fprintf(stderr, "[content_baker] export plane failed (code=%d)\n",
                         static_cast<int>(err));
            return 1;
        }
        std::printf("  Wrote %s\n", out.string().c_str());
        return 0;
    }

    int bakeSphere(lux::asset::AssetManager&         mgr,
                   std::shared_ptr<lux::asset::AssetManager> mgr_ref,
                   const std::filesystem::path&      out_dir)
    {
        lux::asset::asset_id_t id;
        if (!parseUuid(kBuiltinSphereMeshIdStr, id))
        {
            std::fprintf(stderr, "[content_baker] bad sphere UUID literal\n");
            return 1;
        }

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
            makeInfo(id, lux::asset::EAssetType::MESH));
        asset->setData(std::make_unique<lux::rdesc::Mesh>(std::move(mesh)));
        if (!mgr.registerAsset(std::move(asset)))
        {
            std::fprintf(stderr, "[content_baker] register sphere failed\n");
            return 1;
        }

        const auto out = out_dir / "Meshes" / "SM_Sphere.luxasset";
        if (!ensureDir(out.parent_path())) return 1;

        lux::asset::MeshSerDeser ser(mgr_ref);
        if (auto err = ser.exportAsLuxAsset(id, out);
            err != lux::asset::EAssetError::SUCCESS)
        {
            std::fprintf(stderr, "[content_baker] export sphere failed (code=%d)\n",
                         static_cast<int>(err));
            return 1;
        }
        std::printf("  Wrote %s\n", out.string().c_str());
        return 0;
    }

} // namespace

namespace
{
    // Built-in materials are authored and compiled entirely in Toolchain.
    // EditorBuiltins is only a runtime adoption/fallback layer and is not a
    // dependency of this executable.
    int bakeBuiltinMaterials(const std::filesystem::path& out_dir)
    {
        auto mgr_ref = std::make_shared<lux::asset::AssetManager>(
            lux::asset::runtimeAssetCodecCatalog()
        );

        const auto registerMaterial = [&mgr_ref](
            const char* id_str,
            lux::asset::MaterialData data) -> bool
        {
            lux::asset::asset_id_t id;
            if (!parseUuid(id_str, id))
                return false;
            auto asset = std::make_unique<lux::asset::MaterialAsset>(
                makeInfo(id, lux::asset::EAssetType::MATERIAL));
            asset->setData(std::make_unique<lux::asset::MaterialData>(
                std::move(data)));
            return mgr_ref->registerAsset(std::move(asset));
        };

        auto white = compileGraphToPayload(
            makeNeutralPbrGraph(1.f, 1.f, 1.f, 0.f, 0.6f), {});
        if (!white || !registerMaterial(
                kBuiltinWhitePbrMaterialIdStr,
                std::move(*white)))
        {
            std::fprintf(stderr, "[content_baker] white material compile failed\n");
            return 1;
        }

        auto missing = compileGraphToPayload(
            makeNeutralPbrGraph(1.f, 0.f, 1.f, 0.f, 1.f), {});
        if (!missing || !registerMaterial(
                kBuiltinMissingMaterialIdStr,
                std::move(*missing)))
        {
            std::fprintf(stderr, "[content_baker] missing material compile failed\n");
            return 1;
        }

        static_assert(kBuiltinEmissiveCount > 0);
        const float* first_color = kBuiltinEmissiveColors[0];
        auto emissive_template = compileGraphToPayload(
            makeEmissivePbrGraph(
                first_color[0], first_color[1], first_color[2]),
            {});
        if (!emissive_template)
        {
            std::fprintf(stderr, "[content_baker] emissive compile failed\n");
            return 1;
        }
        for (int i = 0; i < kBuiltinEmissiveCount; ++i)
        {
            const float* color = kBuiltinEmissiveColors[i];
            lux::asset::MaterialData data;
            const auto graph = makeEmissivePbrGraph(
                color[0], color[1], color[2]);
            lux::toolchain::flattenMaterialRuntimeValues(graph, data);
            data.gbuffer_spirv = emissive_template->gbuffer_spirv;
            data.gbuffer_info = emissive_template->gbuffer_info;
            data.forward_spirv = emissive_template->forward_spirv;
            data.forward_info = emissive_template->forward_info;
            data.texture_slot_ids = emissive_template->texture_slot_ids;
            if (!registerMaterial(
                    kBuiltinEmissiveIdStrs[i],
                    std::move(data)))
            {
                std::fprintf(
                    stderr,
                    "[content_baker] emissive material %d register failed\n",
                    i);
                return 1;
            }
        }

        const auto mat_dir = out_dir / "Materials";
        if (!ensureDir(mat_dir)) return 1;

        lux::asset::MaterialSerDeser ser(mgr_ref);
        const auto exportOne = [&](const char* id_str,
                                   const std::filesystem::path& file) -> int
        {
            lux::asset::asset_id_t id;
            if (!parseUuid(id_str, id))
            {
                std::fprintf(stderr, "[content_baker] bad UUID literal '%s'\n", id_str);
                return 1;
            }
            if (mgr_ref->queryInfo(id) == nullptr)
            {
                std::fprintf(stderr,
                    "[content_baker] builtin %s not registered — bake failed upstream\n",
                    id_str);
                return 1;
            }
            if (auto err = ser.exportAsLuxAsset(id, file);
                err != lux::asset::EAssetError::SUCCESS)
            {
                std::fprintf(stderr, "[content_baker] export '%s' failed (code=%d)\n",
                             file.string().c_str(), static_cast<int>(err));
                return 1;
            }
            std::printf("  Wrote %s\n", file.string().c_str());
            return 0;
        };

        if (int rc = exportOne(kBuiltinWhitePbrMaterialIdStr,
                               mat_dir / "M_WhitePbr.luxasset"); rc != 0)
            return rc;
        // M_Missing(资产悬空的醒目兜底,裁决七):player 里资产同样会悬空,
        // 这一份**必须**落盘 —— 纯进程注册(PreviewGrey 那样)在 player 不存在。
        if (int rc = exportOne(kBuiltinMissingMaterialIdStr,
                               mat_dir / "M_Missing.luxasset"); rc != 0)
            return rc;
        for (int i = 0; i < kBuiltinEmissiveCount; ++i)
        {
            const auto name = lux::format("M_Emissive_{}.luxasset", i);
            if (int rc = exportOne(kBuiltinEmissiveIdStrs[i], mat_dir / name); rc != 0)
                return rc;
        }
        return 0;
    }
} // namespace

int main(int argc, char* argv[])
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: %s <output_dir>\n", argv[0]);
        return 2;
    }

    const std::filesystem::path out_dir = argv[1];
    std::printf("[content_baker] target dir: %s\n", out_dir.string().c_str());

    // The SerDeser API queries the AssetManager for the asset to export, so
    // both the asset registration and the exporters need to share one
    // manager. The baker owns the manager outright.
    auto mgr_ref = std::make_shared<lux::asset::AssetManager>(
        lux::asset::runtimeAssetCodecCatalog()
    );
    auto& mgr    = *mgr_ref;

    if (int rc = bakeCube  (mgr, mgr_ref, out_dir); rc != 0) return rc;
    if (int rc = bakePlane (mgr, mgr_ref, out_dir); rc != 0) return rc;
    if (int rc = bakeSphere(mgr, mgr_ref, out_dir); rc != 0) return rc;
    if (int rc = bakeBuiltinMaterials(out_dir); rc != 0) return rc;

    std::printf("[content_baker] done.\n");
    return 0;
}
