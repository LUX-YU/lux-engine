#include "EditorBootstrap.hpp"

#include <lux/engine/editor/application/EditorApplication.hpp>
#include <lux/engine/meta/Meta.hpp>

#include <charconv>
#include <cstdio>
#include <optional>
#include <string_view>

namespace
{
    struct Arguments final
    {
        std::size_t frames{};
        bool visible{true};
        bool validation{};
    };

    [[nodiscard]] std::optional<Arguments> parseArguments(int argc, char** argv) noexcept
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
            1280U,
            720U,
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
        std::fprintf(stderr, "usage: lux_editor [--frames positive-count] [--hidden] [--validation]\n");
        return 2;
    }

    lux::meta::ReflectionRegistry::initRegistry();
    auto meta = lux::editor::application::buildDevelopmentSceneMeta();
    if (!meta)
    {
        lux::meta::ReflectionRegistry::destroyRegistry();
        return 1;
    }

    auto application = lux::editor::EditorApplication::create({
        {2U, 64U, 64U, {64U}, lux::process::BlockingSchedulerConfig{2U, 64U}},
        {64U},
        std::move(*meta),
        {},
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
            auto bootstrap = lux::editor::application::EditorBootstrap::create(context->get());
            if (bootstrap)
            {
                const auto run = (*application)->run(arguments->frames);
                bootstrap->reset();
                result = run ? 0 : 1;
            }
        }
    }
    const auto shutdown = (*application)->shutdown();
    if (!shutdown)
        result = 1;
    application->reset();
    lux::meta::ReflectionRegistry::destroyRegistry();
    return result;
}
