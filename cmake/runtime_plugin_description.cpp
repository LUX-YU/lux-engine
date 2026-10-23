#include "SystemDataJson.hpp"
// Native build tool: serialize the same typed export tables linked into the plugin.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <lux/engine/scene/ScenePluginExports.hpp>
#include <lux/engine/simulation/ecs/ComponentPluginExports.hpp>
#include <map>
#include <nlohmann/json.hpp>
#include <string>

extern "C" const lux::scene::ScenePluginExports* lux_scene_exports_v1() noexcept;
extern "C" const lux::simulation::ecs::ComponentPluginExports* lux_component_exports_v1() noexcept;

int main(int argc, char** argv)
{
    if (argc != 2 && argc != 4 && argc != 6)
    {
        return 2;
    }
    using Json = nlohmann::json;
    Json value{{"format", "lux.engine.plugin"}, {"version", 3}};
    value["plugin"] = {
        {"id", argc >= 4 ? argv[2] : "lux.builtin.runtime"},
        {"version", 2},
        {"source", "builtin"},
        {"author", "Lux"},
        {"description", argc >= 4 ? argv[3] : "Transform, World loading and CPU spatial query implementations."},
        {"runtime_library", {{"exports", {"scene", "components"}}}},
        {"dependencies", Json::array()}
    };
    if (argc == 6)
    {
        value["plugin"]["dependencies"].push_back({{"plugin", argv[4]}, {"version", std::stoul(argv[5])}});
    }
    for (const auto* key :
         {"abilities",
          "implementations",
          "systems",
          "components",
          "configurations",
          "render_features",
          "render_scene_bindings"})
    {
        value[key] = Json::array();
    }
    std::map<std::string, bool> abilities;
    const auto addAbility = [&](std::string_view name)
    {
        if (abilities.emplace(name, true).second)
        {
            value["abilities"].push_back({{"id", name}, {"version", 1}, {"display_name", name}});
        }
    };
    const auto component_table = lux_component_exports_v1();
    const auto componentName = [&](lux::cxx::TypeToken type) -> std::string
    {
        for (const auto& entry : std::span{component_table->entries, component_table->count})
        {
            if (entry.cpp_type == type)
            {
                return std::string(entry.id.name);
            }
        }
        // External access uses a stable C++ contract name, not an installed layout or offset.
        std::string name(type.name());
        for (std::size_t p{}; (p = name.find("::", p)) != std::string::npos;)
        {
            name.replace(p, 2, ".");
        }
        return name;
    };
    for (const auto& entry : std::span{component_table->entries, component_table->count})
    {
        value["components"].push_back(
            {{"id", entry.id.name},
             {"version", entry.version},
             {"display_name", entry.id.name},
             {"fields", Json::array()},
             {"storage",
              entry.semantic_kind == lux::simulation::ecs::EComponentSemanticKind::RUNTIME_DERIVED ? "runtime_derived"
                                                                                                   : "authoring"}}
        );
    }
    const auto systemValue =
        [&](const lux::system::SystemTypeDescription& type, const char* domain, lux::cxx::TypeToken configuration_type)
    {
        Json system{
            {"id", type.canonical_name},
            {"version", type.version},
            {"domain", domain},
            {"requirements", Json::array()},
            {"data_inputs", systemDataJson(type.data_inputs)},
            {"data_outputs", systemDataJson(type.data_outputs)}
        };
        if (!type.configuration_schema_name.empty())
        {
            system["configuration"] = {
                {"id", type.configuration_schema_name},
                {"version", type.configuration_schema_version}
            };
            value["configurations"].push_back(
                {{"id", type.configuration_schema_name},
                 {"version", type.configuration_schema_version},
                 {"display_name", type.configuration_schema_name},
                 {"fields", Json::array()},
                 {"_cpp_type", configuration_type.name()}}
            );
        }
        for (const auto capability : type.capabilities)
        {
            addAbility(capability);
            value["implementations"].push_back(
                {{"ability", {{"id", capability}, {"version", 1}}}, {"system", type.canonical_name}}
            );
        }
        return system;
    };

    const auto scene = lux_scene_exports_v1();
    for (const auto& entry : std::span{scene->entries, scene->count})
    {
        auto system = systemValue(*entry.description, "scene", entry.configuration.type);
        for (const auto& requirement : entry.requirements)
        {
            addAbility(requirement.capability);
            system["requirements"].push_back(
                {{"slot", requirement.name},
                 {"ability", {{"id", requirement.capability}, {"version", 1}}},
                 {"optional", requirement.optional}}
            );
        }
        value["systems"].push_back(std::move(system));
    }
    const std::string output = value.dump(2) + "\n";
    std::ifstream previous(argv[1], std::ios::binary);
    if (std::string(std::istreambuf_iterator<char>(previous), {}) == output)
    {
        return 0;
    }
    std::ofstream file(argv[1], std::ios::binary | std::ios::trunc);
    file << output;
    return file.good() ? 0 : 4;
}
