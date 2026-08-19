#include <lux/engine/authoring/world/WorldSourceCodec.hpp>
#include <lux/engine/editor/scene/DemoSceneTemplate.hpp>
#include <lux/engine/editor/scene/WorldActorEcsAdapter.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/scene/ScenePackageCodec.hpp>
#include <lux/engine/toolchain/spatial3d_scene/Spatial3DEntitySceneAdapter.hpp>

#include <lux/engine/ecs/ComponentTypeCatalog.hpp>
#include <lux/engine/ecs/PersistentEntityIndex.hpp>
#include <lux/engine/ecs/components/NameComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/components/Transform3DComponent.hpp>
#include <lux/engine/ecs/components/PersistentEntityIdComponent.hpp>
#include <lux/engine/ecs/systems/HierarchicalTransformSystem.hpp>
#include <lux/engine/ecs/render/components/3d/MeshComponent.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/cxx/algorithm/hash.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

namespace
{
    constexpr std::string_view kTransientProbeSchema =
        "org.lux.test.transient_probe";

    struct TransientProbe final
    {
        std::uint32_t value{0u};
    };

    bool transientProbeHas(
        lux::meta::EntityRegistryBase& registry,
        entt::entity entity)
    {
        return registry.all_of<TransientProbe>(entity);
    }

    void* transientProbeGet(
        lux::meta::EntityRegistryBase& registry,
        entt::entity entity)
    {
        return registry.try_get<TransientProbe>(entity);
    }

    void* transientProbeEmplace(
        lux::meta::EntityRegistryBase& registry,
        entt::entity entity)
    {
        return &registry.get_or_emplace<TransientProbe>(entity);
    }

    void transientProbeRemove(
        lux::meta::EntityRegistryBase& registry,
        entt::entity entity)
    {
        (void)registry.remove<TransientProbe>(entity);
    }

    void transientProbeNotify(
        lux::meta::EntityRegistryBase& registry,
        entt::entity entity)
    {
        registry.patch<TransientProbe>(entity, [](auto&) noexcept {});
    }

    void transientProbeReserve(
        lux::meta::EntityRegistryBase& registry,
        std::size_t additional)
    {
        auto& storage = registry.storage<TransientProbe>();
        storage.reserve(storage.size() + additional);
    }

    void* transientProbeTransfer(
        lux::meta::EntityRegistryBase&,
        entt::entity,
        lux::meta::EntityRegistryBase&,
        entt::entity) noexcept
    {
        return nullptr;
    }

    [[nodiscard]] lux::ecs::ComponentSchemaDescriptor
    transientProbeDescriptor()
    {
        const auto token = lux::ecs::typeToken<TransientProbe>();
        return {
            {token.hash, std::string{token.name}},
            {lux::cxx::algorithm::fnv1a(kTransientProbeSchema),
             std::string{kTransientProbeSchema}},
            1u,
            nullptr,
            {
                &transientProbeHas,
                &transientProbeGet,
                &transientProbeEmplace,
                &transientProbeRemove,
                nullptr,
                &transientProbeNotify,
                &transientProbeReserve,
                &transientProbeTransfer,
                false},
            "org.lux.test",
            {},
            lux::ecs::EComponentSerializationPolicy::TRANSIENT};
    }

    int failures = 0;

    void expect(bool condition, const char* message)
    {
        std::printf("  [%s] %s\n", condition ? "PASS" : "FAIL", message);
        if (!condition)
            ++failures;
    }

}

