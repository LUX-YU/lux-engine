#include <cassert>
#include <cstdio>
#include <fstream>
#include <lux/cxx/algorithm/Sha256.hpp>
#include <lux/engine/editor/ProjectManifest.hpp>
#include <lux/engine/project/PluginCatalog.hpp>
#include <nlohmann/json.hpp>

int main(int argc, char** argv)
{
    assert(argc == 2);
    using Json = nlohmann::json;
    const auto directory = std::filesystem::absolute(argv[1]);
    std::filesystem::create_directories(directory);
    const auto file = directory / "external.json";
    uuids::uuid_name_generator ids{*uuids::uuid::from_string("629c02e8-504e-4ebe-b633-ce8fc2269954")};
    for (const auto* name : {"Vendor_Plugin-1", "Vendor.Render-Plugin", "_my_plugin", "1.plugin"})
    {
        Json module{{"id", name}, {"version", 1}, {"dependencies", Json::array()}};
        Json digest_input{{"module", module}};
        Json description{{"format", "lux.engine.plugin"}, {"version", 2}};
        for (const auto* field :
             {"abilities",
              "implementations",
              "systems",
              "components",
              "configurations",
              "render_features",
              "render_scene_bindings"})
        {
            description[field] = digest_input[field] = Json::array();
        }
        const auto bytes = digest_input.dump();
        std::array<char, 64> hex;
        lux::cxx::algorithm::Sha256::hash(std::as_bytes(std::span{bytes})).formatHex(hex);
        const std::string digest(hex.data(), hex.size());
        module["author"] = "Regression";
        module["description"] = "Identity parity through actual PluginCatalog";
        module["source"] = "plugin";
        module["runtime_library"] = {
            {"path", "external.dll"},
            {"interface_version", 1},
            {"sdk_abi", "test"},
            {"build_id", std::string(64, '0')},
            {"declaration_digest", digest},
            {"exports", {"components"}}
        };
        description["plugin"] = std::move(module);
        {
            std::ofstream output(file, std::ios::binary);
            output << description.dump();
        }
        lux::project::PluginCatalog catalog;
        auto accepted = catalog.read(file, directory);
        const bool valid = name[0] != '1';
        assert(bool(accepted) == valid);
        lux::editor::ProjectManifest project{1, ids("project"), "Project"};
        project.plugins = {{name, 1}};
        auto selected = lux::editor::validateProjectManifest(project);
        std::printf("Plugin %s: catalog=%d manifest=%d expected=%d\n", name, bool(accepted), bool(selected), valid);
        std::fflush(stdout);
        assert(bool(selected) == valid);
        if (valid)
        {
            assert(catalog.find(name));
            auto encoded = lux::editor::encodeProjectManifest(project);
            assert(encoded && lux::editor::decodeProjectManifest(*encoded)->plugins == project.plugins);
        }
        else
        {
            assert(accepted.error().code == lux::project::EPluginError::INVALID_DESCRIPTION);
            assert(selected.error().code == lux::editor::EProjectError::INVALID_PLUGIN);
        }
    }
}
