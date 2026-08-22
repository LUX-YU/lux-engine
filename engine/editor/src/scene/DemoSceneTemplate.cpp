#include <lux/engine/editor/scene/DemoSceneTemplate.hpp>

#include <lux/engine/authoring/world/WorldSourceCodec.hpp>
#include <lux/engine/editor/content/EditorBuiltins.hpp>
#include <lux/cxx/algorithm/Sha256.hpp>
#include <lux/engine/editor/scene/WorldActorEcsAdapter.hpp>

#include <lux/engine/ecs/render/components/3d/Camera3DComponent.hpp>
#include <lux/engine/ecs/render/components/PrimaryCameraTag.hpp>
#include <lux/engine/ecs/render/components/3d/DirectionalLightComponent.hpp>
#include <lux/engine/ecs/render/components/3d/PointLightComponent.hpp>
#include <lux/engine/ecs/render/components/3d/SpotLightComponent.hpp>
#include <lux/engine/ecs/render/components/3d/SceneSettingsComponent.hpp>
#include <lux/engine/ecs/render/components/3d/Grid3DComponent.hpp>
#include <lux/engine/ecs/render/components/3d/MeshComponent.hpp>
#include <lux/engine/ecs/components/NameComponent.hpp>
#include <lux/engine/ecs/components/ParentComponent.hpp>
#include <lux/engine/ecs/render/components/3d/SkyboxComponent.hpp>
#include <lux/engine/ecs/components/Transform3DComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/components/PersistentEntityIdComponent.hpp>
#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/Registry.hpp>     // EntityRegistry
#include <lux/engine/function/render/client/resources/lighting/LightDescriptor.hpp>  // LIGHT_FLAG_*

#include <Eigen/Core>
#include <Eigen/Geometry>

