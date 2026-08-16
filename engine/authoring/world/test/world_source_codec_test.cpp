#include <lux/engine/authoring/world/WorldSourceCodec.hpp>
#include <lux/engine/authoring/world/WorldDescriptorIndex.hpp>
#include <lux/engine/authoring/world/WorldAuthoringTransaction.hpp>

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <limits>
#include <vector>

namespace
{
    uuids::uuid uuid(const char* value)
    {
        return uuids::uuid::from_string(value).value();
    }

    struct Fixture final
    {
        lux::authoring::WorldSourceDocument root;
        lux::authoring::WorldDescriptorPageDocument page;
    };

    Fixture sourceDocument()
    {
        using namespace lux::authoring;
        using namespace lux::authoring;
        Fixture fixture;
        auto& root = fixture.root;
        root.world = lux::entity_scene::EntitySceneId{
            uuid("81000000-0000-4000-8000-000000000001")};
        root.contributions.push_back({
            lux::extensions::ContributionId{"org.example.scene"},
            1u,
            {std::byte{0x42u}}});
        PartitionSpaceDescriptor surface;
        surface.id = PartitionSpaceId{
            uuid("82000000-0000-4000-8000-000000000001")};
        surface.topology = EPartitionTopology::PLANAR_XZ;
        surface.cell_edge = 128.0f;
        surface.macro_edge_cells = 32u;
        root.spaces.push_back(surface);
        root.data_layers.push_back(
            DataLayerId{"org.example.gameplay"});
        root.required_extensions.push_back({
            lux::extensions::ExtensionId{"org.example.runtime"},
            1u,
            2u});
        root.instance_sets.push_back({
            InstanceSetId{
                uuid("85000000-0000-4000-8000-000000000001")},
            4u});

        auto& page = fixture.page;
        page.world = root.world;
        page.space = surface.id;
        page.macro = {
            EPartitionTopology::PLANAR_XZ,
            PlanarMacroCoord{0, 0}};
        page.id = makeWorldDescriptorPageId(
            root.world, page.space, page.macro);

        WorldActorSourceDescriptor actor_a;
        actor_a.id = lux::entity_scene::PersistentEntityId{
            uuid("83000000-0000-4000-8000-000000000001")};
        actor_a.display_name = "Gate A";
        actor_a.actor_class = "org.example.gate";
        const std::array<std::byte, 1u> actor_content{
            std::byte{0x41u}};
        actor_a.content_digest = lux::cxx::algorithm::Sha256::hash(actor_content);
        actor_a.document_path = makeWorldActorDocumentPath(
            actor_a.id, actor_a.content_digest);
        actor_a.space = surface.id;
        actor_a.position = lux::spatial::Position3D{100.0, 0.0, 15.0};
        actor_a.bounds_half_extent = {2.0f, 3.0f, 4.0f};
        actor_a.data_layers.push_back(DataLayerId{"org.example.gameplay"});

        WorldActorSourceDescriptor actor_b = actor_a;
        actor_b.id = lux::entity_scene::PersistentEntityId{
            uuid("83000000-0000-4000-8000-000000000002")};
        actor_b.display_name = "Gate B";
        actor_b.document_path = makeWorldActorDocumentPath(
            actor_b.id, actor_b.content_digest);
        actor_b.position = lux::spatial::Position3D{200.0, 0.0, 15.0};
        actor_a.transform_parent = actor_b.id;
        actor_a.references.push_back({
            actor_b.id, EWorldActorReferenceKind::LOCAL});
        actor_b.references.push_back({
            actor_a.id, EWorldActorReferenceKind::REQUIRED});
        page.actors = {actor_b, actor_a};

        WorldPageSourceDescriptor content;
        content.id = uuid("84000000-0000-4000-8000-000000000001");
        content.kind = EWorldPageSourceKind::TERRAIN;
        content.owner = lux::authoring::TerrainSetId{
            uuid("88000000-0000-4000-8000-000000000001")};
        content.document_path = "Terrain/0_0.lxtp";
        content.space = surface.id;
        content.cell = {
            EPartitionTopology::PLANAR_XZ,
            PlanarCellCoord{0, 0}};
        const std::array<std::byte, 1u> data{std::byte{42u}};
        content.content_digest = lux::cxx::algorithm::Sha256::hash(data);
        page.pages.push_back(content);

        const auto encoded_page = encodeWorldDescriptorPage(root, page);
        assert(encoded_page);
        const auto digest = lux::cxx::algorithm::Sha256::hash(*encoded_page);
        root.descriptor_pages.push_back({
            page.id,
            page.space,
            page.macro,
            makeWorldDescriptorPagePath(page.id, digest),
            digest,
            static_cast<std::uint32_t>(page.actors.size()),
            static_cast<std::uint32_t>(page.pages.size())});
        return fixture;
    }
}

