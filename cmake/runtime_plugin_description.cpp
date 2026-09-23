// Native build tool: serialize the same typed export tables linked into the plugin.
#include <lux/engine/scene/ScenePluginExports.hpp>
#include <lux/engine/scene/RenderScenePluginExports.hpp>
#include <lux/engine/simulation/SimulationPluginExports.hpp>
#include <lux/engine/simulation/ecs/ComponentPluginExports.hpp>
#include <lux/engine/function/render/features/BuiltinFeatures.hpp>
#include <nlohmann/json.hpp>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>

extern "C" const lux::simulation::SimulationPluginExports *lux_simulation_exports_v1() noexcept;
extern "C" const lux::scene::ScenePluginExports *lux_scene_exports_v1() noexcept;
extern "C" const lux::simulation::ecs::ComponentPluginExports *lux_component_exports_v1() noexcept;
extern "C" const lux::scene::RenderScenePluginExports *lux_render_scene_exports_v1() noexcept;

int main(int argc, char **argv)
{
    if (argc != 2) return 2;
    using Json = nlohmann::json;
    Json value{{"format", "lux.engine.plugin"}, {"version", 1},
        {"plugin", {{"id", "lux.builtin.runtime"}, {"version", 1}, {"source", "builtin"}, {"author", "Lux"},
            {"description", "Builtin Scene and Simulation implementations."},
            {"runtime_library", {{"exports", {"simulation", "scene", "components", "render_scene"}}}},
            {"dependencies", {{{"plugin", "lux.builtin.render"}, {"version", 1}}}}}}};
    for (const auto *key : {"abilities", "implementations", "systems", "components", "configurations",
                            "render_features", "render_scene_bindings"}) value[key] = Json::array();
    std::map<std::string, bool> abilities;
    const auto addAbility = [&](std::string_view name) {
        if (abilities.emplace(name, true).second)
            value["abilities"].push_back({{"id", name}, {"version", 1}, {"display_name", name}});
    };
    const auto component_table = lux_component_exports_v1();
    const auto componentName = [&](lux::cxx::TypeToken type) -> std::string {
        for (const auto &entry : std::span{component_table->entries, component_table->count})
            if (entry.cpp_type == type) return std::string(entry.id.name);
        // External access uses a stable C++ contract name, not an installed layout or offset.
        std::string name(type.name());
        for (std::size_t p{}; (p = name.find("::", p)) != std::string::npos;) name.replace(p, 2, ".");
        return name;
    };
    for (const auto &entry : std::span{component_table->entries, component_table->count})
        value["components"].push_back({{"id", entry.id.name}, {"version", entry.version},
            {"display_name", entry.id.name}, {"fields", Json::array()},
            {"storage", entry.semantic_kind == lux::simulation::ecs::EComponentSemanticKind::RUNTIME_DERIVED ?
                "runtime_derived" : "authoring"}});
    const auto systemValue = [&](const lux::system::SystemTypeDescription &type, const char *domain, lux::cxx::TypeToken configuration_type) {
        Json system{{"id", type.canonical_name}, {"version", type.version}, {"domain", domain},
                    {"requirements", Json::array()}};
        if (!type.configuration_schema_name.empty())
        {
            system["configuration"] = {{"id", type.configuration_schema_name}, {"version", type.configuration_schema_version}};
            value["configurations"].push_back({{"id", type.configuration_schema_name},
                {"version", type.configuration_schema_version}, {"display_name", type.configuration_schema_name},
                {"fields", Json::array()}, {"_cpp_type", configuration_type.name()}});
        }
        for (const auto capability : type.capabilities)
        {
            addAbility(capability);
            value["implementations"].push_back({{"ability", {{"id", capability}, {"version", 1}}},
                                                {"system", type.canonical_name}});
        }
        return system;
    };
    const auto simulation = lux_simulation_exports_v1();
    for (const auto &entry : std::span{simulation->entries, simulation->count})
    {
        auto system = systemValue(entry.description->type, "simulation", entry.configuration.type);
        system["access"] = {{"components", Json::array()}, {"external", Json::array()}};
        for (const auto &access : entry.access.components)
            system["access"]["components"].push_back({{"component", componentName(access.type)},
                {"mode", access.mode == lux::simulation::ESystemAccessMode::WRITE ? "write" : "read"}});
        for (const auto &access : entry.access.external)
            system["access"]["external"].push_back({{"resource", componentName(access.type)},
                {"mode", access.mode == lux::simulation::ESystemAccessMode::WRITE ? "write" : "read"}});
        value["systems"].push_back(std::move(system));
    }
    const auto scene = lux_scene_exports_v1();
    for (const auto &entry : std::span{scene->entries, scene->count})
    {
        auto system = systemValue(*entry.description, "scene", entry.configuration.type);
        for (const auto &requirement : entry.requirements)
        {
            addAbility(requirement.capability);
            system["requirements"].push_back({{"slot", requirement.name},
                {"ability", {{"id", requirement.capability}, {"version", 1}}}, {"optional", requirement.optional}});
        }
        value["systems"].push_back(std::move(system));
    }
    const auto bindings = lux_render_scene_exports_v1();
    const auto features = lux::render::builtinRenderFeatureRegistrations();
    for (const auto &entry : std::span{bindings->entries, bindings->count})
    {
        const auto feature = std::ranges::find_if(features, [&](const auto &f) { return f.factory.descriptor.type == entry.feature; });
        if (feature == features.end()) return 3;
        Json binding{{"feature", feature->factory.descriptor.canonical_name}, {"scene_system", entry.scene_system.name},
                     {"observations", Json::array()}};
        for (const auto &observation : entry.observations)
        {
            Json events = Json::array();
            if (observation.events & 1) events.push_back("construct");
            if (observation.events & 2) events.push_back("update");
            if (observation.events & 4) events.push_back("destroy");
            binding["observations"].push_back({{"component", componentName(observation.component)}, {"events", events}});
        }
        value["render_scene_bindings"].push_back(std::move(binding));
    }
    const std::string output = value.dump(2) + "\n";
    std::ifstream previous(argv[1], std::ios::binary);
    if (std::string(std::istreambuf_iterator<char>(previous), {}) == output) return 0;
    std::ofstream file(argv[1], std::ios::binary | std::ios::trunc);
    file << output;
    return file.good() ? 0 : 4;
}
