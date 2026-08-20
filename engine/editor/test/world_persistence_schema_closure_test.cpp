#include "scene/WorldPersistenceSchemaClosure.hpp"

#include <lux/engine/authoring/world/WorldSourceCodec.hpp>
#include <lux/cxx/algorithm/Sha256.hpp>

#include <array>
#include <cstdio>
#include <filesystem>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace
{
    int failures = 0;

    void check(bool condition, const char* label)
    {
        std::printf("  [%s] %s\n", condition ? "OK" : "FAIL", label);
        if (!condition)
            ++failures;
    }

    [[nodiscard]] uuids::uuid uuid(const char* value)
    {
        return uuids::uuid::from_string(value).value();
    }

    [[nodiscard]] lux::cxx::expected<
        lux::authoring::WorldActorSourceDescriptor,
        std::string>
    makeActor(
        const std::filesystem::path& root_document,
        const lux::authoring::WorldSourceDocument& source,
        uuids::uuid id,
        std::string schema,
        lux::spatial::Position3D position,
        bool save)
    {
        lux::authoring::WorldActorDocument document;
        document.world = source.world;
        document.actor = lux::authoring::WorldActorId{id};
        document.actor_class = "org.lux.test.actor";
        document.space = source.spaces.front().id;
        document.position = position;
        document.name_table = {
            std::byte{1u}, std::byte{0u},
            std::byte{0u}, std::byte{0u}};
        document.components.push_back({std::move(schema), 1u, {}});

        auto encoded = lux::authoring::encodeWorldActorDocument(document);
        if (!encoded)
            return lux::cxx::unexpected(encoded.error());
        const auto digest = lux::cxx::algorithm::Sha256::hash(*encoded);

        lux::authoring::WorldActorSourceDescriptor descriptor;
        descriptor.id = document.actor;
        descriptor.display_name = "Actor";
        descriptor.actor_class = document.actor_class;
        descriptor.content_digest = digest;
        descriptor.document_path = lux::authoring::makeWorldActorDocumentPath(
            descriptor.id,
            digest);
        descriptor.space = document.space;
        descriptor.position = document.position;
        if (save)
        {
            auto saved = lux::authoring::saveWorldSourceDocument(
                root_document,
                descriptor.document_path,
                *encoded);
            if (!saved)
                return lux::cxx::unexpected(saved.error());
        }
        return descriptor;
    }

    [[nodiscard]] lux::cxx::expected<void, std::string> addPage(
        const std::filesystem::path& root_document,
        lux::authoring::WorldSourceDocument& source,
        const lux::authoring::WorldDescriptorPageDocument& page,
        bool save)
    {
        auto encoded = lux::authoring::encodeWorldDescriptorPage(source, page);
        if (!encoded)
            return lux::cxx::unexpected(encoded.error());
        const auto digest = lux::cxx::algorithm::Sha256::hash(*encoded);
        const auto path = lux::authoring::makeWorldDescriptorPagePath(
            page.id,
            digest);
        source.descriptor_pages.push_back({
            page.id,
            page.space,
            page.macro,
            path,
            digest,
            static_cast<std::uint32_t>(page.actors.size()),
            static_cast<std::uint32_t>(page.pages.size())});
        if (!save)
            return {};
        return lux::authoring::saveWorldSourceDocument(
            root_document,
            path,
            *encoded);
    }
}

int main()
{
    namespace fs = std::filesystem;
    auto source = lux::authoring::makeWorldSourceDocument(
        lux::authoring::EPartitionTopology::PLANAR_XZ);
    const auto root_document = fs::temp_directory_path() /
        "lux-world-persistence-schema-closure" / "World.luxworld";
    std::error_code error;
    fs::remove_all(root_document.parent_path(), error);

    auto live = makeActor(
        root_document,
        source,
        uuid("71000000-0000-4000-8000-000000000001"),
        "org.lux.test.live",
        {},
        false);
    auto preserved = makeActor(
        root_document,
        source,
        uuid("72000000-0000-4000-8000-000000000001"),
        "org.lux.test.preserved",
        {},
        true);
    auto untouched = makeActor(
        root_document,
        source,
        uuid("73000000-0000-4000-8000-000000000001"),
        "org.lux.test.untouched",
        {4096.0, 0.0, 0.0},
        true);
    check(live && preserved && untouched, "Actor fixtures encode");
    if (!live || !preserved || !untouched)
        return 1;

    lux::authoring::WorldDescriptorPageDocument rewritten_page;
    rewritten_page.world = source.world;
    rewritten_page.space = source.spaces.front().id;
    rewritten_page.macro = {
        lux::authoring::EPartitionTopology::PLANAR_XZ,
        lux::authoring::PlanarMacroCoord{0, 0}};
    rewritten_page.id = lux::authoring::makeWorldDescriptorPageId(
        source.world,
        rewritten_page.space,
        rewritten_page.macro);
    rewritten_page.actors = {*live, *preserved};

    lux::authoring::WorldDescriptorPageDocument untouched_page;
    untouched_page.world = source.world;
    untouched_page.space = source.spaces.front().id;
    untouched_page.macro = {
        lux::authoring::EPartitionTopology::PLANAR_XZ,
        lux::authoring::PlanarMacroCoord{1, 0}};
    untouched_page.id = lux::authoring::makeWorldDescriptorPageId(
        source.world,
        untouched_page.space,
        untouched_page.macro);
    untouched_page.actors = {*untouched};

    const auto rewritten_added = addPage(
        root_document,
        source,
        rewritten_page,
        false);
    const auto untouched_added = addPage(
        root_document,
        source,
        untouched_page,
        true);
    check(
        rewritten_added && untouched_added,
        "rewritten and retained Descriptor Page fixtures encode");
    if (!rewritten_added || !untouched_added)
        return 1;

    std::vector<lux::editor::detail::WorldActorComponentSchemaSnapshot>
        rewritten_actors{
            {live->id, {"org.lux.test.live"}},
            {lux::authoring::WorldActorId{
                 uuid("74000000-0000-4000-8000-000000000001")},
             {"org.lux.test.transient"}}};
    const std::array rewritten_pages{rewritten_page};
    const auto schemas =
        lux::editor::detail::collectFinalWorldComponentSchemas(
            root_document,
            source,
            rewritten_pages,
            rewritten_actors);
    check(
        schemas && *schemas == std::vector<std::string>{
            "org.lux.test.live",
            "org.lux.test.preserved",
            "org.lux.test.untouched"},
        "final closure keeps unloaded Actor providers and ignores unreferenced transient snapshots");

    fs::remove_all(root_document.parent_path(), error);
    return failures == 0 ? 0 : 1;
}