int main()
{
    using namespace lux::authoring;
    auto fixture = sourceDocument();

    auto generated_source = makeWorldSourceDocument(
        lux::authoring::EPartitionTopology::PLANAR_XY);
    generated_source.data_layers.push_back(
        lux::authoring::DataLayerId{"org.example.generated"});
    generated_source.contributions.push_back({
        lux::extensions::ContributionId{"org.example.pixel"},
        2u,
        {std::byte{0x7fu}}});
    const auto generated_bytes = encodeWorldSource(generated_source);
    assert(generated_bytes);
    const auto generated_roundtrip = decodeWorldSource(*generated_bytes);
    assert(generated_roundtrip &&
        generated_roundtrip->contributions ==
            generated_source.contributions &&
        generated_roundtrip->data_layers == generated_source.data_layers);

    const auto page_first = encodeWorldDescriptorPage(
        fixture.root, fixture.page);
    assert(page_first);
    std::ranges::reverse(fixture.page.actors);
    const auto page_second = encodeWorldDescriptorPage(
        fixture.root, fixture.page);
    assert(page_second && *page_first == *page_second);

    const auto root_first = encodeWorldSource(fixture.root);
    assert(root_first);
    const auto root_decoded = decodeWorldSource(*root_first);
    assert(root_decoded && root_decoded->descriptor_pages.size() == 1u);
    assert(root_decoded->contributions == fixture.root.contributions);
    assert(root_decoded->required_extensions ==
        fixture.root.required_extensions);
    assert(root_decoded->descriptor_pages.front().actor_count == 2u);
    assert(root_decoded->descriptor_pages.front().page_count == 1u);
    auto legacy_root = *root_first;
    const std::uint32_t legacy_root_version = 3u;
    std::memcpy(
        legacy_root.data() + sizeof(std::uint32_t),
        &legacy_root_version,
        sizeof(legacy_root_version));
    assert(!decodeWorldSource(legacy_root));
    const auto page_decoded = decodeWorldDescriptorPage(
        *root_decoded, *page_first);
    assert(page_decoded && page_decoded->actors.size() == 2u);
    assert(page_decoded->actors.front().id.value() ==
        uuid("83000000-0000-4000-8000-000000000001"));
    auto legacy_descriptor_page = *page_first;
    const std::uint32_t legacy_descriptor_version = 1u;
    std::memcpy(
        legacy_descriptor_page.data() + sizeof(std::uint32_t),
        &legacy_descriptor_version,
        sizeof(legacy_descriptor_version));
    assert(!decodeWorldDescriptorPage(
        *root_decoded,
        legacy_descriptor_page));

    auto invalid_page = fixture.page;
    invalid_page.actors.front().document_path = "../escape.lxad";
    assert(!encodeWorldDescriptorPage(fixture.root, invalid_page));
    invalid_page = fixture.page;
    invalid_page.actors.front().position =
        lux::spatial::Position3D{100000.0, 0.0, 0.0};
    assert(!encodeWorldDescriptorPage(fixture.root, invalid_page));
    std::vector<std::byte> oversized_descriptor_page(
        static_cast<std::size_t>(WorldSourceCodecLimits{}
            .maximum_descriptor_page_bytes) + 1u);
    assert(!decodeWorldDescriptorPage(
        fixture.root,
        oversized_descriptor_page));

    auto corrupt = *root_first;
    corrupt.push_back(std::byte{0u});
    assert(!decodeWorldSource(corrupt));
    auto invalid_contribution = fixture.root;
    invalid_contribution.contributions.front().id =
        lux::extensions::ContributionId{"Org.Example.Scene"};
    assert(!encodeWorldSource(invalid_contribution));

    const auto nonce = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    const auto directory = std::filesystem::temp_directory_path()
        / ("lux-world-source-v4-" + std::to_string(nonce));
    const auto root_path = directory / "Worlds" / "Main.luxworld";
    const auto& reference = fixture.root.descriptor_pages.front();
    assert(saveWorldSourceDocument(
        root_path, reference.document_path, *page_first));
    assert(saveWorldSource(root_path, fixture.root));
    assert(saveWorldSource(root_path, fixture.root));
    const std::array<std::byte, 2u> live_actor_bytes{
        std::byte{0x10u}, std::byte{0x20u}};
    const std::array<std::byte, 1u> orphan_bytes{
        std::byte{0x7fu}};
    assert(saveWorldSourceDocument(
        root_path,
        fixture.page.actors.front().document_path,
        live_actor_bytes));
    assert(saveWorldSourceDocument(
        root_path,
        "Actors/orphan.lxad",
        orphan_bytes));
    const auto garbage = collectWorldSourceGarbage(
        root_path,
        WorldSourceGarbageCollectionConfig{
            .grace_period = std::chrono::seconds::zero(),
            .maximum_removals_per_pass = 1u});
    assert(garbage && garbage->removed_documents == 1u &&
        !garbage->removal_budget_exhausted);
    assert(resolveWorldSourceDocument(
        root_path,
        fixture.page.actors.front().document_path) &&
        std::filesystem::exists(
            *resolveWorldSourceDocument(
                root_path,
                fixture.page.actors.front().document_path)));
    assert(!std::filesystem::exists(
        root_path.parent_path() / "Actors" / "orphan.lxad"));
    const auto loaded_root = loadWorldSource(root_path);
    assert(loaded_root && loaded_root->descriptor_pages.size() == 1u);
    const auto loaded_page = loadWorldDescriptorPage(
        root_path, *loaded_root, loaded_root->descriptor_pages.front());
    assert(loaded_page && loaded_page->actors.size() == 2u);

    const auto cache_path = worldDescriptorIndexCachePath(
        directory / "cache", loaded_root->world);
    auto rebuilt_index = WorldDescriptorIndex::rebuild(
        root_path, *loaded_root, cache_path);
    assert(rebuilt_index && rebuilt_index->actorCount() == 2u);
    assert(rebuilt_index->pageCount() == 1u);
    auto loaded_index = WorldDescriptorIndex::load(
        cache_path, *loaded_root);
    assert(loaded_index && loaded_index->actorCount() == 2u);
    assert(loaded_index->stats().cached_bytes == 0u);
    const auto actor_id = lux::entity_scene::PersistentEntityId{
        uuid("83000000-0000-4000-8000-000000000001")};
    const auto indexed_actor = loaded_index->find(actor_id);
    assert(indexed_actor && indexed_actor->display_name == "Gate A");
    const auto source_actor = std::ranges::find(
        fixture.page.actors,
        actor_id,
        &WorldActorSourceDescriptor::id);
    assert(source_actor != fixture.page.actors.end());
    assert(indexed_actor->position == source_actor->position);
    assert(indexed_actor->bounds_half_extent ==
        source_actor->bounds_half_extent);
    assert(loaded_index->page(indexed_actor->descriptor_page));
    assert(loaded_index->actorsInPage(
        indexed_actor->descriptor_page).size() == 2u);
    assert(loaded_index->search("gate b", 0u, 1u).front().actor.value() ==
        uuid("83000000-0000-4000-8000-000000000002"));
    assert(loaded_index->stats().cached_search_objects == 1u);
    assert(loaded_index->stats().cached_bytes <=
        loaded_index->stats().cache_budget_bytes);

    const auto object_directory = cache_path.parent_path() / "objects";
    assert(std::filesystem::is_directory(object_directory));
    assert(std::distance(
        std::filesystem::directory_iterator(object_directory),
        std::filesystem::directory_iterator{}) >= 2);

    WorldDescriptorPageDocument distant_page;
    distant_page.world = loaded_root->world;
    distant_page.space = loaded_root->spaces.front().id;
    distant_page.macro = {
        lux::authoring::EPartitionTopology::PLANAR_XZ,
        lux::authoring::PlanarMacroCoord{1, 0}};
    distant_page.id = makeWorldDescriptorPageId(
        distant_page.world, distant_page.space, distant_page.macro);
    auto distant_actor = fixture.page.actors.front();
    distant_actor.id = lux::entity_scene::PersistentEntityId{
        uuid("93000000-0000-4000-8000-000000000001")};
    distant_actor.display_name = "Distant Gate";
    distant_actor.document_path = makeWorldActorDocumentPath(
        distant_actor.id, distant_actor.content_digest);
    distant_actor.position = lux::spatial::Position3D{
        5000.0, 0.0, 15.0};
    distant_actor.transform_parent.reset();
    distant_actor.references.clear();
    distant_page.actors.push_back(distant_actor);

    auto expanded_root = *loaded_root;
    const auto distant_bytes = encodeWorldDescriptorPage(
        expanded_root, distant_page);
    assert(distant_bytes);
    const auto distant_digest = lux::cxx::algorithm::Sha256::hash(*distant_bytes);
    expanded_root.descriptor_pages.push_back({
        distant_page.id,
        distant_page.space,
        distant_page.macro,
        makeWorldDescriptorPagePath(distant_page.id, distant_digest),
        distant_digest,
        1u,
        0u});
    const std::array added_pages{distant_page};
    assert(loaded_index->updatePages(expanded_root, added_pages));
    assert(loaded_index->actorCount() == 3u);
    assert(loaded_index->find(distant_actor.id));

    auto changed_page = *loaded_page;
    const auto changed_actor = std::ranges::find(
        changed_page.actors,
        actor_id,
        &WorldActorSourceDescriptor::id);
    assert(changed_actor != changed_page.actors.end());
    changed_actor->display_name = "Gate A Prime";
    auto changed_root = expanded_root;
    const auto changed_bytes = encodeWorldDescriptorPage(
        changed_root, changed_page);
    assert(changed_bytes);
    const auto changed_digest = lux::cxx::algorithm::Sha256::hash(*changed_bytes);
    changed_root.descriptor_pages.front().document_path =
        makeWorldDescriptorPagePath(changed_page.id, changed_digest);
    changed_root.descriptor_pages.front().content_digest = changed_digest;
    assert(!loaded_index->updatePages(changed_root, {}));
    const std::array changed_pages{changed_page};
    assert(loaded_index->updatePages(changed_root, changed_pages));

    auto incrementally_loaded = WorldDescriptorIndex::load(
        cache_path, changed_root);
    assert(incrementally_loaded && incrementally_loaded->actorCount() == 3u);
    const auto changed_index_actor = incrementally_loaded->find(actor_id);
    assert(changed_index_actor &&
        changed_index_actor->display_name == "Gate A Prime");
    assert(incrementally_loaded->search("gate a prime", 0u, 1u).size() ==
        1u);
    assert(incrementally_loaded->search("distant", 0u, 1u).front().actor ==
        distant_actor.id);
    assert(incrementally_loaded->search("gate a", 0u, 8u).size() == 1u);
    assert(incrementally_loaded->find(distant_actor.id));
    assert(!WorldDescriptorIndex::load(cache_path, expanded_root));

    auto stale_root = changed_root;
    stale_root.contributions.front().config.push_back(std::byte{0x11u});
    assert(!WorldDescriptorIndex::load(cache_path, stale_root));

    auto bad_reference = loaded_root->descriptor_pages.front();
    bad_reference.content_digest[0] ^= std::byte{0xffu};
    assert(!loadWorldDescriptorPage(root_path, *loaded_root, bad_reference));
    assert(!resolveWorldSourceDocument(root_path, "../escape.lxad"));

    const auto empty_2d = makeWorldSourceDocument(
        lux::authoring::EPartitionTopology::PLANAR_XY);
    assert(!empty_2d.world.empty());
    assert(empty_2d.spaces.front().topology ==
        lux::authoring::EPartitionTopology::PLANAR_XY);
    assert(encodeWorldSource(empty_2d));

    const std::array<std::byte, 1u> actor_data{std::byte{0x12u}};
    const auto actor_digest = lux::cxx::algorithm::Sha256::hash(actor_data);
    assert(makeWorldActorDocumentPath(
        lux::entity_scene::PersistentEntityId{
            uuid("83000000-0000-4000-8000-000000000001")},
        actor_digest) ==
        "Actors/83/83000000-0000-4000-8000-000000000001/"
        + lux::cxx::algorithm::toHex(actor_digest) + ".lxad");

    WorldActorDocument actor_document;
    actor_document.world = fixture.root.world;
    actor_document.actor = fixture.page.actors.front().id;
    actor_document.actor_class = fixture.page.actors.front().actor_class;
    actor_document.space = fixture.page.actors.front().space;
    actor_document.position = fixture.page.actors.front().position;
    actor_document.transform_parent =
        fixture.page.actors.front().transform_parent;
    actor_document.data_layers = fixture.page.actors.front().data_layers;
    actor_document.references = fixture.page.actors.front().references;
    actor_document.name_table = {
        std::byte{1u}, std::byte{0u},
        std::byte{0u}, std::byte{0u}};
    const auto encoded_actor_document = encodeWorldActorDocument(
        actor_document);
    assert(encoded_actor_document);
    const auto decoded_actor_document = decodeWorldActorDocument(
        *encoded_actor_document);
    assert(decoded_actor_document &&
        decoded_actor_document->actor_class == actor_document.actor_class &&
        decoded_actor_document->space == actor_document.space &&
        decoded_actor_document->position == actor_document.position &&
        decoded_actor_document->transform_parent ==
            actor_document.transform_parent &&
        decoded_actor_document->data_layers == actor_document.data_layers &&
        decoded_actor_document->references == actor_document.references);
    actor_document.position = lux::spatial::Position3D{
        1'000'000'000'000.001,
        -1'000'000'000'000.001,
        0.001};
    const auto large_actor_document = encodeWorldActorDocument(
        actor_document);
    const auto decoded_large_actor_document = large_actor_document
        ? decodeWorldActorDocument(*large_actor_document)
        : decltype(decodeWorldActorDocument({})){};
    assert(decoded_large_actor_document &&
        decoded_large_actor_document->position == actor_document.position);
    auto planar_actor_document = actor_document;
    planar_actor_document.position = lux::spatial::Position2D{
        -1'000'000'000'000.001,
        1'000'000'000'000.001};
    const auto encoded_planar_actor_document = encodeWorldActorDocument(
        planar_actor_document);
    const auto decoded_planar_actor_document =
        encoded_planar_actor_document
        ? decodeWorldActorDocument(*encoded_planar_actor_document)
        : decltype(decodeWorldActorDocument({})){};
    assert(decoded_planar_actor_document &&
        decoded_planar_actor_document->position ==
            planar_actor_document.position);
    auto legacy_actor_document = *encoded_actor_document;
    const std::uint32_t legacy_actor_version = 1u;
    std::memcpy(
        legacy_actor_document.data() + sizeof(std::uint32_t),
        &legacy_actor_version,
        sizeof(legacy_actor_version));
    assert(!decodeWorldActorDocument(legacy_actor_document));
    auto invalid_actor_position = actor_document;
    invalid_actor_position.position = lux::spatial::Position3D{
        std::numeric_limits<double>::infinity(), 0.0, 0.0};
    assert(!encodeWorldActorDocument(invalid_actor_position));

    WorldInstancePageDocument instance_page;
    instance_page.world = fixture.root.world;
    instance_page.instance_set = lux::authoring::InstanceSetId{
        uuid("85000000-0000-4000-8000-000000000001")};
    instance_page.space = fixture.root.spaces.front().id;
    instance_page.cell = {
        lux::authoring::EPartitionTopology::PLANAR_XZ,
        lux::authoring::PlanarCellCoord{0, 0}};
    EditableWorldInstance instance;
    instance.id = {instance_page.instance_set, 2u};
    instance.position = lux::spatial::Position3D{10.0, 2.0, 12.0};
    instance.rotation = {0.0f, 0.0f, 0.0f, 2.0f};
    instance.mesh = uuid("86000000-0000-4000-8000-000000000001");
    instance.data_layers.push_back(
        lux::authoring::DataLayerId{"org.example.gameplay"});
    instance_page.instances.push_back(instance);
    instance_page.tombstones.push_back(1u);
    const auto encoded_instances = encodeWorldInstancePage(
        fixture.root, instance_page);
    assert(encoded_instances);
    const auto decoded_instances = decodeWorldInstancePage(
        fixture.root, *encoded_instances);
    assert(decoded_instances && decoded_instances->instances.size() == 1u);
    assert(decoded_instances->instances.front().id.local_id == 2u);
    assert(decoded_instances->instances.front().rotation[3] == 1.0f);
    auto legacy_instances = *encoded_instances;
    const std::uint32_t legacy_instance_version = 1u;
    std::memcpy(
        legacy_instances.data() + sizeof(std::uint32_t),
        &legacy_instance_version,
        sizeof(legacy_instance_version));
    assert(!decodeWorldInstancePage(fixture.root, legacy_instances));
    auto rotated_instances = instance_page;
    rotated_instances.instances.front().rotation = {
        0.0f, std::sin(0.37f), 0.0f, std::cos(0.37f)};
    const auto encoded_rotated = encodeWorldInstancePage(
        fixture.root, rotated_instances);
    assert(encoded_rotated);
    const auto decoded_rotated = decodeWorldInstancePage(
        fixture.root, *encoded_rotated);
    assert(decoded_rotated);
    const auto reencoded_rotated = encodeWorldInstancePage(
        fixture.root, *decoded_rotated);
    assert(reencoded_rotated && *reencoded_rotated == *encoded_rotated);
    auto invalid_instances = instance_page;
    invalid_instances.instances.front().position =
        lux::spatial::Position3D{4096.0, 0.0, 0.0};
    assert(!encodeWorldInstancePage(fixture.root, invalid_instances));
    const auto instance_digest = lux::cxx::algorithm::Sha256::hash(*encoded_instances);
    const auto instance_path = makeWorldInstancePagePath(
        instance_page.instance_set, instance_page.cell, instance_digest);
    assert(!instance_path.empty());
    assert(saveWorldSourceDocument(
        root_path, instance_path, *encoded_instances));
    assert(loadWorldInstancePage(
        root_path, instance_path, fixture.root));

    WorldActorSourceDescriptor converted_descriptor;
    converted_descriptor.id = lux::entity_scene::PersistentEntityId{
        uuid("87000000-0000-4000-8000-000000000001")};
    converted_descriptor.display_name = "Converted Instance";
    converted_descriptor.actor_class = "org.example.static_actor";
    converted_descriptor.content_digest = fixture.page.actors.front()
        .content_digest;
    converted_descriptor.document_path = makeWorldActorDocumentPath(
        converted_descriptor.id,
        converted_descriptor.content_digest);
    converted_descriptor.space = instance_page.space;
    converted_descriptor.position = instance.position;
    WorldActorDocument converted_document;
    converted_document.world = fixture.root.world;
    converted_document.actor = converted_descriptor.id;
    auto to_actor = convertInstanceToActor(
        fixture.root,
        instance_page,
        fixture.page,
        instance.id,
        converted_descriptor,
        converted_document);
    assert(to_actor);
    assert(to_actor->instance_page.instances.empty());
    assert(to_actor->instance_page.tombstones.size() == 2u);
    assert(to_actor->actor_descriptor_page.actors.size() == 3u);

    EditableWorldInstance converted_instance;
    converted_instance.mesh = instance.mesh;
    constexpr std::array<std::string_view, 0u> no_components{};
    auto to_instance = convertActorToInstance(
        fixture.root,
        to_actor->actor_descriptor_page,
        to_actor->actor_document,
        to_actor->instance_page,
        converted_instance,
        no_components);
    assert(to_instance);
    assert(to_instance->instance.id.local_id == 4u);
    assert(to_instance->instance_set.next_local_id == 5u);
    const auto component_rejection = validateInstanceComponentAddition(
        "org.example.ScriptComponent");
    assert(!component_rejection
        && component_rejection.error().can_convert_to_actor);

    auto duplicated = duplicateWorldInstance(
        fixture.root,
        instance_page,
        instance.id,
        lux::spatial::Position3D{20.0, 2.0, 20.0});
    assert(duplicated);
    assert(duplicated->created_instance.id.local_id == 4u);
    assert(duplicated->page.instances.size() == 2u);
    assert(duplicated->instance_set.next_local_id == 5u);
    auto deleted = deleteWorldInstance(
        duplicated->page,
        duplicated->created_instance.id);
    assert(deleted && deleted->page.instances.size() == 1u);
    assert(std::ranges::count(
        deleted->page.tombstones,
        duplicated->created_instance.id.local_id) == 1);

    auto moved_in_page = moveWorldInstance(
        fixture.root,
        instance_page,
        instance_page,
        instance.id,
        lux::spatial::Position3D{25.0, 2.0, 25.0});
    assert(moved_in_page && !moved_in_page->crossedCell());
    assert(std::get<lux::spatial::Position3D>(
        moved_in_page->moved_instance.position).x == 25.0);

    auto destination_page = instance_page;
    destination_page.cell.coordinate = lux::authoring::PlanarCellCoord{1, 0};
    destination_page.instances.clear();
    destination_page.tombstones = {instance.id.local_id};
    auto moved_across_cell = moveWorldInstance(
        fixture.root,
        instance_page,
        destination_page,
        instance.id,
        lux::spatial::Position3D{130.0, 2.0, 20.0});
    assert(moved_across_cell && moved_across_cell->crossedCell());
    assert(moved_across_cell->source_page.instances.empty());
    assert(moved_across_cell->destination_page->instances.size() == 1u);
    assert(moved_across_cell->destination_page->tombstones.empty());
    auto advanced_root = fixture.root;
    advanced_root.instance_sets.front() = duplicated->instance_set;
    auto duplicated_after_move = duplicateWorldInstance(
        advanced_root,
        *moved_across_cell->destination_page,
        instance.id,
        lux::spatial::Position3D{140.0, 2.0, 20.0});
    assert(duplicated_after_move);
    assert(duplicated_after_move->created_instance.id.local_id == 5u);
    assert(duplicated_after_move->instance_set.next_local_id == 6u);
    assert(!moveWorldInstance(
        fixture.root,
        instance_page,
        destination_page,
        instance.id,
        lux::spatial::Position3D{20.0, 2.0, 20.0}));

    constexpr std::size_t terrain_samples =
        static_cast<std::size_t>(kWorldTerrainSampleEdge)
        * kWorldTerrainSampleEdge;
    WorldTerrainPageDocument terrain;
    terrain.world = fixture.root.world;
    terrain.terrain_set = lux::authoring::TerrainSetId{
        uuid("88000000-0000-4000-8000-000000000001")};
    terrain.space = fixture.root.spaces.front().id;
    terrain.cell = {
        lux::authoring::EPartitionTopology::PLANAR_XZ,
        lux::authoring::PlanarCellCoord{0, 0}};
    terrain.height_min = -512.0f;
    terrain.height_max = 2048.0f;
    terrain.heights.assign(terrain_samples, 12.5f);
    terrain.weight_layer_count = 3u;
    terrain.weight_planes[0].assign(terrain_samples * 4u, 0u);
    terrain.weight_planes[1].assign(terrain_samples * 4u, 0u);
    terrain.holes.assign((terrain_samples + 7u) / 8u, 0u);
    const auto encoded_terrain = encodeWorldTerrainPage(
        fixture.root, terrain);
    assert(encoded_terrain);
    assert(decodeWorldTerrainPage(fixture.root, *encoded_terrain));

    const auto root_2d = makeWorldSourceDocument(
        lux::authoring::EPartitionTopology::PLANAR_XY);
    WorldTilePageDocument tile;
    tile.world = root_2d.world;
    tile.tilemap = lux::authoring::TilemapId{
        uuid("89000000-0000-4000-8000-000000000001")};
    tile.space = root_2d.spaces.front().id;
    tile.cell = {
        lux::authoring::EPartitionTopology::PLANAR_XY,
        lux::authoring::PlanarCellCoord{-4, 7}};
    tile.tileset = uuid("8a000000-0000-4000-8000-000000000001");
    tile.tileset_columns = 16u;
    tile.tileset_rows = 8u;
    tile.tile_ordinals.assign(
        static_cast<std::size_t>(kWorldLogicalChunkEdge)
            * kWorldLogicalChunkEdge,
        0xffffffffu);
    tile.tile_ordinals[17] = 42u;
    tile.collision_boxes = {{2u, 3u, 4u, 5u}};
    const auto encoded_tile = encodeWorldTilePage(root_2d, tile);
    assert(encoded_tile);
    const auto decoded_tile = decodeWorldTilePage(root_2d, *encoded_tile);
    assert(decoded_tile && decoded_tile->tile_ordinals[17] == 42u &&
        decoded_tile->collision_boxes == tile.collision_boxes);

    WorldPixelPageDocument pixel;
    pixel.world = root_2d.world;
    pixel.field = lux::authoring::PixelFieldId{
        uuid("8b000000-0000-4000-8000-000000000001")};
    pixel.space = root_2d.spaces.front().id;
    pixel.cell = tile.cell;
    pixel.material_base.assign(
        static_cast<std::size_t>(kWorldLogicalChunkEdge)
            * kWorldLogicalChunkEdge,
        0u);
    pixel.material_base[33] = 9u;
    pixel.generator = WorldPixelGeneratorSource{
        lux::authoring::ChunkGeneratorId{"org.example.pixel.generator"},
        2u,
        3u,
        12345u,
        {std::byte{0x5u}}};
    const auto encoded_pixel = encodeWorldPixelPage(root_2d, pixel);
    assert(encoded_pixel);
    const auto decoded_pixel = decodeWorldPixelPage(root_2d, *encoded_pixel);
    assert(decoded_pixel && decoded_pixel->material_base[33] == 9u);
    assert(decoded_pixel->generator
        && decoded_pixel->generator->id == pixel.generator->id);
    auto generated_pixel = pixel;
    generated_pixel.material_base.clear();
    const auto encoded_generated = encodeWorldPixelPage(
        root_2d, generated_pixel);
    assert(encoded_generated);
    const auto decoded_generated = decodeWorldPixelPage(
        root_2d, *encoded_generated);
    assert(decoded_generated && decoded_generated->material_base.empty());

    std::error_code ignored;
    std::filesystem::remove_all(directory, ignored);
    std::cout << "world source codec tests passed\n";
    return 0;
}