int main()
{
    namespace fs = std::filesystem;
    std::printf("=== LXAD -> LXSC/LXES round-trip ===\n");

    lux::meta::meta_module_init();
    lux::ecs::ComponentTypeCatalog components;
    expect(
        lux::ecs::registerGeneratedComponents(components).has_value(),
        "generated component schemas registered");
    expect(
        components.registerSchema(transientProbeDescriptor()).has_value(),
        "transient component schema registered");
    const auto* transform_schema = components.findByType(
        lux::ecs::typeToken<lux::ecs::Transform3DComponent>());
    const lux::meta::RefField* position_field = nullptr;
    if (transform_schema && transform_schema->ref_class)
    {
        const auto found = std::ranges::find(
            transform_schema->ref_class->fields,
            std::string_view{"position"},
            &lux::meta::RefField::name);
        if (found != transform_schema->ref_class->fields.end())
            position_field = &*found;
    }
    expect(
        position_field &&
            position_field->type.ptr != nullptr &&
            static_cast<const lux::meta::RefClass*>(
                position_field->type.ptr)->full_name ==
                "lux::spatial::Position3D",
        "non-final Position field retains its nested reflection link");

    const lux::entity_scene::EntitySceneId world_id{
        *uuids::uuid::from_string(
            "99999999-0000-4000-8000-000000000001")};
    const auto mesh_id = *uuids::uuid::from_string(
        "00000000-0000-4000-8000-cccccccccccc");
    lux::meta::EntityRegistry source_registry;
    lux::ecs::PersistentEntityIndex source_persistent_entities{
        source_registry};
    const auto hello = source_registry.create();
    source_registry.emplace<lux::ecs::NameComponent>(
        hello, lux::ecs::NameComponent{"Hello"});
    auto& transform = source_registry.emplace<
        lux::ecs::Transform3DComponent>(hello);
    transform.position = {1.5, 2.5, -3.0};
    transform.scale = Eigen::Vector3f(2.0f, 1.0f, 0.5f);
    const lux::spatial::Position3D hello_position{1.5, 2.5, -3.0};
    source_registry.emplace<lux::ecs::MeshComponent>(
        hello,
        lux::ecs::MeshComponent{
            mesh_id, mesh_id, true, true, false});
    source_registry.emplace<TransientProbe>(hello, 73u);
    const auto peer = source_registry.create();
    source_registry.emplace<lux::ecs::NameComponent>(
        peer, lux::ecs::NameComponent{"World"});
    source_registry.emplace<lux::ecs::Transform3DComponent>(peer);
    const auto child = source_registry.create();
    source_registry.emplace<lux::ecs::NameComponent>(
        child, lux::ecs::NameComponent{"Child"});
    source_registry.emplace<lux::ecs::Transform3DComponent>(
        child).position = {4.0, 0.0, 0.0};
    source_registry.emplace<lux::ecs::ResolvedTransform3DComponent>(
        child,
        lux::ecs::ResolvedTransform3DComponent{
            {5.5, 2.5, -3.0}, Eigen::Matrix3f::Identity()});
    expect(lux::ecs::setParent(source_registry, child, hello),
        "Authoring child linked to its transient ECS parent");

    lux::editor::WorldActorEcsAdapter authoring{
        components, source_persistent_entities};
    auto hello_source = authoring.capture(
        source_registry,
        hello,
        world_id,
        "Hello");
    auto peer_source = authoring.capture(
        source_registry,
        peer,
        world_id,
        "World");
    auto child_source = authoring.capture(
        source_registry,
        child,
        world_id,
        "Child");
    expect(
        hello_source && std::ranges::none_of(
            hello_source->components,
            [](const auto& component)
            {
                return component.schema_name == kTransientProbeSchema;
            }),
        "Authoring capture excludes transient component schemas");
    const lux::authoring::PartitionSpaceId space_id{
        *uuids::uuid::from_string(
            "99999999-0000-4000-8000-000000000010")};
    if (hello_source)
    {
        hello_source->actor_class = "org.lux.test.actor";
        hello_source->space = space_id;
        hello_source->position = hello_position;
    }
    if (peer_source)
    {
        peer_source->actor_class = "org.lux.test.actor";
        peer_source->space = space_id;
        peer_source->position = lux::spatial::Position3D{0.0, 0.0, 0.0};
    }
    if (child_source)
    {
        child_source->actor_class = "org.lux.test.actor";
        child_source->space = space_id;
        child_source->position =
            lux::spatial::Position3D{5.5, 2.5, -3.0};
    }
    auto hello_bytes = hello_source
        ? lux::authoring::encodeWorldActorDocument(*hello_source)
        : decltype(lux::authoring::encodeWorldActorDocument({})){};
    auto peer_bytes = peer_source
        ? lux::authoring::encodeWorldActorDocument(*peer_source)
        : decltype(lux::authoring::encodeWorldActorDocument({})){};
    auto child_bytes = child_source
        ? lux::authoring::encodeWorldActorDocument(*child_source)
        : decltype(lux::authoring::encodeWorldActorDocument({})){};
    expect(hello_bytes && peer_bytes && child_bytes,
        "root and child LXAD Actors encoded independently");
    auto hello_document = hello_bytes
        ? lux::authoring::decodeWorldActorDocument(*hello_bytes)
        : decltype(lux::authoring::decodeWorldActorDocument({})){};
    auto peer_document = peer_bytes
        ? lux::authoring::decodeWorldActorDocument(*peer_bytes)
        : decltype(lux::authoring::decodeWorldActorDocument({})){};
    auto child_document = child_bytes
        ? lux::authoring::decodeWorldActorDocument(*child_bytes)
        : decltype(lux::authoring::decodeWorldActorDocument({})){};
    expect(hello_document && peer_document && child_document,
        "LXAD v2 hierarchy documents decoded");
    expect(
        hello_document && std::ranges::none_of(
            hello_document->components,
            [](const auto& component)
            {
                return component.schema_name == kTransientProbeSchema;
            }),
        "encoded LXAD bytes exclude transient component schemas");

    const auto hello_id = lux::entity_scene::PersistentEntityId{
        source_registry.get<
        lux::ecs::PersistentEntityIdComponent>(hello).id().value()};
    const auto peer_id = lux::entity_scene::PersistentEntityId{
        source_registry.get<
        lux::ecs::PersistentEntityIdComponent>(peer).id().value()};
    expect(!hello_id.empty() && hello_id != peer_id,
        "Authoring assigned distinct stable Actor IDs");
    expect(child_source && child_source->transform_parent == hello_id,
        "Authoring captured the Transform parent as a stable World ID");

    lux::authoring::WorldSourceDocument root =
        lux::authoring::makeWorldSourceDocument(
            lux::authoring::EPartitionTopology::PLANAR_XZ);
    root.contributions.push_back({
        lux::extensions::ContributionId{"org.lux.builtin.physics3d"},
        0u,
        {std::byte{0x10u}, std::byte{0x20u}}});
    auto encoded_root = lux::authoring::encodeWorldSource(root);
    auto decoded_root = encoded_root
        ? lux::authoring::decodeWorldSource(*encoded_root)
        : decltype(lux::authoring::decodeWorldSource({})){};
    expect(decoded_root &&
            decoded_root->contributions == root.contributions,
        "LXWA v4 Root owns only the generic scene contribution plan");

    const auto temp_root = fs::temp_directory_path() / "lux-world-roundtrip";
    std::error_code error;
    fs::remove_all(temp_root, error);
    fs::create_directories(temp_root, error);
    const auto demo = temp_root / "demo.luxworld";
    expect(lux::editor::writeDemoScene(demo, components),
        "demo LXWA/LXAI/LXAD World written");
    auto demo_source = lux::authoring::loadWorldSource(demo);
    expect(demo_source && !demo_source->spaces.empty() &&
            !demo_source->descriptor_pages.empty(),
        "demo LXWA v4 Root indexes external Actor content");
    expect(
        demo_source && demo_source->contributions.size() == 1u &&
            demo_source->contributions.front().id.name() ==
                "org.lux.builtin.presentation3d",
        "demo LXWA v4 Root installs its 3D presentation contribution");
    std::vector<lux::ecs::ComponentSchemaDescriptor> frozen_schemas{
        components.all().begin(), components.all().end()};
    lux::ecs::ComponentTypeCatalog worker_components;
    auto frozen_registration = worker_components.registerSchemas(
        frozen_schemas);
    expect(
        frozen_registration &&
            *frozen_registration == frozen_schemas.size(),
        "Play Cook rebuilds a worker-local catalog from an owning schema snapshot");
    auto cooked_demo = lux::toolchain::cookSpatial3DEntitySceneSource(
        demo,
        worker_components,
        lux::toolchain::Spatial3DMeshAssetCatalog{});
    if (!cooked_demo)
    {
        std::fprintf(
            stderr,
            "Spatial3D EntityScene cook failed: %s\n",
            cooked_demo.error().detail.c_str());
    }
    expect(cooked_demo && !cooked_demo->sections.empty(),
        "demo Authoring World cooked through LXAD -> LXSC/LXES");
    auto decoded_package = cooked_demo
        ? lux::scene::decodeScenePackage(cooked_demo->encoded_package)
        : decltype(lux::scene::decodeScenePackage({})){};
    expect(
        decoded_package && cooked_demo &&
            decoded_package->id == cooked_demo->package.id,
        "cooked LXSC package decodes with the expected Scene identity");
    bool sections_decode = cooked_demo.has_value();
    if (cooked_demo)
    {
        for (const auto& section : cooked_demo->sections)
        {
            auto decoded =
                lux::ecs::scene_format::decodeEntitySectionImage(
                section.encoded_image);
            if (!decoded || decoded->section != section.record.id)
            {
                sections_decode = false;
                break;
            }
        }
    }
    expect(
        sections_decode,
        "every cooked LXES image decodes with its package Section identity");

    fs::remove_all(temp_root, error);
    lux::meta::meta_module_deinit();
    if (failures == 0)
    {
        std::printf("\nAll checks passed.\n");
        return 0;
    }
    std::printf("\n%d check(s) FAILED.\n", failures);
    return 1;
}
