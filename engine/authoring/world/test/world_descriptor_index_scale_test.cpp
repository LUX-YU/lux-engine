#include <lux/engine/authoring/world/WorldDescriptorIndex.hpp>
#include <lux/engine/authoring/world/WorldSourceCodec.hpp>

#include <uuid.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <ranges>
#include <string>

namespace
{
    constexpr std::uint64_t kActorCount = 1'000'000u;
    constexpr std::uint32_t kActorsPerPage = 1024u;

    uuids::uuid actorUuid(std::uint64_t ordinal)
    {
        const auto bucket = static_cast<std::uint16_t>(ordinal & 0x0fffu);
        std::array<std::uint8_t, 16u> bytes{};
        bytes[0] = static_cast<std::uint8_t>(bucket >> 4u);
        bytes[1] = static_cast<std::uint8_t>((bucket & 0x0fu) << 4u);
        bytes[6] = 0x40u;
        bytes[8] = static_cast<std::uint8_t>(
            0x80u | ((ordinal >> 56u) & 0x3fu));
        for (std::size_t index = 0u; index < 7u; ++index)
        {
            bytes[15u - index] = static_cast<std::uint8_t>(
                ordinal >> (index * 8u));
        }
        return uuids::uuid{bytes};
    }

    struct TempTree final
    {
        std::filesystem::path root;

        ~TempTree()
        {
            std::error_code error;
            std::filesystem::remove_all(root, error);
        }
    };
}

