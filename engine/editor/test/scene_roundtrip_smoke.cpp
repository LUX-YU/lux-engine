/// Smoke test: Scene::save + Scene::load round-trip.
///
/// Builds a small entt::registry by hand with a few annotated components
/// (NameComponent, TransformComponent, MeshComponent), saves it to a
/// tmpdir, loads it into a fresh registry, and verifies every field
/// survived the trip.

#include <lux/engine/editor/scene/Scene.hpp>

#include <lux/engine/gameplay/ComponentTypeRegistry.hpp>
#include <lux/engine/gameplay/3d/world/components/MeshComponent.hpp>
#include <lux/engine/gameplay/world/components/NameComponent.hpp>
#include <lux/engine/gameplay/3d/world/components/TransformComponent.hpp>
#include <lux/engine/meta/Meta.hpp>           // meta_module_init / deinit

#include <entt/entt.hpp>
#include <uuid.h>

#include <cstdio>
#include <filesystem>
#include <string>

namespace
{
    int fail_count = 0;

    void expect(bool cond, const char* msg)
    {
        std::printf("  [%s] %s\n", cond ? "PASS" : "FAIL", msg);
        if (!cond) ++fail_count;
    }
} // namespace

int main()
{
    namespace fs = std::filesystem;

    // Drain the meta-module init queue so ReflectionRegistry +
    // ComponentTypeRegistry are populated. Without this Scene::save sees
    // an empty catalogue and writes a scene with zero components.
    lux::meta::meta_module_init();

    const auto tmp_root = fs::temp_directory_path() / "lux_test_scene";
    std::error_code ec;
    fs::remove_all(tmp_root, ec);
    fs::create_directories(tmp_root, ec);
    const auto out = tmp_root / "test.luxscene";

    std::printf("=== Phase D smoke: Scene round-trip ===\n");

    // Cube reference UUID — same value the engine builtins use.
    const auto kCubeUuid = *uuids::uuid::from_string("00000000-0000-4000-8000-cccccccccccc");

    // ── Build a 2-entity registry. ──
    lux::meta::EntityRegistry src_reg;

    const auto e0 = src_reg.create();
    src_reg.emplace<lux::gameplay::NameComponent>(e0,
        lux::gameplay::NameComponent{"Hello"});
    {
        auto& tc = src_reg.emplace<lux::gameplay::d3::TransformComponent>(e0);
        tc.position = Eigen::Vector3f(1.5f, 2.5f, -3.0f);
        tc.scale    = Eigen::Vector3f(2.0f, 1.0f, 0.5f);
    }
    src_reg.emplace<lux::gameplay::d3::MeshComponent>(e0,
        lux::gameplay::d3::MeshComponent{kCubeUuid, kCubeUuid, true, true, false});

    const auto e1 = src_reg.create();
    src_reg.emplace<lux::gameplay::NameComponent>(e1,
        lux::gameplay::NameComponent{"World"});

    // ── Save. ──
    {
        lux::editor::SceneSaveOptions opts;
        opts.active_camera = e0;
        auto r = lux::editor::Scene::save(out, src_reg, opts);
        expect(r.has_value(), "save returned ok");
        expect(fs::exists(out), "file was written");
        expect(fs::file_size(out) > 64, "file is non-trivial in size");
    }

    // ── Load into a fresh registry. ──
    lux::meta::EntityRegistry dst_reg;
    lux::editor::SceneLoadResult result{};
    {
        auto r = lux::editor::Scene::load(out, dst_reg, result);
        expect(r.has_value(), "load returned ok");
        expect(result.created_entities.size() == 2u, "two entities created");
    }

    // ── Verify components survived. ──
    //
    // Entity iteration order is entt-implementation-defined, so look up
    // entities by NameComponent value rather than index. This is the same
    // pattern editor UI would use when "finding" an entity post-load.
    auto findByName = [&](std::string_view want) -> entt::entity {
        for (auto e : result.created_entities)
        {
            auto* nc = dst_reg.try_get<lux::gameplay::NameComponent>(e);
            if (nc && nc->name == want) return e;
        }
        return entt::null;
    };

    const auto hello = findByName("Hello");
    const auto world = findByName("World");

    expect(hello != entt::null, "found entity 'Hello' by name");
    expect(world != entt::null, "found entity 'World' by name");

    if (hello != entt::null)
    {
        expect(dst_reg.all_of<lux::gameplay::d3::TransformComponent>(hello),  "Hello has TransformComponent");
        expect(dst_reg.all_of<lux::gameplay::d3::MeshComponent>(hello),       "Hello has MeshComponent");

        if (auto* tc = dst_reg.try_get<lux::gameplay::d3::TransformComponent>(hello))
        {
            const bool pos_ok = tc->position.isApprox(Eigen::Vector3f(1.5f, 2.5f, -3.0f), 1e-6f);
            const bool scl_ok = tc->scale.isApprox(   Eigen::Vector3f(2.0f, 1.0f,  0.5f), 1e-6f);
            expect(pos_ok, "Hello Transform.position preserved");
            expect(scl_ok, "Hello Transform.scale preserved");
        }

        if (auto* mc = dst_reg.try_get<lux::gameplay::d3::MeshComponent>(hello))
        {
            expect(mc->mesh_asset_id == kCubeUuid,
                                                   "Hello Mesh.mesh_asset_id (asset_id_t / uuid) preserved");
            expect(mc->material_asset_id == kCubeUuid,
                                                   "Hello Mesh.material_asset_id preserved");
            expect(mc->cast_shadow == true,        "Hello Mesh.cast_shadow preserved");
            expect(mc->visible == true,            "Hello Mesh.visible preserved");
            expect(mc->two_sided_shadow == false,  "Hello Mesh.two_sided_shadow preserved");
        }

        expect(result.active_camera == hello, "active_camera resolved back to Hello");
    }

    if (world != entt::null)
    {
        // 'World' should be NameComponent-only.
        expect(!dst_reg.all_of<lux::gameplay::d3::TransformComponent>(world),
            "World does NOT have TransformComponent (preserved minimal entity)");
        expect(!dst_reg.all_of<lux::gameplay::d3::MeshComponent>(world),
            "World does NOT have MeshComponent");
    }

    fs::remove_all(tmp_root, ec);
    lux::meta::meta_module_deinit();

    if (fail_count == 0)
    {
        std::printf("\nAll checks passed.\n");
        return 0;
    }
    std::printf("\n%d check(s) FAILED.\n", fail_count);
    return 1;
}
