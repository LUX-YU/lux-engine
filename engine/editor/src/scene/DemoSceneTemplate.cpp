#include <lux/engine/editor/scene/DemoSceneTemplate.hpp>

#include <lux/engine/editor/content/EditorBuiltins.hpp>
#include <lux/engine/editor/scene/Scene.hpp>

#include <lux/pack/d3/world/components/CameraComponent.hpp>
#include <lux/pack/d3/world/components/DirectionalLightComponent.hpp>
#include <lux/pack/d3/world/components/PointLightComponent.hpp>
#include <lux/pack/d3/world/components/SpotLightComponent.hpp>
#include <lux/pack/d3/world/components/SceneSettingsComponent.hpp>
#include <lux/pack/d3/world/components/GridComponent.hpp>
#include <lux/pack/d3/world/components/MeshComponent.hpp>
#include <lux/engine/ecs/components/NameComponent.hpp>
#include <lux/pack/d3/world/components/SkyboxComponent.hpp>
#include <lux/pack/d3/world/components/TransformComponent.hpp>
#include <lux/pack/d3/world/components/WorldTransformComponent.hpp>
#include <lux/engine/meta/LuxObject.hpp>     // EntityRegistry
#include <lux/engine/render/core/LightDescriptor.hpp>  // LIGHT_FLAG_*

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <cmath>
#include <cstdio>
#include <string>
#include <system_error>

namespace lux::editor
{
    namespace
    {
        constexpr float kPi = 3.14159265358979323846f;

        bool parseUuid(const char* s, lux::asset::asset_id_t& out) noexcept
        {
            auto parsed = uuids::uuid::from_string(s);
            if (!parsed) return false;
            out = *parsed;
            return true;
        }
    } // namespace

