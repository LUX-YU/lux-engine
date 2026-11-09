// Build-time projection of the same declarations used by the runtime registration.
#include "SystemDataJson.hpp"
#include <fstream>
#include <lux/engine/physics2d/Physics2DComponents.ecs_schema.hpp>
#include <lux/engine/physics2d/Physics2DSystem.hpp>
#include <lux/engine/ecs/TransformSchema.hpp>
#include <nlohmann/json.hpp>

using Json = nlohmann::json;

int main(int argc, char** argv)
{
    if (argc != 3)
    {
        return 2;
    }
    using namespace lux;
    std::ifstream source(argv[1]);
    Json value = Json::parse(source);
    value["version"] = 3;
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
    const auto& type = physics2d::Physics2DSystem::Description;
    for (const auto capability : type.capabilities)
    {
        value["abilities"].push_back({{"id", capability}, {"version", 1}, {"display_name", capability}});
        value["implementations"].push_back(
            {{"ability", {{"id", capability}, {"version", 1}}}, {"system", type.canonical_name}}
        );
    }
    Json system{
        {"id", type.canonical_name},
        {"version", type.version},
        {"domain", "simulation"},
        {"requirements", Json::array()},
        {"data_inputs", systemDataJson(type.data_inputs)},
        {"data_outputs", systemDataJson(type.data_outputs)},
        {"configuration", {{"id", type.configuration_schema_name}, {"version", type.configuration_schema_version}}},
        {"access", {{"components", Json::array()}, {"external", Json::array()}}}
    };
    const auto components = ecs::generated::physics2dComponentSchemas();
    const auto transforms = ecs::transformComponentSchemas();
    const auto componentName = [&](cxx::type_info token) -> std::string_view
    {
        for (const auto range : {components, transforms})
        {
            for (const auto& entry : range)
            {
                if (entry.cpp_type == token)
                {
                    return entry.id.name;
                }
            }
        }
        return {};
    };
    for (const auto& access : physics2d::Physics2DSystem::Access.spec().components)
    {
        const auto name = componentName(access.type);
        if (name.empty())
        {
            return 3;
        }
        system["access"]["components"].push_back(
            {{"component", name}, {"mode", access.mode == simulation::ESystemAccessMode::WRITE ? "write" : "read"}}
        );
    }
    value["systems"].push_back(std::move(system));
    value["configurations"].push_back(
        {{"id", type.configuration_schema_name},
         {"version", type.configuration_schema_version},
         {"display_name", "Physics 2D Configuration"},
         {"_cpp_type", cxx::type_name<physics2d::Physics2DSystemConfiguration>()},
         {"fields", Json::array()}}
    );
    for (const auto& entry : components)
    {
        value["components"].push_back(
            {{"id", entry.id.name},
             {"version", entry.version},
             {"display_name", entry.id.name},
             {"storage", "authoring"},
             {"fields", Json::array()}}
        );
    }
    const auto output = value.dump(2) + "\n";
    std::ifstream previous(argv[2], std::ios::binary);
    if (std::string(std::istreambuf_iterator<char>(previous), {}) == output)
    {
        return 0;
    }
    std::ofstream file(argv[2], std::ios::binary | std::ios::trunc);
    file << output;
    return file.good() ? 0 : 4;
}
