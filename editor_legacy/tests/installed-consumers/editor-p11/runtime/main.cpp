#include <lux/engine/project/PluginLibrary.hpp>
#include <lux/engine/project/PluginManager.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <cassert>
#include <iostream>

int main(int argc, char** argv)
{
    namespace project = lux::project;
    if (argc == 3)
    {
        assert(std::string_view(argv[1]) == "--builtins");
        const std::filesystem::path installation{argv[2]};
        project::PluginCatalog catalog;
        assert(catalog.read(installation / "share/lux-engine/plugins/catalog.json", installation));
        std::vector<project::MetadataIdentity> selected;
        for (const auto& plugin : catalog.plugins())
            if (plugin.builtin)
                selected.push_back(plugin.identity);
        assert(!selected.empty());
        auto plugins = project::PluginManager::create(std::move(catalog), selected);
        if (!plugins)
        {
            const auto& failure = plugins.error();
            std::cerr << static_cast<unsigned>(failure.code) << ' ' << failure.plugin << ' ' << failure.subject << ' '
                      << failure.detail << '\n';
            return 1;
        }
        for (const auto& identity : selected)
            assert(plugins->find(identity.id));
        std::cout << "PASS installed built-in plugin closure, without statically linking/preloading the plugins\n";
        return 0;
    }
    assert(argc == 2);
    const std::filesystem::path path{argv[1]};
    project::PluginLibraryDescription library;
    library.path = path.filename();
    library.sdk_abi = project::pluginSdkAbi();
    library.build_id = "p11-installed";
    library.declaration_digest = "p11-declarations";
    library.exports = {project::EPluginExport::COMPONENTS};
    project::PluginDescription description;
    description.identity = {"qualification.p11", 1};
    description.root = path.parent_path();
    description.runtime_library = library;
    const auto& schema = lux::simulation::ecs::transformComponentSchemas().front();
    description.components.push_back({{std::string(schema.id.name), schema.version}});
    auto plugin = project::PluginLibrary::load(description);
    assert(plugin && (*plugin)->components().size() == 1);
    assert((*plugin)->components().front().id == schema.id);
    assert((*plugin)->components().front().operations.valid());
    std::cout << "PASS installed runtime plugin without Editor target/header/ABI\n";
}
