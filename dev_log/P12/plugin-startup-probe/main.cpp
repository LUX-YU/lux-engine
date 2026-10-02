#include <lux/engine/editor/storage/ProjectPlugins.hpp>
#include <iostream>
int main(int argc, char** argv) {
 const lux::editor::ProjectPluginEntry plugins[]={{"lux.builtin.physics2d",1},{"lux.builtin.render",1},{"lux.builtin.runtime",2},{"lux.builtin.scene_render",1}};
 auto result=lux::editor::loadProjectPlugins(std::filesystem::current_path(), plugins, argv[1]);
 if(!result) { const auto& e=result.error(); std::cout << static_cast<int>(e.code) << " | " << e.plugin << " | " << e.subject << " | " << e.detail << '\n'; return 1; }
 std::cout << "Plugins loaded\n";
}

