#include <lux/engine/hosts/player/GameHost.hpp>

#include <lux/engine/hosts/game_application/GameApplication.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/input/ActionMapper.hpp>
#include <lux/engine/log/Log.hpp>

#include <chrono>
#include <cstdio>
#include <utility>

namespace lux::game
{
    struct GameHost::Impl final
    {
        GameHostConfig config;
        std::unique_ptr<lux::window::GlfwRuntime> glfw;
        std::unique_ptr<lux::window::LuxWindow> window;
        GameApplication application;
        bool live{false};

        ~Impl()
        {
            (void)application.close();
        }
    };

    GameHost::GameHost() = default;
    GameHost::~GameHost() = default;

    bool GameHost::init(const GameHostConfig& config)
    {
        if (impl_ && impl_->live)
            return true;

        auto next = std::make_unique<Impl>();
        next->config = config;

        // The process adapter chooses the diagnostic outlet. GameApplication
        // owns diagnostic categories and their runtime routing.
        lux::log::setOutput(
            [](const lux::log::LogRecord& record)
            { lux::log::writeRecordToStderr(record); }
        );

        next->glfw = std::make_unique<lux::window::GlfwRuntime>();
        if (!next->glfw->valid())
        {
            lux::log::error("player", "glfwInit failed");
            return false;
        }
        next->window = std::make_unique<lux::window::LuxWindow>(
            static_cast<int>(config.width),
            static_cast<int>(config.height),
            config.title
        );
        if (!next->window->isInitialized())
            return false;

#if defined(__PLATFORM_WIN32__)
        const auto native_surface = reinterpret_cast<std::uint64_t>(
            next->window->win32Handle()
        );
#else
        const std::uint64_t native_surface = 0u;
#endif
        if (native_surface == 0u)
        {
            lux::log::error(
                "player",
                "this desktop platform has no native surface adapter yet"
            );
            return false;
        }

        std::uint32_t framebuffer_width = 0;
        std::uint32_t framebuffer_height = 0;
        next->window->framebufferSize(
            framebuffer_width,
            framebuffer_height
        );
        GameApplicationConfig application_config;
        application_config.title = config.title;
        application_config.game_pak_file = config.pak_file;
        application_config.base_pak_file = config.base_pak_file;
        application_config.boot_scene = config.scene_vpath;
        application_config.save_root = config.save_root;
        application_config.capacity_request = config.capacity_request;
        application_config.extensions = config.extensions;
        application_config.enable_validation = config.enable_validation;
        const auto required_extensions =
            lux::window::LuxWindow::requiredVulkanInstanceExtensions();
        application_config.vulkan_instance_extensions.reserve(
            required_extensions.size()
        );
        for (const auto* extension : required_extensions)
            application_config.vulkan_instance_extensions.emplace_back(extension);

        if (!next->application.start(
                std::move(application_config),
                native_surface,
                lux::math::Extent2u{
                    framebuffer_width,
                    framebuffer_height
                }
            ))
        {
            return false;
        }

        next->live = true;
        impl_ = std::move(next);
        return true;
    }

    void GameHost::dumpRenderGraph()
    {
        if (!impl_ || !impl_->live)
            return;
        auto dump = impl_->application.renderGraphDump();
        if (!dump)
            return;
        std::fprintf(
            stderr, // no_terminal_io: allow (explicit product output)
            "=== RENDER GRAPH DUMP (%zu bytes) ===\n%.*s\n"
            "=== RENDER GRAPH DUMP END ===\n",
            dump->size(),
            static_cast<int>(dump->size()),
            dump->data()
        );
    }

    int GameHost::run()
    {
        if (!impl_ || !impl_->live)
            return 1;
        auto& host = *impl_;

        using clock = std::chrono::steady_clock;
        auto last = clock::now();
        std::uint64_t frame_count = 0u;
        while (!host.window->shouldClose())
        {
            lux::window::LuxWindow::pollEvents();
            const auto now = clock::now();
            const float dt = std::chrono::duration<float>(now - last).count();
            last = now;

            std::uint32_t framebuffer_width = 0;
            std::uint32_t framebuffer_height = 0;
            host.window->framebufferSize(
                framebuffer_width,
                framebuffer_height
            );
            if (framebuffer_width == 0u || framebuffer_height == 0u)
            {
                (void)host.application.pumpIdleFor(
                    std::chrono::milliseconds{10}
                );
                continue;
            }

            const auto snapshot = host.window->captureInputSnapshot();
            host.application.inputMapper().update(
                snapshot,
                host.application.inputContexts(),
                dt
            );

            if (host.config.dump_graph && ++frame_count == 120u)
                dumpRenderGraph();

            (void)host.application.tick(
                dt,
                lux::math::Extent2u{
                    framebuffer_width,
                    framebuffer_height
                }
            );
        }
        return 0;
    }

    void GameHost::shutdown() noexcept
    {
        if (!impl_)
            return;
        if (impl_->application.close())
        {
            impl_->live = false;
            impl_.reset();
        }
    }

    lux::extensions::EngineExtensions& GameHost::extensions() noexcept
    {
        return impl_->application.extensions();
    }
}