#include <cmath>
#include <cstdio>
#include <string>
#include <system_error>
#include <unordered_set>
#include <vector>

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

    bool writeDemoScene(
        const std::filesystem::path& scene_file_path,
        const lux::ecs::ComponentTypeCatalog& components)
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
        if (!parseUuid(lux::engine::content::kBuiltinCubeMeshIdStr, cube_id) ||
            !parseUuid(lux::engine::content::kBuiltinPlaneMeshIdStr, plane_id) ||
            !parseUuid(
                lux::engine::content::kBuiltinWhitePbrMaterialIdStr,
                white_pbr_id))
        {
            std::fprintf(stderr,
                "[writeDemoScene] bad EditorBuiltins UUID literal\n");
            return false;
        }

        // ── Build the World in memory ──
        lux::ecs::Registry reg;
        lux::ecs::PersistentEntityIndex persistent_entities{reg};

        // Editor Camera — looks at the origin from up-and-back.
        const auto camera_e = reg.create();
        reg.emplace<lux::ecs::NameComponent>(camera_e,
            lux::ecs::NameComponent{"Editor Camera"});
        {
            auto& tc = reg.emplace<lux::ecs::Transform3DComponent>(camera_e);
            tc.position = {6.0, 6.0, 8.0};
            // Yaw ~ -35° round Y so the camera faces -Z toward the cube;
            // pitch ~ -30° to look down at the floor.
            const Eigen::Quaternionf q_yaw(
                Eigen::AngleAxisf(-35.f * kPi / 180.f, Eigen::Vector3f::UnitY()));
            const Eigen::Quaternionf q_pitch(
                Eigen::AngleAxisf(-30.f * kPi / 180.f, Eigen::Vector3f::UnitX()));
            tc.rotation = (q_yaw * q_pitch).normalized();
        }
        reg.emplace<lux::ecs::ResolvedTransform3DComponent>(camera_e);
        reg.emplace<lux::ecs::PrimaryCameraTag>(camera_e);
        {
            auto& cc = reg.emplace<lux::ecs::Camera3DComponent>(camera_e);
            cc.fov_rad = 60.f * (kPi / 180.f);
            cc.near_z  = 0.1f;
            cc.far_z   = 500.f;
            cc.aspect  = 16.f / 9.f;
        }

        // Skybox singleton — uses the editor-builtin equirect texture
        // (registered by EditorBuiltins::registerInto from disk). If the
        // file is missing, the Skybox bridge pushes nothing and the pass
        // renders the cleared HDR target; the scene still loads cleanly.
        {
            const auto e = reg.create();
            reg.emplace<lux::ecs::NameComponent>(e,
                lux::ecs::NameComponent{"Skybox"});
            reg.emplace<lux::ecs::Transform3DComponent>(e);
            reg.emplace<lux::ecs::ResolvedTransform3DComponent>(e);
            // Skybox UUID is the texture id that EditorBuiltins manages
            // privately; we leave the field nil here. EditorBuiltins'
            // SkyboxComponent default below works because the loader
            // installs the same skybox id at editor startup — we just
            // don't have the const exposed at link time. Leaving nil
            // means the Skybox bridge is a no-op until the user assigns
            // a texture via the Inspector; acceptable for the demo.
            reg.emplace<lux::ecs::SkyboxComponent>(e,
                lux::ecs::SkyboxComponent{});
        }

        // Grid singleton.
        {
            const auto e = reg.create();
            reg.emplace<lux::ecs::NameComponent>(e,
                lux::ecs::NameComponent{"Grid"});
            reg.emplace<lux::ecs::Transform3DComponent>(e);
            reg.emplace<lux::ecs::ResolvedTransform3DComponent>(e);
            reg.emplace<lux::ecs::Grid3DComponent>(e,
                lux::ecs::Grid3DComponent{});
        }

        // Directional sun light — shadow-casting to exercise the full
        // MeshShadow + ShadowMap pipeline.
        {
            const auto e = reg.create();
            reg.emplace<lux::ecs::NameComponent>(e,
                lux::ecs::NameComponent{"Sun"});
            reg.emplace<lux::ecs::Transform3DComponent>(e);
            reg.emplace<lux::ecs::ResolvedTransform3DComponent>(e);
            lux::ecs::DirectionalLightComponent dl{};
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
            reg.emplace<lux::ecs::DirectionalLightComponent>(e, dl);
        }

        // Floor — 10×10 plane at y=0 with the editor-builtin white PBR.
        {
            const auto e = reg.create();
            reg.emplace<lux::ecs::NameComponent>(e,
                lux::ecs::NameComponent{"Floor"});
            reg.emplace<lux::ecs::Transform3DComponent>(e);
            reg.emplace<lux::ecs::ResolvedTransform3DComponent>(e);
            lux::ecs::MeshComponent mc{};
            mc.mesh_asset_id     = plane_id;
            mc.material_asset_id = white_pbr_id;
            mc.cast_shadow       = true;
            mc.visible           = true;
            reg.emplace<lux::ecs::MeshComponent>(e, mc);
        }

        // Cube — lifted to y=1.5 so its shadow falls on the floor.
        {
            const auto e = reg.create();
            reg.emplace<lux::ecs::NameComponent>(e,
                lux::ecs::NameComponent{"Cube"});
            auto& tc = reg.emplace<lux::ecs::Transform3DComponent>(e);
            tc.position = {0.0, 1.5, 0.0};
            reg.emplace<lux::ecs::ResolvedTransform3DComponent>(e);
            lux::ecs::MeshComponent mc{};
            mc.mesh_asset_id     = cube_id;
            mc.material_asset_id = white_pbr_id;
            mc.cast_shadow       = true;
            mc.visible           = true;
            reg.emplace<lux::ecs::MeshComponent>(e, mc);
        }

        // ── Persist as one LXWA root plus content-addressed external Actors ──
        // The launcher must create the same Authoring structure the live
        // Editor writes: LXAD Actors are indexed by an LXAI macro page; the
        // compact LXWA root references that page and is committed last.
        auto source = lux::authoring::makeWorldSourceDocument(
            lux::authoring::EPartitionTopology::PLANAR_XZ);
        source.contributions.push_back({
            lux::authoring::WorldSceneFeatureId{
                "org.lux.builtin.presentation3d"},
            0u,
            {}});
        lux::authoring::WorldDescriptorPageDocument descriptor_page;
        descriptor_page.world = source.world;
        descriptor_page.space = source.spaces.front().id;
        descriptor_page.macro = {
            lux::authoring::EPartitionTopology::PLANAR_XZ,
            lux::authoring::PlanarMacroCoord{0, 0}};
        descriptor_page.id = lux::authoring::makeWorldDescriptorPageId(
            source.world,
            descriptor_page.space,
            descriptor_page.macro);
        WorldActorEcsAdapter actor_adapter{components, persistent_entities};
        std::unordered_set<std::string> stable_ids;
        for (const auto entity : reg.view<entt::entity>())
        {
            auto actor_document = actor_adapter.capture(
                reg,
                entity,
                source.world,
                "demo external Actor");
            if (!actor_document)
            {
                std::fprintf(
                    stderr,
                    "[writeDemoScene] Actor capture failed: %s\n",
                    actor_document.error().c_str());
                return false;
            }
            const auto* stable = reg.try_get<
                lux::ecs::PersistentEntityIdComponent>(entity);
            if (!stable || stable->id().empty() ||
                !stable_ids.insert(
                    uuids::to_string(stable->id().value())).second)
            {
                std::fprintf(
                    stderr,
                    "[writeDemoScene] Actor identity is missing or duplicate\n");
                return false;
            }
            lux::authoring::WorldActorSourceDescriptor descriptor;
            descriptor.id = lux::authoring::WorldActorId{
                stable->id().value()};
            descriptor.actor_class = "org.lux.actor";
            descriptor.space = source.spaces.front().id;
            if (const auto* name =
                    reg.try_get<lux::ecs::NameComponent>(entity))
            {
                descriptor.display_name = name->name;
            }
            if (descriptor.display_name.empty())
            {
                descriptor.display_name =
                    "Actor " + uuids::to_string(stable->id().value());
            }

            descriptor.position = actor_document->position;
            descriptor.transform_parent =
                actor_document->transform_parent;
            if (descriptor.transform_parent)
            {
                descriptor.references.push_back({
                    *descriptor.transform_parent,
                    lux::authoring::EWorldActorReferenceKind::LOCAL});
            }
            actor_document->actor_class = descriptor.actor_class;
            actor_document->space = descriptor.space;
            actor_document->position = descriptor.position;
            actor_document->transform_parent =
                descriptor.transform_parent;
            actor_document->data_layers = descriptor.data_layers;
            actor_document->references = descriptor.references;
            auto encoded = lux::authoring::encodeWorldActorDocument(
                *actor_document);
            if (!encoded)
            {
                std::fprintf(
                    stderr,
                    "[writeDemoScene] Actor encode failed: %s\n",
                    encoded.error().c_str());
                return false;
            }
            const auto content_digest = lux::cxx::algorithm::Sha256::hash(*encoded);
            descriptor.content_digest = content_digest;
            descriptor.document_path =
                lux::authoring::makeWorldActorDocumentPath(
                    descriptor.id,
                    content_digest);
            if (auto saved = lux::authoring::saveWorldSourceDocument(
                    scene_file_path,
                    descriptor.document_path,
                    *encoded);
                !saved)
            {
                std::fprintf(
                    stderr,
                    "[writeDemoScene] Actor write failed: %s\n",
                    saved.error().c_str());
                return false;
            }
            descriptor_page.actors.push_back(std::move(descriptor));
        }

        auto descriptor_bytes =
            lux::authoring::encodeWorldDescriptorPage(
                source,
                descriptor_page);
        if (!descriptor_bytes)
        {
            std::fprintf(
                stderr,
                "[writeDemoScene] Descriptor Page encode failed: %s\n",
                descriptor_bytes.error().c_str());
            return false;
        }
        const auto descriptor_digest = lux::cxx::algorithm::Sha256::hash(
            *descriptor_bytes);
        const auto descriptor_path =
            lux::authoring::makeWorldDescriptorPagePath(
                descriptor_page.id,
                descriptor_digest);
        if (auto descriptor_saved =
                lux::authoring::saveWorldSourceDocument(
                    scene_file_path,
                    descriptor_path,
                    *descriptor_bytes);
            !descriptor_saved)
        {
            std::fprintf(
                stderr,
                "[writeDemoScene] Descriptor Page write failed: %s\n",
                descriptor_saved.error().c_str());
            return false;
        }
        source.descriptor_pages.push_back({
            descriptor_page.id,
            descriptor_page.space,
            descriptor_page.macro,
            descriptor_path,
            descriptor_digest,
            static_cast<std::uint32_t>(descriptor_page.actors.size()),
            0u});
        auto saved = lux::authoring::saveWorldSource(
            scene_file_path,
            source);
        if (!saved)
        {
            std::fprintf(
                stderr,
                "[writeDemoScene] World root write failed: %s\n",
                saved.error().c_str());
            return false;
        }
        return true;
    }

} // namespace lux::editor
