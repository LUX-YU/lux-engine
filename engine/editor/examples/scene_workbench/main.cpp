#include "DevelopmentScene.hpp"
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <thread>
namespace
{
    void report(const lux::editor::application::ApplicationFailure &error)
    {
        std::fprintf(stderr, "ER1 failure application=%u\n", unsigned(error.code));
        if (error.window)
            std::fprintf(stderr, "window=%u\n", unsigned(error.window->code));
        if (error.scene)
        {
            std::fprintf(stderr, "scene=%u session=%llu\n", unsigned(error.scene->code), error.scene->session.value);
            if (error.scene->simulation)
                std::fprintf(stderr, "simulation=%u\n", unsigned(error.scene->simulation->code));
        }
        if (error.renderer)
            std::fprintf(stderr, "renderer=%u\n", unsigned(error.renderer->code));
        if (error.execution)
            std::fprintf(stderr, "execution=%u\n", unsigned(*error.execution));
        if (error.assets)
            std::fprintf(stderr, "asset_endpoint=%u\n", unsigned(*error.assets));
    }
} // namespace
int main(int argc, char **argv)
{
    using namespace lux::editor;
    std::size_t frames{};
    bool alternate{}, validation{}, hidden{};
    std::optional<ui::WindowFontSpec> font;
    auto asset_path =
        std::filesystem::absolute(argv[0]).parent_path().parent_path() / "share/lux-engine/editor/sv1.luxpak";
    for (int i = 1; i < argc; ++i)
    {
        const std::string_view arg{argv[i]};
        if (arg == "--validation")
            validation = true;
        else if (arg == "--hidden")
            hidden = true;
        else if (arg == "--alternate")
            alternate = true;
        else if (arg == "--assets" && i + 1 < argc)
            asset_path = argv[++i];
        else if (arg == "--font" && i + 1 < argc)
        {
            font.emplace();
            font->file = argv[++i];
        }
        else if (arg == "--frames" && i + 1 < argc)
        {
            const std::string_view text{argv[++i]};
            const auto parsed = std::from_chars(text.data(), text.data() + text.size(), frames);
            if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !frames)
                return 2;
        }
        else
            return 2;
    }
    lux::meta::ReflectionRegistry::initRegistry();
    auto metadata = examples::buildDevelopmentSceneMeta();
    if (!metadata)
    {
        std::fprintf(stderr, "ER1 metadata error=%u\n", unsigned(metadata.error()));
        return 1;
    }
    auto pak = lux::asset::PakAssetProvider::loadFromFile(asset_path);
    if (!pak)
    {
        std::fprintf(stderr, "ER1 asset package: %s\n", pak.error().c_str());
        return 1;
    }
    application::EditorApplicationCreateInfo config;
    config.window.visible = !hidden;
    config.window.font = std::move(font);
    config.window.title = "Lux Editor / ER-1";
    config.renderer.validation = validation;
    config.renderer.validation_message_sink = [](std::uint32_t severity, std::string_view text) {
        std::fprintf(stderr, "Vulkan severity=%u %.*s\n", severity, int(text.size()), text.data());
    };
    config.execution = {2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}};
    config.asset_read = {64};
    config.mounts = {{"/Seed", *pak, 0}};
    config.metadata = std::make_shared<lux::scene::SceneMetaManager>(std::move(*metadata));
    config.source = alternate ? &examples::openAlternateScene : &examples::openDevelopmentScene;
    auto created = application::EditorApplication::create(config);
    if (!created)
    {
        report(created.error());
        return 1;
    }
    auto app = std::move(*created);
    const auto start = std::chrono::steady_clock::now();
    const auto started = app->start();
    int result = 1;
    if (started)
    {
        const auto run = app->run(frames);
        if (!run)
            report(run.error());
        else
        {
            std::fprintf(stderr, "ER1 ui_iterations=%zu\n", *run);
            result = 0;
        }
    }
    else
        report(started.error());
    static_cast<void>(app->requestClose());
    const auto closing = std::chrono::steady_clock::now();
    bool reported{};
    for (;;)
    {
        const auto closed = app->advanceShutdown(64);
        if (closed && *closed)
            break;
        if (!closed && !reported)
        {
            report(closed.error());
            reported = true;
            result = 1;
        }
        if (frames && std::chrono::steady_clock::now() - closing > std::chrono::seconds{30})
        {
            std::fprintf(stderr, "ER1 shutdown incomplete; owners retained; diagnostic process exit is FAILURE\n");
            std::fflush(stderr);
            std::_Exit(3);
        }
        std::this_thread::yield();
    }
    const auto stats = app->rendererStatistics();
    const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
    std::fprintf(
        stderr,
        "ER1 seconds=%.6f frames=%llu fif_mask=%llu descriptors=%llu/%llu texture_misses=%llu "
        "events=%llu dropped=%llu validation_errors=%llu gpu_completed=%llu leases=%zu views=%zu accepted=%zu\n",
        elapsed, stats.frames, stats.slots, stats.descriptors_created, stats.descriptors_retired, stats.texture_misses,
        stats.render_events, stats.dropped_events, stats.validation_errors, stats.gpu_completed, stats.runtime_leases,
        stats.views, stats.accepted_frames);
    if (stats.validation_errors || stats.texture_misses || stats.runtime_leases || stats.views)
        result = 1;
    app.reset();
    config.metadata.reset();
    lux::meta::ReflectionRegistry::destroyRegistry();
    return result;
}
