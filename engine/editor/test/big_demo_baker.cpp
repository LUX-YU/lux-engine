/// big_demo_baker — one-shot generator for the large multi-light demo scene.
///
/// Drains the meta-module init queue (so ComponentTypeRegistry +
/// ReflectionRegistry are populated → Scene::save serializes components) and
/// writes a city-scale `.luxscene` via writeBigDemoScene. Run with the output
/// path as argv[1]:
///
///   big_demo_baker <project>/Scenes/BigDemo.luxscene
///
/// The referenced assets (primitives + emissive bulb palette) are EditorBuiltins
/// registered by the editor at startup, so the generated scene resolves on load
/// without any extra cooking.

#include <lux/engine/editor/scene/DemoSceneTemplate.hpp>
#include <lux/engine/meta/Meta.hpp>           // meta_module_init / deinit

#include <cstdio>
#include <filesystem>

int main(int argc, char** argv)
{
    lux::meta::meta_module_init();

    const std::filesystem::path out =
        (argc > 1) ? std::filesystem::path(argv[1])
                   : std::filesystem::path("BigDemo.luxscene");

    const bool ok = lux::editor::writeBigDemoScene(out);
    std::printf("[big_demo_baker] %s -> %s\n",
                ok ? "OK" : "FAIL", out.string().c_str());

    lux::meta::meta_module_deinit();
    return ok ? 0 : 1;
}
