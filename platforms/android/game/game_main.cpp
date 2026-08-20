// Android NativeActivity adapter for the platform-neutral GameApplication.
// This file owns APK extraction, NativeWindow lifecycle, ALooper integration
// and Logcat. Runtime/scene/render/extension/shutdown composition lives in the
// installed game_application component shared with desktop and future games.

#include <lux/engine/hosts/game_application/GameApplication.hpp>
#include <lux/game/LaunchManifest.hpp>
#include <lux/engine/window/LuxWindow.hpp>
#include <lux/engine/log/Log.hpp>

#include <android_native_app_glue.h>
#include <android/asset_manager.h>
#include <android/log.h>
#include <android/native_window.h>

#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <utility>

#define LUX_TAG "luxgame"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO, LUX_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LUX_TAG, __VA_ARGS__)

namespace
{
    void redirectStdioToLogcat()
    {
        static int pipe_fds[2];
        static bool started = false;
        if (started || pipe(pipe_fds) != 0)
            return;
        started = true;
        setvbuf(stdout, nullptr, _IOLBF, 0);
        setvbuf(stderr, nullptr, _IONBF, 0);
        dup2(pipe_fds[1], STDOUT_FILENO);
        dup2(pipe_fds[1], STDERR_FILENO);
        std::thread(
            []
            {
                char line[1024];
                ssize_t count;
                while ((count = read(
                            pipe_fds[0],
                            line,
                            sizeof(line) - 1
                        )) > 0)
                {
                    while (count > 0 &&
                           (line[count - 1] == '\n' ||
                            line[count - 1] == '\r'))
                    {
                        --count;
                    }
                    line[count] = '\0';
                    if (count > 0)
                    {
                        __android_log_write(
                            ANDROID_LOG_INFO,
                            "luxstdio",
                            line
                        );
                    }
                }
            }
        ).detach();
    }

    struct AndroidGame final
    {
        std::unique_ptr<lux::game::GameApplication> application;
        std::uint32_t width{0u};
        std::uint32_t height{0u};
        bool animating{false};
        std::chrono::steady_clock::time_point last_frame{};
    };

    AndroidGame game;

    [[nodiscard]] bool extractAsset(
        AAssetManager* manager,
        const char* name,
        const std::filesystem::path& destination)
    {
        AAsset* asset = AAssetManager_open(
            manager,
            name,
            AASSET_MODE_STREAMING
        );
        if (!asset)
        {
            LOGE("APK asset '%s' is missing", name);
            return false;
        }

        const auto total = static_cast<std::uint64_t>(
            AAsset_getLength64(asset)
        );
        std::error_code error;
        std::filesystem::create_directories(
            destination.parent_path(),
            error
        );
        if (error)
        {
            AAsset_close(asset);
            LOGE(
                "cannot create '%s': %s",
                destination.parent_path().string().c_str(),
                error.message().c_str()
            );
            return false;
        }
        if (std::filesystem::exists(destination, error) &&
            std::filesystem::file_size(destination, error) == total)
        {
            AAsset_close(asset);
            return true;
        }

        std::ofstream output(
            destination,
            std::ios::binary | std::ios::trunc
        );
        if (!output)
        {
            AAsset_close(asset);
            LOGE("cannot write '%s'", destination.string().c_str());
            return false;
        }
        char buffer[64u * 1024u];
        int count;
        while ((count = AAsset_read(asset, buffer, sizeof(buffer))) > 0)
            output.write(buffer, count);
        AAsset_close(asset);
        output.flush();
        const bool complete = output.good() && count == 0;
        if (!complete)
            LOGE("extract '%s' failed mid-stream", name);
        return complete;
    }

    [[nodiscard]] bool isDeploymentRelative(
        const std::filesystem::path& path)
    {
        if (path.empty() || path.is_absolute() || path.has_root_path())
            return false;
        for (const auto& part : path.lexically_normal())
        {
            if (part == "..")
                return false;
        }
        return true;
    }

    [[nodiscard]] bool prepareApplicationConfig(
        android_app* native_app,
        lux::game::GameApplicationConfig& config)
    {
        constexpr const char* kManifestAsset = "game.luxruntime.toml";
        AAssetManager* asset_manager =
            native_app->activity->assetManager;
        const std::filesystem::path data_directory =
            native_app->activity->internalDataPath;
        const auto manifest_path = data_directory / kManifestAsset;
        if (!extractAsset(asset_manager, kManifestAsset, manifest_path))
            return false;

        auto manifest = lux::game::LaunchManifest::loadFromFile(
            manifest_path
        );
        if (!manifest)
        {
            LOGE(
                "packaged runtime manifest was rejected: %s",
                manifest.error().c_str()
            );
            return false;
        }
        if (!isDeploymentRelative(manifest->game_pak) ||
            (!manifest->base_pak.empty() &&
             !isDeploymentRelative(manifest->base_pak)))
        {
            LOGE("runtime pak paths must be APK-relative");
            return false;
        }

        const auto deployment = data_directory / "deployment";
        const auto game_pak_path = deployment / manifest->game_pak;
        const auto game_pak_asset = manifest->game_pak.generic_string();
        if (!extractAsset(
                asset_manager,
                game_pak_asset.c_str(),
                game_pak_path
            ))
        {
            return false;
        }

        std::filesystem::path base_pak_path;
        if (!manifest->base_pak.empty())
        {
            base_pak_path = deployment / manifest->base_pak;
            const auto base_pak_asset =
                manifest->base_pak.generic_string();
            if (!extractAsset(
                    asset_manager,
                    base_pak_asset.c_str(),
                    base_pak_path
                ))
            {
                return false;
            }
        }

        config.extensions.clear();
        for (const auto& extension : manifest->extensions)
        {
            if (!isDeploymentRelative(extension.path))
            {
                LOGE(
                    "runtime extension '%.*s' must use an APK-relative path",
                    static_cast<int>(extension.id.name().size()),
                    extension.id.name().data()
                );
                return false;
            }
            const auto packaged_path = extension.path.generic_string();
            const auto extracted_path = deployment / extension.path;
            if (!extractAsset(
                    asset_manager,
                    packaged_path.c_str(),
                    extracted_path
                ))
            {
                return false;
            }
            config.extensions.push_back(
                lux::extensions::ExtensionModuleRequirement::fromPath(
                    extension.id,
                    extracted_path,
                    lux::extensions::EExtensionModuleTarget::RUNTIME,
                    extension.required_major,
                    extension.minimum_minor
                )
            );
        }

        config.title = manifest->title;
        config.game_pak_file = game_pak_path;
        config.base_pak_file = base_pak_path;
        config.boot_scene = manifest->boot_scene;
        config.blocking_io_threads = 2u;
        config.background_cpu_concurrency = 2u;
        config.texture_streaming = {
            .query_interval_frames = 8u,
            .maximum_demand_entries = 16u,
            .maximum_replacement_tasks = 2u,
            .maximum_replacement_bytes = 4u * 1024u * 1024u,
        };
        const auto required_extensions =
            lux::window::LuxWindow::requiredVulkanInstanceExtensions();
        config.vulkan_instance_extensions.reserve(
            required_extensions.size()
        );
        for (const auto* extension : required_extensions)
            config.vulkan_instance_extensions.emplace_back(extension);
        return true;
    }