    bool writeDemoScene(const std::filesystem::path& scene_file_path)
    {
        // Ensure parent dir exists.
        if (auto parent = scene_file_path.parent_path(); !parent.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
            if (ec)
            {
                std::fprintf(stderr,
                    "[writeDemoScene] failed to create parent dir '%s': %s\n",
                    parent.string().c_str(), ec.message().c_str());
                return false;
            }
        }

        // Parse the EditorBuiltins UUID literals — same IDs the editor's
        // AssetManager registers, so loading this scene later resolves.
        lux::asset::asset_id_t cube_id{}, plane_id{}, white_pbr_id{};
        if (!parseUuid(kBuiltinCubeMeshIdStr, cube_id)        ||
            !parseUuid(kBuiltinPlaneMeshIdStr, plane_id)      ||
            !parseUuid(kBuiltinWhitePbrMaterialIdStr, white_pbr_id))
        {
            std::fprintf(stderr,
                "[writeDemoScene] bad EditorBuiltins UUID literal\n");
            return false;
        }

        // ── Build the World in memory ──
        lux::meta::EntityRegistry reg;

        // Editor Camera — looks at the origin from up-and-back.
        const auto camera_e = reg.create();
        reg.emplace<lux::ecs::NameComponent>(camera_e,
            lux::ecs::NameComponent{"Editor Camera"});
        {
            auto& tc = reg.emplace<lux::pack::TransformComponent>(camera_e);
            tc.position = Eigen::Vector3f(6.f, 6.f, 8.f);
            // Yaw ~ -35° round Y so the camera faces -Z toward the cube;
            // pitch ~ -30° to look down at the floor.
            const Eigen::Quaternionf q_yaw(
                Eigen::AngleAxisf(-35.f * kPi / 180.f, Eigen::Vector3f::UnitY()));
            const Eigen::Quaternionf q_pitch(
                Eigen::AngleAxisf(-30.f * kPi / 180.f, Eigen::Vector3f::UnitX()));
            tc.rotation = (q_yaw * q_pitch).normalized();
        }
        reg.emplace<lux::pack::WorldTransformComponent>(camera_e);
        {
            auto& cc = reg.emplace<lux::pack::CameraComponent>(camera_e);
            cc.fov_rad = 60.f * (kPi / 180.f);
            cc.near_z  = 0.1f;
            cc.far_z   = 500.f;
            cc.aspect  = 16.f / 9.f;
            cc.y_flip  = true;
        }

        // Skybox singleton — uses the editor-builtin equirect texture
        // (registered by EditorBuiltins::registerInto from disk). If the
        // file is missing, the Skybox bridge pushes nothing and the pass
        // renders the cleared HDR target; the scene still loads cleanly.
        {
            const auto e = reg.create();
            reg.emplace<lux::ecs::NameComponent>(e,
                lux::ecs::NameComponent{"Skybox"});
            // Skybox UUID is the texture id that EditorBuiltins manages
            // privately; we leave the field nil here. EditorBuiltins'
            // SkyboxComponent default below works because the loader
            // installs the same skybox id at editor startup — we just
            // don't have the const exposed at link time. Leaving nil
            // means the Skybox bridge is a no-op until the user assigns
            // a texture via the Inspector; acceptable for the demo.
            reg.emplace<lux::pack::SkyboxComponent>(e,
                lux::pack::SkyboxComponent{});
        }

        // Grid singleton.
        {
            const auto e = reg.create();
            reg.emplace<lux::ecs::NameComponent>(e,
                lux::ecs::NameComponent{"Grid"});
            reg.emplace<lux::pack::GridComponent>(e,
                lux::pack::GridComponent{});
        }

        // Directional sun light — shadow-casting to exercise the full
        // MeshShadow + ShadowMap pipeline.
        {
            const auto e = reg.create();
            reg.emplace<lux::ecs::NameComponent>(e,
                lux::ecs::NameComponent{"Sun"});
            lux::pack::DirectionalLightComponent dl{};
            dl.direction      = Eigen::Vector3f(-0.5f, -1.f, -0.3f).normalized();
            dl.color          = Eigen::Vector3f(1.f, 0.95f, 0.8f);
            dl.intensity      = 1.5f;
            dl.cast_shadow    = true;
            // Editor scene casts only the sun (no point/spot shadows), so all 4
            // atlas pages are free for the cascades: 4096 gives each cascade a full
            // 4096² page (~2× texel density vs 2048) → shadow detail stays sharp
            // ~2× further from the camera before the per-cascade downsample shows.
            dl.shadow_map_size = 4096;
            dl.shadow_bias    = 0.002f;
            dl.cascade_count  = 4;
            // Absolute-metre cascade far distances (NOT [0,1] normalized ratios):
            // cascade 0 = near→10 m (sharpest), 1 = 10→30, 2 = 30→70, 3 = 70→150.
            dl.cascade_splits = {10.f, 30.f, 70.f, 150.f, 0.f, 0.f, 0.f, 0.f};
            reg.emplace<lux::pack::DirectionalLightComponent>(e, dl);
        }

        // Floor — 10×10 plane at y=0 with the editor-builtin white PBR.
        {
            const auto e = reg.create();
            reg.emplace<lux::ecs::NameComponent>(e,
                lux::ecs::NameComponent{"Floor"});
            reg.emplace<lux::pack::TransformComponent>(e);
            reg.emplace<lux::pack::WorldTransformComponent>(e);
            lux::pack::MeshComponent mc{};
            mc.mesh_asset_id     = plane_id;
            mc.material_asset_id = white_pbr_id;
            mc.cast_shadow       = true;
            mc.visible           = true;
            reg.emplace<lux::pack::MeshComponent>(e, mc);
        }

        // Cube — lifted to y=1.5 so its shadow falls on the floor.
        {
            const auto e = reg.create();
            reg.emplace<lux::ecs::NameComponent>(e,
                lux::ecs::NameComponent{"Cube"});
            auto& tc = reg.emplace<lux::pack::TransformComponent>(e);
            tc.position = Eigen::Vector3f(0.f, 1.5f, 0.f);
            reg.emplace<lux::pack::WorldTransformComponent>(e);
            lux::pack::MeshComponent mc{};
            mc.mesh_asset_id     = cube_id;
            mc.material_asset_id = white_pbr_id;
            mc.cast_shadow       = true;
            mc.visible           = true;
            reg.emplace<lux::pack::MeshComponent>(e, mc);
        }

        // ── Persist ──
        lux::editor::SceneSaveOptions opts;
        opts.active_camera = camera_e;
        auto saved = lux::editor::Scene::save(scene_file_path, reg, opts);
        if (!saved)
        {
            std::fprintf(stderr,
                "[writeDemoScene] Scene::save failed: %s\n",
                saved.error().c_str());
            return false;
        }
        return true;
    }

