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
 * Each asset is wrapped under a hardcoded UUID matching the constants in
 * `EditorBuiltins.hpp`. At runtime `EditorBuiltins::registerInto` loads
 * the files back into the running `AssetManager`; in dev builds the
 * editor falls back to the in-memory geometry helpers under
 * `BuiltinGeometry.hpp` if the files are missing.
 *
 * The white-PBR material is NOT baked here. `MaterialSerDeser::export*`
 * is an unimplemented stub today; until the import pipeline lands in
 * M3b, the white-PBR builtin stays programmatic inside EditorBuiltins.
 *
 * The skybox texture is also not baked — it already lives on disk under
 * the render module's runtime-assets tree, and EditorBuiltins continues
 * to load it from there directly.
 */

#include <lux/engine/editor/content/BuiltinGeometry.hpp>
#include <lux/engine/editor/content/EditorBuiltins.hpp>      // for the kBuiltin*IdStr constants

#include <lux/engine/asset/AssetManager.hpp>
#include <lux/engine/asset/MeshAsset.hpp>
#include <lux/engine/asset/MeshSerDeser.hpp>
#include <lux/engine/description/Mesh.hpp>
#include <lux/engine/description/Vertex.hpp>
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
    using namespace lux::editor;

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
    // manager. We use a real shared_ptr here (unlike EditorBuiltins's
    // aliasing trick) because the baker owns the manager outright.
    auto mgr_ref = std::make_shared<lux::asset::AssetManager>();
    auto& mgr    = *mgr_ref;

    if (int rc = bakeCube  (mgr, mgr_ref, out_dir); rc != 0) return rc;
    if (int rc = bakePlane (mgr, mgr_ref, out_dir); rc != 0) return rc;
    if (int rc = bakeSphere(mgr, mgr_ref, out_dir); rc != 0) return rc;

    std::printf("[content_baker] done.\n");
    return 0;
}