int main()
{
    using namespace lux::authoring;
    using namespace lux::authoring;

    const auto nonce = std::chrono::steady_clock::now()
        .time_since_epoch().count();
    TempTree temporary{
        std::filesystem::temp_directory_path() /
            ("lux-world-descriptor-scale-" + std::to_string(nonce))};
    const auto world_file = temporary.root / "Worlds" / "Scale.luxworld";
    const auto cache_file = worldDescriptorIndexCachePath(
        temporary.root / "Cache",
        lux::authoring::WorldId{uuids::uuid::from_string(
            "a1000000-0000-4000-8000-000000000001").value()});

    WorldSourceDocument source;
    source.world = lux::authoring::WorldId{uuids::uuid::from_string(
        "a1000000-0000-4000-8000-000000000001").value()};
    PartitionSpaceDescriptor space;
    space.id = PartitionSpaceId{uuids::uuid::from_string(
        "a1000000-0000-4000-8000-000000000002").value()};
    space.topology = EPartitionTopology::PLANAR_XZ;
    space.cell_edge = 128.0f;
    space.macro_edge_cells = 32u;
    source.spaces.push_back(space);

    const auto page_count = static_cast<std::uint32_t>(
        (kActorCount + kActorsPerPage - 1u) / kActorsPerPage);
    const std::uint64_t needle_ordinal = kActorCount / 2u;
    const auto needle_actor = lux::authoring::WorldActorId{
        actorUuid(needle_ordinal)};
    for (std::uint32_t page_index = 0u;
         page_index < page_count; ++page_index)
    {
        const PlanarMacroCoord macro{
            static_cast<std::int64_t>(page_index % 32u),
            static_cast<std::int64_t>(page_index / 32u)};
        WorldDescriptorPageDocument page;
        page.world = source.world;
        page.space = space.id;
        page.macro = {EPartitionTopology::PLANAR_XZ, macro};
        page.id = makeWorldDescriptorPageId(
            source.world, page.space, page.macro);
        const auto first = static_cast<std::uint64_t>(page_index) *
            kActorsPerPage;
        const auto count = static_cast<std::uint32_t>(std::min<std::uint64_t>(
            kActorsPerPage, kActorCount - first));
        page.actors.reserve(count);
        for (std::uint32_t local = 0u; local < count; ++local)
        {
            const auto ordinal = first + local;
            WorldActorSourceDescriptor actor;
            actor.id = lux::authoring::WorldActorId{
                actorUuid(ordinal)};
            actor.space = space.id;
            const auto cell_x = static_cast<std::int64_t>(local % 32u);
            const auto cell_z = static_cast<std::int64_t>(local / 32u);
            actor.position = lux::math::Position3d{
                static_cast<double>(macro.a * 32 + cell_x) *
                        space.cell_edge + 1.0,
                0.0,
                static_cast<double>(macro.b * 32 + cell_z) *
                        space.cell_edge + 1.0};
            actor.bounds_half_extent = {0.5f, 0.5f, 0.5f};
            actor.display_name = ordinal == needle_ordinal
                ? "Needle Million"
                : "Actor";
            actor.actor_class = "x";
            const std::array<std::byte, sizeof(ordinal)> content =
                std::bit_cast<
                    std::array<std::byte, sizeof(ordinal)>>(ordinal);
            actor.content_digest = lux::cxx::algorithm::Sha256::hash(content);
            actor.document_path = makeWorldActorDocumentPath(
                actor.id, actor.content_digest);
            page.actors.push_back(std::move(actor));
        }
        const auto bytes = encodeWorldDescriptorPage(source, page);
        assert(bytes);
        const auto digest = lux::cxx::algorithm::Sha256::hash(*bytes);
        const auto path = makeWorldDescriptorPagePath(page.id, digest);
        assert(saveWorldSourceDocument(world_file, path, *bytes));
        source.descriptor_pages.push_back({
            page.id,
            page.space,
            page.macro,
            path,
            digest,
            count,
            0u});
    }
    assert(saveWorldSource(world_file, source));

    const auto build_begin = std::chrono::steady_clock::now();
    auto index = WorldDescriptorIndex::rebuild(
        world_file, source, cache_file);
    const auto build_end = std::chrono::steady_clock::now();
    assert(index && index->actorCount() == kActorCount);

    auto loaded = WorldDescriptorIndex::load(cache_file, source);
    assert(loaded);
    const auto cold = loaded->stats();
    assert(cold.actor_count == kActorCount &&
        cold.page_count == page_count && cold.cached_bytes == 0u);
    assert(loaded->find(
        lux::authoring::WorldActorId{actorUuid(0u)}));
    assert(loaded->find(needle_actor));
    assert(loaded->find(lux::authoring::WorldActorId{
        actorUuid(kActorCount - 1u)}));
    const auto matches = loaded->search("needle million", 0u, 4u);
    assert(matches.size() == 1u && matches.front().actor == needle_actor);
    const auto warm = loaded->stats();
    assert(warm.cached_actor_objects <= 3u &&
        warm.cached_search_objects == 1u &&
        warm.cached_bytes <= warm.cache_budget_bytes);

    const auto changed_page_index = static_cast<std::uint32_t>(
        needle_ordinal / kActorsPerPage);
    auto changed_page = loadWorldDescriptorPage(
        world_file, source, source.descriptor_pages[changed_page_index]);
    assert(changed_page);
    const auto changed_actor = std::ranges::find(
        changed_page->actors,
        needle_actor,
        &WorldActorSourceDescriptor::id);
    assert(changed_actor != changed_page->actors.end());
    changed_actor->display_name = "Needle Updated";
    const auto changed_bytes = encodeWorldDescriptorPage(
        source, *changed_page);
    assert(changed_bytes);
    const auto changed_digest = lux::cxx::algorithm::Sha256::hash(*changed_bytes);
    auto& changed_reference = source.descriptor_pages[changed_page_index];
    changed_reference.document_path = makeWorldDescriptorPagePath(
        changed_page->id, changed_digest);
    changed_reference.content_digest = changed_digest;
    assert(saveWorldSourceDocument(
        world_file, changed_reference.document_path, *changed_bytes));
    const std::array changed_pages{*changed_page};
    const auto update_begin = std::chrono::steady_clock::now();
    assert(loaded->updatePages(source, changed_pages));
    const auto update_end = std::chrono::steady_clock::now();
    assert(saveWorldSource(world_file, source));
    assert(loaded->search("needle million", 0u, 1u).empty());
    const auto updated = loaded->search("needle updated", 0u, 1u);
    assert(updated.size() == 1u && updated.front().actor == needle_actor);
    assert(loaded->stats().cached_bytes <=
        loaded->stats().cache_budget_bytes);

    const auto build_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(build_end - build_begin).count();
    const auto update_ms = std::chrono::duration_cast<
        std::chrono::milliseconds>(update_end - update_begin).count();
    std::cout << "descriptor_scale actors=" << kActorCount
              << " pages=" << page_count
              << " build_ms=" << build_ms
              << " update_ms=" << update_ms
              << " cache_bytes=" << loaded->stats().cached_bytes
              << " cache_budget=" << loaded->stats().cache_budget_bytes
              << '\n';
    return 0;
}
