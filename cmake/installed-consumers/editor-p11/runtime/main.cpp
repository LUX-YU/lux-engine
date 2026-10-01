#include <lux/engine/project/PluginLibrary.hpp>
#include <lux/engine/simulation/ecs/TransformSchema.hpp>
#include <cassert>
#include <iostream>

int main(int argc, char** argv)
{
    assert(argc == 2);
    namespace project = lux::project;
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