    [[nodiscard]] lux::math::Extent2u updateSurfaceExtent(
        android_app* native_app)
    {
        game.width = static_cast<std::uint32_t>(
            ANativeWindow_getWidth(native_app->window)
        );
        game.height = static_cast<std::uint32_t>(
            ANativeWindow_getHeight(native_app->window)
        );
        return {game.width, game.height};
    }

    [[nodiscard]] bool startApplication(android_app* native_app)
    {
        lux::game::GameApplicationConfig config;
        if (!prepareApplicationConfig(native_app, config))
            return false;

        auto application =
            std::make_unique<lux::game::GameApplication>();
        const auto extent = updateSurfaceExtent(native_app);
        if (!application->start(
                std::move(config),
                reinterpret_cast<std::uint64_t>(native_app->window),
                extent
            ))
        {
            return false;
        }
        ALooper* looper = native_app->looper;
        application->bindExternalWake(
            [looper]
            {
                if (looper)
                    ALooper_wake(looper);
            }
        );
        game.application = std::move(application);
        return true;
    }

    [[nodiscard]] bool acquireSurface(android_app* native_app)
    {
        if (!native_app->window)
            return false;
        if (!game.application)
            return startApplication(native_app);

        const auto extent = updateSurfaceExtent(native_app);
        return game.application->attachSurface(
            reinterpret_cast<std::uint64_t>(native_app->window),
            extent
        );
    }

    void releaseSurface()
    {
        if (game.application &&
            game.application->surfaceAttached() &&
            !game.application->detachSurface())
        {
            LOGE("surface target release failed");
        }
    }

    void frame()
    {
        const auto now = std::chrono::steady_clock::now();
        const float dt = std::chrono::duration<float>(
            now - game.last_frame
        ).count();
        game.last_frame = now;
        (void)game.application->tick(
            dt,
            lux::math::Extent2u{game.width, game.height}
        );
    }

    void onAppCmd(android_app* native_app, std::int32_t command)
    {
        switch (command)
        {
        case APP_CMD_INIT_WINDOW:
            LOGI("[lifecycle] INIT_WINDOW");
            if (native_app->window && acquireSurface(native_app))
            {
                game.last_frame = std::chrono::steady_clock::now();
                game.animating = true;
            }
            break;

        case APP_CMD_TERM_WINDOW:
            LOGI("[lifecycle] TERM_WINDOW");
            game.animating = false;
            releaseSurface();
            break;

        case APP_CMD_GAINED_FOCUS:
            if (game.application &&
                !game.application->surfaceAttached() &&
                native_app->window)
            {
                LOGI("[lifecycle] re-acquiring retained NativeWindow");
                if (acquireSurface(native_app))
                    game.last_frame = std::chrono::steady_clock::now();
            }
            game.animating = game.application &&
                game.application->surfaceAttached();
            break;

        case APP_CMD_LOST_FOCUS:
            game.animating = false;
            break;

        default:
            LOGI("[lifecycle] cmd=%d", command);
            break;
        }
    }
}

void android_main(android_app* native_app)
{
    redirectStdioToLogcat();
    LOGI("=== lux game start ===");
    lux::log::setOutput(
        [](const lux::log::LogRecord& record)
        { lux::log::writeRecordToLogcat(record, LUX_TAG); }
    );

    native_app->onAppCmd = onAppCmd;
    for (;;)
    {
        int events = 0;
        android_poll_source* source = nullptr;
        while (ALooper_pollOnce(
                   game.animating ? 0 : -1,
                   nullptr,
                   &events,
                   reinterpret_cast<void**>(&source)
               ) >= 0)
        {
            if (source)
                source->process(native_app, source);
            if (native_app->destroyRequested)
            {
                LOGI("[lifecycle] destroyRequested");
                game.animating = false;
                if (game.application && !game.application->close())
                {
                    LOGE("game application close watchdog expired");
                    continue;
                }
                game.application.reset();
                LOGI("=== lux game end ===");
                return;
            }
        }
        if (!game.animating && game.application)
            (void)game.application->pumpSafePoint();
        if (game.animating && game.application &&
            game.application->surfaceAttached())
        {
            frame();
        }
    }
}
