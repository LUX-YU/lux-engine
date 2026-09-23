// Build-time projection of the same declarations used by the runtime registration.
#include <lux/engine/physics2d/Physics2DSystem.type_static_info.hpp>
#include <lux/engine/physics2d/Physics2DComponents.type_static_info.hpp>
#include <lux/engine/physics2d/Physics2DComponents.ecs_schema.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <nlohmann/json.hpp>
#include <fstream>

using Json = nlohmann::json;
template<class T> Json fields()
{
    Json result = Json::array();
    T defaults;
    std::apply([&](const auto &...field) {
        const auto append = [&](const auto &f) {
            using Value = std::remove_cvref_t<decltype(defaults.*f.pointer)>;
            Json value{{"id", f.name}, {"type", lux::cxx::type_name<Value>()}, {"display_name", f.name}};
            if constexpr (std::is_arithmetic_v<Value>) value["default_display"] = defaults.*f.pointer;
            result.push_back(std::move(value));
        };
        (append(field), ...);
    }, lux::meta::TypeStaticInfo<T>::fields);
    return result;
}

int main(int argc, char **argv)
{
    if (argc != 3) return 2;
    using namespace lux;
    std::ifstream source(argv[1]);
    Json value = Json::parse(source);
    for (const auto *key : {"abilities", "implementations", "systems", "components", "configurations",
                           "render_features", "render_scene_bindings"}) value[key] = Json::array();
    const auto &type = physics2d::Physics2DSystem::Description.type;
    for (const auto capability : type.capabilities)
    {
        value["abilities"].push_back({{"id", capability}, {"version", 1}, {"display_name", capability}});
        value["implementations"].push_back({{"ability", {{"id", capability}, {"version", 1}}},
                                            {"system", type.canonical_name}});
    }
    Json system{{"id", type.canonical_name}, {"version", type.version}, {"domain", "simulation"},
        {"requirements", Json::array()}, {"configuration", {{"id", type.configuration_schema_name},
            {"version", type.configuration_schema_version}}},
        {"access", {{"components", Json::array()}, {"external", Json::array()}}}};
    const auto components = simulation::ecs::generated::physics2dComponentSchemas();
    const auto transforms = simulation::ecs::transformComponentSchemas();
    const auto componentName = [&](cxx::TypeToken token) -> std::string_view {
        for (const auto range : {components, transforms})
            for (const auto &entry : range) if (entry.cpp_type == token) return entry.id.name;
        return {};
    };
    for (const auto &access : physics2d::Physics2DSystem::Access.spec().components)
    {
        const auto name = componentName(access.type);
        if (name.empty()) return 3;
        system["access"]["components"].push_back({{"component", name},
            {"mode", access.mode == simulation::ESystemAccessMode::WRITE ? "write" : "read"}});
    }
    value["systems"].push_back(std::move(system));
    value["configurations"].push_back({{"id", type.configuration_schema_name},
        {"version", type.configuration_schema_version}, {"display_name", "Physics 2D Configuration"},
        {"fields", fields<physics2d::Physics2DSystemConfiguration>()}});
    for (const auto &entry : components)
    {
        auto members = entry.cpp_type == cxx::typeToken<physics2d::BoxCollider2D>() ?
            fields<physics2d::BoxCollider2D>() : fields<physics2d::RigidBody2D>();
        value["components"].push_back({{"id", entry.id.name}, {"version", entry.version},
            {"display_name", entry.id.name}, {"storage", "authoring"}, {"fields", std::move(members)}});
    }
    const auto output = value.dump(2) + "\n";
    std::ifstream previous(argv[2], std::ios::binary);
    if (std::string(std::istreambuf_iterator<char>(previous), {}) == output) return 0;
    std::ofstream file(argv[2], std::ios::binary | std::ios::trunc);
    file << output;
    return file.good() ? 0 : 4;
}