    // ========================================================================
    //  writeBigDemoScene — a city-scale streaming + multi-light showcase.
    // ========================================================================
    bool writeBigDemoScene(const std::filesystem::path& scene_file_path)
    {
        if (auto parent = scene_file_path.parent_path(); !parent.empty())
        {
            std::error_code ec;
            std::filesystem::create_directories(parent, ec);
        }

        // Builtin asset UUIDs (registered by EditorBuiltins at editor startup).
        lux::asset::asset_id_t cube_id{}, sphere_id{}, plane_id{}, white_id{};
        if (!parseUuid(kBuiltinCubeMeshIdStr, cube_id)   ||
            !parseUuid(kBuiltinSphereMeshIdStr, sphere_id) ||
            !parseUuid(kBuiltinPlaneMeshIdStr, plane_id)  ||
            !parseUuid(kBuiltinWhitePbrMaterialIdStr, white_id))
        {
            std::fprintf(stderr, "[writeBigDemoScene] bad builtin UUID literal\n");
            return false;
        }
        lux::asset::asset_id_t emissive_id[kBuiltinEmissiveCount]{};
        for (int i = 0; i < kBuiltinEmissiveCount; ++i)
            if (!parseUuid(kBuiltinEmissiveIdStrs[i], emissive_id[i]))
            {
                std::fprintf(stderr, "[writeBigDemoScene] bad emissive UUID literal\n");
                return false;
            }

        lux::meta::EntityRegistry reg;

        // ── Helpers ──
        const auto addMesh = [&](const std::string&             name,
                                 const lux::asset::asset_id_t&  mesh,
                                 const lux::asset::asset_id_t&  mat,
                                 const Eigen::Vector3f&         pos,
                                 const Eigen::Vector3f&         scale,
                                 bool                           cast_shadow)
        {
            const auto e = reg.create();
            reg.emplace<lux::ecs::NameComponent>(e, lux::ecs::NameComponent{name});
            auto& tc = reg.emplace<lux::pack::TransformComponent>(e);
            tc.position = pos;
            tc.scale    = scale;
            reg.emplace<lux::pack::WorldTransformComponent>(e);
            lux::pack::MeshComponent mc{};
            mc.mesh_asset_id     = mesh;
            mc.material_asset_id  = mat;
            mc.cast_shadow       = cast_shadow;
            mc.visible           = true;
            reg.emplace<lux::pack::MeshComponent>(e, mc);
        };

        // [0,1) deterministic hash from two integers (no RNG → reproducible scene).
        const auto hash01 = [](int x, int y) -> float
        {
            const float s = std::sin(static_cast<float>(x) * 127.1f
                                   + static_cast<float>(y) * 311.7f) * 43758.5453f;
            return s - std::floor(s);
        };

        // ── Camera — elevated vantage in the middle of the city. ──
        const auto camera_e = reg.create();
        reg.emplace<lux::ecs::NameComponent>(camera_e,
            lux::ecs::NameComponent{"Editor Camera"});
        {
            auto& tc = reg.emplace<lux::pack::TransformComponent>(camera_e);
            tc.position = Eigen::Vector3f(0.f, 45.f, 90.f);
            const Eigen::Quaternionf q_pitch(
                Eigen::AngleAxisf(-22.f * kPi / 180.f, Eigen::Vector3f::UnitX()));
            tc.rotation = q_pitch.normalized();
        }
        reg.emplace<lux::pack::WorldTransformComponent>(camera_e);
        {
            auto& cc = reg.emplace<lux::pack::CameraComponent>(camera_e);
            cc.fov_rad = 60.f * (kPi / 180.f);
            cc.near_z  = 0.1f;
            cc.far_z   = 3000.f;
            cc.aspect  = 16.f / 9.f;
            cc.y_flip  = true;
        }

        // ── Skybox + Grid singletons. ──
        {
            const auto e = reg.create();
            reg.emplace<lux::ecs::NameComponent>(e, lux::ecs::NameComponent{"Skybox"});
            reg.emplace<lux::pack::SkyboxComponent>(e, lux::pack::SkyboxComponent{});
        }
        {
            const auto e = reg.create();
            reg.emplace<lux::ecs::NameComponent>(e, lux::ecs::NameComponent{"Grid"});
            reg.emplace<lux::pack::GridComponent>(e, lux::pack::GridComponent{});
        }

        // Scene Settings singleton (the UE WorldSettings model): scene-global view
        // distance (render cull) + a TIGHT, fine streaming bubble that visibly
        // follows the camera. Edited live via the Scene Settings panel (Scene tab),
        // serialized with the scene. cull_distance >= unload_range so loaded content
        // is also drawable.
        {
            const auto e = reg.create();
            reg.emplace<lux::ecs::NameComponent>(e,
                lux::ecs::NameComponent{"Scene Settings"});
            lux::pack::SceneSettingsComponent ss{};
            ss.cull_distance    = 512.f;   // render draw distance
            ss.cell_size        = 48.f;
            ss.load_range       = 300.f;
            ss.unload_range     = 450.f;
            ss.prefetch_range   = 620.f;
            ss.evict_age_frames = 90;
            reg.emplace<lux::pack::SceneSettingsComponent>(e, ss);
        }

        // ── Directional sun (the only shadow-caster — point/spot shadows are
        //    cubemap/extra-map heavy and we have many lights). ──
        {
            const auto e = reg.create();
            reg.emplace<lux::ecs::NameComponent>(e, lux::ecs::NameComponent{"Sun"});
            lux::pack::DirectionalLightComponent dl{};
            dl.direction       = Eigen::Vector3f(-0.4f, -1.f, -0.35f).normalized();
            dl.color           = Eigen::Vector3f(0.9f, 0.92f, 1.0f);
            dl.intensity       = 1.1f;
            dl.cast_shadow     = true;
            // Only shadow-caster in the big demo → all 4 atlas pages free for the
            // cascades, so a full 4096² per cascade (~2× texel density vs 2048).
            dl.shadow_map_size = 4096;
            dl.shadow_bias     = 0.002f;
            dl.cascade_count   = 4;
            dl.cascade_splits  = {20.f, 60.f, 140.f, 300.f, 0.f, 0.f, 0.f, 0.f};
            reg.emplace<lux::pack::DirectionalLightComponent>(e, dl);
        }

        // ── Ground — a 26×26 grid of plane tiles (SM_Plane is 10×10, scaled
        //    ×20 → 200×200 each) covering a ~5200×5200 world. ──
        //    kGround is EVEN, so a tile boundary lands exactly on x=0 / z=0 (the
        //    world origin). Tiles sized at exactly kTile would meet edge-to-edge and
        //    leave a sub-pixel rasterization CRACK along that shared edge — visible as
        //    a dark line straight through the viewport centre (the editor orbit-camera
        //    centres the origin). Oversize each tile a hair so neighbours OVERLAP and
        //    no crack can form; the overlap is invisible (same white PBR, identical
        //    y=0 plane → identical GBuffer in the overlap, no z-fight).
        constexpr int   kGround     = 26;
        constexpr float kTile       = 200.f;
        constexpr float kTileScale  = 20.f;
        constexpr float kTileOverlap = 1.01f;   // +1% → ~2-unit overlap per seam, kills the crack
        const float     ground0     = -(kGround - 1) * 0.5f * kTile;
        for (int gz = 0; gz < kGround; ++gz)
            for (int gx = 0; gx < kGround; ++gx)
            {
                const Eigen::Vector3f p(ground0 + gx * kTile, 0.f, ground0 + gz * kTile);
                addMesh("Ground", plane_id, white_id, p,
                        Eigen::Vector3f(kTileScale * kTileOverlap, 1.f, kTileScale * kTileOverlap),
                        /*cast_shadow=*/false);
            }

        // ── Buildings + props — a 40×40 block grid spread wide so the streaming
        //    bubble has plenty to flow through; streets where i%6==0. Positions
        //    are JITTERED (±0.4 cell) so they do NOT line up in rows — otherwise
        //    the streaming boundary sweeping a regular grid pops whole rows at
        //    once (a far row is ~equidistant). Jitter → organic, sphere-like
        //    appear/disappear around the camera. ──
        constexpr int   kGrid    = 40;
        constexpr float kSpacing = 100.f;
        const float     grid0    = -(kGrid - 1) * 0.5f * kSpacing;
        for (int j = 0; j < kGrid; ++j)
            for (int i = 0; i < kGrid; ++i)
            {
                if (i % 6 == 0 || j % 6 == 0) continue;   // leave streets

                const float jx = (hash01(i + 17,  j + 101) - 0.5f) * (kSpacing * 0.8f);
                const float jz = (hash01(i + 211, j + 37)  - 0.5f) * (kSpacing * 0.8f);
                const float cx = grid0 + i * kSpacing + jx;
                const float cz = grid0 + j * kSpacing + jz;
                const float r0 = hash01(i, j);
                const float r1 = hash01(i + 991, j + 53);

                const float height = 5.f + r0 * 45.f;      // 5..50 tall
                const float width  = 8.f + r1 * 10.f;      // 8..18 wide
                addMesh("Building", cube_id, white_id,
                        Eigen::Vector3f(cx, height * 0.5f, cz),
                        Eigen::Vector3f(width, height, width), /*cast_shadow=*/true);

                // Decorative sphere atop the taller buildings.
                if (r0 > 0.75f)
                    addMesh("Dome", sphere_id, white_id,
                            Eigen::Vector3f(cx, height + width * 0.4f, cz),
                            Eigen::Vector3f(width * 0.8f, width * 0.8f, width * 0.8f),
                            /*cast_shadow=*/true);
            }

        // ── Point lights at street intersections (i%6==0 && j%6==0), each with
        //    a co-located emissive bulb (the visible source). ──
        int point_count = 0;
        for (int j = 0; j < kGrid; j += 6)
            for (int i = 0; i < kGrid; i += 6)
            {
                const float cx = grid0 + i * kSpacing;
                const float cz = grid0 + j * kSpacing;
                const int   ci = (i / 6 + j / 6) % kBuiltinEmissiveCount;
                const Eigen::Vector3f col(kBuiltinEmissiveColors[ci][0],
                                          kBuiltinEmissiveColors[ci][1],
                                          kBuiltinEmissiveColors[ci][2]);
                const Eigen::Vector3f pos(cx, 9.f, cz);

                const auto e = reg.create();
                reg.emplace<lux::ecs::NameComponent>(e,
                    lux::ecs::NameComponent{"PointLight"});
                auto& tc = reg.emplace<lux::pack::TransformComponent>(e);
                tc.position = pos;
                tc.scale    = Eigen::Vector3f(2.5f, 2.5f, 2.5f);   // visible glowing bulb
                reg.emplace<lux::pack::WorldTransformComponent>(e);
                // Visible bulb mesh (emissive sphere, same colour as the light).
                lux::pack::MeshComponent mc{};
                mc.mesh_asset_id     = sphere_id;
                mc.material_asset_id  = emissive_id[ci];
                mc.cast_shadow       = false;
                mc.visible           = true;
                reg.emplace<lux::pack::MeshComponent>(e, mc);
                // The light itself (position taken from the transform). With the
                // range-windowed falloff, `range` is the actual reach.
                lux::pack::PointLightComponent pl{};
                pl.color     = col;
                pl.intensity = 3.f;
                pl.range     = 450.f;
                pl.cast_shadow = false;   // sun is the only caster in this demo (keep all 4 atlas pages for the cascades)
                reg.emplace<lux::pack::PointLightComponent>(e, pl);
                ++point_count;
            }

        // ── A sparser set of downward spot lights (i%12==6 && j%12==6). ──
        int spot_count = 0;
        for (int j = 6; j < kGrid; j += 12)
            for (int i = 6; i < kGrid; i += 12)
            {
                const float cx = grid0 + i * kSpacing;
                const float cz = grid0 + j * kSpacing;
                const int   ci = (i + j) % kBuiltinEmissiveCount;
                const Eigen::Vector3f col(kBuiltinEmissiveColors[ci][0],
                                          kBuiltinEmissiveColors[ci][1],
                                          kBuiltinEmissiveColors[ci][2]);
                const Eigen::Vector3f pos(cx, 55.f, cz);

                const auto e = reg.create();
                reg.emplace<lux::ecs::NameComponent>(e,
                    lux::ecs::NameComponent{"SpotLight"});
                auto& tc = reg.emplace<lux::pack::TransformComponent>(e);
                tc.position = pos;
                tc.scale    = Eigen::Vector3f(3.5f, 3.5f, 3.5f);
                reg.emplace<lux::pack::WorldTransformComponent>(e);
                lux::pack::MeshComponent mc{};
                mc.mesh_asset_id     = sphere_id;
                mc.material_asset_id  = emissive_id[ci];
                mc.cast_shadow       = false;
                mc.visible           = true;
                reg.emplace<lux::pack::MeshComponent>(e, mc);
                lux::pack::SpotLightComponent sl{};
                sl.direction = Eigen::Vector3f(0.f, -1.f, 0.f);   // straight down
                sl.color     = col;
                sl.intensity = 4.f;
                sl.range     = 700.f;
                sl.inner_cone_angle = 0.45f;
                sl.outer_cone_angle = 0.70f;
                sl.cast_shadow = false;   // sun is the only caster in this demo (keep all 4 atlas pages for the cascades)
                reg.emplace<lux::pack::SpotLightComponent>(e, sl);
                ++spot_count;
            }

        lux::editor::SceneSaveOptions opts;
        opts.active_camera = camera_e;
        auto saved = lux::editor::Scene::save(scene_file_path, reg, opts);
        if (!saved)
        {
            std::fprintf(stderr, "[writeBigDemoScene] Scene::save failed: %s\n",
                         saved.error().c_str());
            return false;
        }
        std::fprintf(stderr,
            "[writeBigDemoScene] wrote %s (%d point + %d spot lights + sun)\n",
            scene_file_path.string().c_str(), point_count, spot_count);
        return true;
    }

} // namespace lux::editor
