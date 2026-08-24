#pragma once

#include <lux/engine/authoring/world/WorldSourceCodec.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <algorithm>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lux::editor::detail
{
    /// Component schemas captured for one Actor document that will replace its
    /// existing content object in the current Authoring transaction.
    struct WorldActorComponentSchemaSnapshot final
    {
        lux::authoring::WorldActorId actor;
        std::vector<std::string> schemas;
    };

    /// Rebuild the schema closure from the final LXWA/LXAI Actor membership.
    /// Rewritten Actors use their just-captured schema snapshot; every other
    /// Actor is read from its retained LXAD content object. Consequently an
    /// unreferenced capture candidate cannot contaminate persistence metadata.
    [[nodiscard]] inline lux::cxx::expected<
        std::vector<std::string>,
        std::string>
    collectFinalWorldComponentSchemas(
        const std::filesystem::path& existing_root_document,
        const lux::authoring::WorldSourceDocument& final_source,
        std::span<const lux::authoring::WorldDescriptorPageDocument>
            rewritten_pages,
        std::span<const WorldActorComponentSchemaSnapshot>
            rewritten_actors)
    {
        std::unordered_map<
            std::string,
            const lux::authoring::WorldDescriptorPageDocument*> page_overrides;
        page_overrides.reserve(rewritten_pages.size());
        for (const auto& page : rewritten_pages)
        {
            if (!page_overrides.emplace(
                    uuids::to_string(page.id),
                    &page).second)
            {
                return lux::cxx::unexpected(
                    std::string{"duplicate rewritten Descriptor Page"});
            }
        }

        std::unordered_map<
            std::string,
            const WorldActorComponentSchemaSnapshot*> actor_overrides;
        actor_overrides.reserve(rewritten_actors.size());
        for (const auto& actor : rewritten_actors)
        {
            if (actor.actor.empty() ||
                !actor_overrides.emplace(
                    uuids::to_string(actor.actor.value()),
                    &actor).second)
            {
                return lux::cxx::unexpected(
                    std::string{"invalid or duplicate rewritten Actor"});
            }
        }

        std::unordered_set<std::string> schemas;
        std::unordered_set<std::string> visited_pages;
        visited_pages.reserve(final_source.descriptor_pages.size());
        for (const auto& reference : final_source.descriptor_pages)
        {
            const auto page_key = uuids::to_string(reference.id);
            if (!visited_pages.insert(page_key).second)
            {
                return lux::cxx::unexpected(
                    std::string{"duplicate final Descriptor Page reference"});
            }

            std::optional<lux::authoring::WorldDescriptorPageDocument>
                loaded_page;
            const lux::authoring::WorldDescriptorPageDocument* page = nullptr;
            if (const auto rewritten = page_overrides.find(page_key);
                rewritten != page_overrides.end())
            {
                page = rewritten->second;
            }
            else
            {
                auto loaded = lux::authoring::loadWorldDescriptorPage(
                    existing_root_document,
                    final_source,
                    reference);
                if (!loaded)
                {
                    return lux::cxx::unexpected(
                        std::string{"cannot load retained Descriptor Page '"} +
                        page_key + "': " + loaded.error());
                }
                loaded_page.emplace(std::move(*loaded));
                page = &*loaded_page;
            }

            for (const auto& descriptor : page->actors)
            {
                const auto actor_key = uuids::to_string(
                    descriptor.id.value());
                if (const auto rewritten = actor_overrides.find(actor_key);
                    rewritten != actor_overrides.end())
                {
                    for (const auto& schema : rewritten->second->schemas)
                    {
                        if (schema.empty())
                        {
                            return lux::cxx::unexpected(
                                std::string{"rewritten Actor has an empty "
                                            "component schema"});
                        }
                        schemas.insert(schema);
                    }
                    continue;
                }

                auto document = lux::authoring::loadWorldActorDocument(
                    existing_root_document,
                    descriptor);
                if (!document)
                {
                    return lux::cxx::unexpected(
                        std::string{"cannot load retained Actor '"} +
                        actor_key + "': " + document.error());
                }
                for (const auto& component : document->components)
                    schemas.insert(component.schema_name);
            }
        }

        std::vector<std::string> result(schemas.begin(), schemas.end());
        std::ranges::sort(result);
        return result;
    }
} // namespace lux::editor::detail
