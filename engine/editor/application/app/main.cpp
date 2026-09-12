#include <lux/engine/editor/scene/SceneWorkbench.hpp>

#include <lux/engine/editor/application/EditorApplication.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <filesystem>

#include <charconv>
#include <cstdio>
#include <optional>
#include <string_view>
#include <chrono>

namespace
{
    struct Arguments final
    {
        std::size_t frames{};
        bool visible{true};
        bool validation{};
        std::filesystem::path assets;
    };

    [[nodiscard]] std::optional<Arguments> parseArguments(int argc, char** argv)
    {
        Arguments result;
        for (int index = 1; index < argc; ++index)
        {
            const std::string_view argument{argv[index]};
            if (argument == "--hidden")
            {
                result.visible = false;
            }
            else if (argument == "--validation")
            {
                result.validation = true;
            }
            else if (argument == "--assets" && index + 1 < argc)
                result.assets = argv[++index];
            else if (argument == "--frames" && index + 1 < argc)
            {
                const std::string_view value{argv[++index]};
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result.frames);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || result.frames == 0U)
                    return std::nullopt;
            }
            else
            {
                return std::nullopt;
            }
        }
        return result;
    }

    [[nodiscard]] lux::editor::EditorPresentationConfig presentationConfig(const Arguments& arguments)
    {
        return {
            1600U,
            900U,
            "Lux Editor",
            3U,
            8U,
            8U,
            16U * 1024U * 1024U,
            {8U, 4096U, 2U, 8U, 1024U},
            arguments.visible,
            arguments.validation
        };
    }
} // namespace

int main(int argc, char** argv)
{
    const auto arguments = parseArguments(argc, argv);
    if (!arguments)
    {
        std::fprintf(stderr, "usage: lux_editor [--frames positive-count] [--hidden] [--validation] [--assets pak]\n");
        return 2;
    }

    lux::meta::ReflectionRegistry::initRegistry();
    auto meta = lux::editor::workbench::buildDevelopmentSceneMeta();
    if (!meta)
    {
        lux::meta::ReflectionRegistry::destroyRegistry();
        return 1;
    }

    const auto assets_path = arguments->assets.empty() ?
        std::filesystem::absolute(argv[0]).parent_path().parent_path() / "share/lux-engine/editor/sv1.luxpak" :
        arguments->assets;
    auto provider = lux::asset::PakAssetProvider::loadFromFile(assets_path);
    if (!provider)
    {
        std::fprintf(stderr, "Seed assets unavailable: %s\n", provider.error().c_str());
        lux::meta::ReflectionRegistry::destroyRegistry();
        return 1;
    }
    auto application = lux::editor::EditorApplication::create({
        {2U, 64U, 64U, {64U}, lux::process::BlockingSchedulerConfig{2U, 64U}},
        {64U},
        std::move(*meta),
        {{"/Seed", *provider, 0}},
        presentationConfig(*arguments)
    });
    if (!application)
    {
        lux::meta::ReflectionRegistry::destroyRegistry();
        return 1;
    }

    int result = 1;
    if ((*application)->start())
    {
        auto context = (*application)->context();
        if (context)
        {
            auto bootstrap = lux::editor::workbench::SceneWorkbench::create(
                context->get(), *(*application)->sceneViewRenderPort());
            if (bootstrap)
            {
                const auto started = std::chrono::steady_clock::now();
                const auto run = (*application)->run(arguments->frames, bootstrap->get());
                const auto elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
                std::fprintf(stderr, "SV1 ui iterations=%zu elapsed_seconds=%.6f result=%s\n",
                    run ? *run : 0, elapsed, run ? "success" : "failure");
                result = run ? 0 : 1;
                bootstrap->reset();
            }
            else
                std::fprintf(stderr, "Workbench creation failed: %u\n", unsigned(bootstrap.error()));
        }
    }
    const auto shutdown = (*application)->shutdown();
    if (!shutdown)
        result = 1;
    application->reset();
    lux::meta::ReflectionRegistry::destroyRegistry();
    return result;
}
