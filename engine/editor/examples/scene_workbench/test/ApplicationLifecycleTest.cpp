#include "../DevelopmentScene.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <string_view>
#include <thread>
#if defined(LUX_EDITOR_DIAGNOSTICS)
#include <lux/engine/editor/rendering/detail/RendererTestAccess.hpp>
#endif
namespace
{
    using namespace lux::editor;
    struct Probe
    {
        unsigned &stops, &destroyed;
        Probe(unsigned &s, unsigned &d) : stops(s), destroyed(d)
        {
        }
        void requestStop() noexcept
        {
            ++stops;
        }
        ~Probe() noexcept
        {
            ++destroyed;
        }
    };
    struct OtherTool
    {
    };
    sessions::SceneResult<sessions::SceneOpenInfo> failSource(
        sessions::SessionId id, lux::object::ObjectDispatcherRef, rendering::EditorRenderer &,
        lux::process::asset_loading::AssetReadPort, std::shared_ptr<const lux::scene::SceneMetaManager>) noexcept
    {
        return lux::cxx::unexpected(sessions::SceneFailure{sessions::ESceneError::INVALID_ARGUMENT, id});
    }
#if defined(LUX_EDITOR_DIAGNOSTICS)
    sessions::SceneResult<sessions::SceneOpenInfo> failViewSource(
        sessions::SessionId id, lux::object::ObjectDispatcherRef dispatcher, rendering::EditorRenderer &renderer,
        lux::process::asset_loading::AssetReadPort assets,
        std::shared_ptr<const lux::scene::SceneMetaManager> metadata) noexcept
    {
        auto source = examples::openDevelopmentScene(id, std::move(dispatcher), renderer, std::move(assets),
                                                   std::move(metadata));
        if (source)
            rendering::detail::RendererTestAccess::useSceneForNextView({1001, 9});
        return source;
    }
#endif
} // namespace
int main(int argc, char **argv)
{
    using namespace lux::editor;
    using Clock = std::chrono::steady_clock;
    const bool metadata_only = argc == 2 && std::string_view{argv[1]} == "metadata";
    assert(argc == 1 || metadata_only);
    const auto uninitialized = examples::buildDevelopmentSceneMeta();
    assert(!uninitialized && uninitialized.error() == examples::EDemoBuildError::META_BUILD_FAILURE);
    lux::meta::ReflectionRegistry::initRegistry();
    if (metadata_only)
    {
        auto &registry = lux::meta::ReflectionRegistry::instance();
        const auto *configuration = registry.findClass("lux::simulation::TransformSystemConfiguration");
        assert(configuration && configuration->type.ptr == configuration);
        for (unsigned repeat = 0; repeat != 100; ++repeat)
        {
            auto metadata = examples::buildDevelopmentSceneMeta();
            assert(metadata);
            assert(registry.findClass("lux::simulation::TransformSystemConfiguration") == configuration);
            assert(configuration->type.ptr == configuration);
        }
        lux::meta::ReflectionRegistry::destroyRegistry();
        std::puts("ER1 metadata PASS: uninitialized rejection, 100 builds, stable reflection identity");
        return 0;
    }
    {
        auto built_meta = examples::buildDevelopmentSceneMeta();
        assert(built_meta);
        auto metadata = std::make_shared<lux::scene::SceneMetaManager>(std::move(*built_meta));
#if defined(LUX_EDITOR_DIAGNOSTICS)
        {
            application::EditorApplicationCreateInfo info;
            info.metadata = metadata;
            info.source = &failViewSource;
            info.execution = {2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}};
            info.asset_read = {64};
            info.window.visible = false;
            info.renderer.validation = true;
            auto created = application::EditorApplication::create(info);
            assert(created);
            auto app = std::move(*created);
            assert(app->start());
            const auto first = app->run(128);
            const auto expected = lux::render::renderError<lux::render::err::scene::NotFound>(1001);
            assert(!first && first.error().renderer && first.error().renderer_diagnostic);
            const auto original = *first.error().renderer;
            assert(original.render_error.type == expected.type && original.render_error.args == expected.args);
            // The first call consumes the owning renderer diagnostic. The second must still receive
            // the exact per-View failure through Workspace, with no need for another backend event.
            const auto second = app->run(128);
            assert(!second && second.error().code == application::EApplicationError::SCENE_FAILURE);
            assert(second.error().scene && second.error().scene->renderer);
            const auto &propagated = *second.error().scene->renderer;
            assert(propagated.code == original.code && propagated.view == original.view &&
                   propagated.request == original.request &&
                   propagated.render_error.type == original.render_error.type &&
                   propagated.render_error.args == original.render_error.args &&
                   propagated.backend_status == original.backend_status);
            assert(app->requestClose());
            const auto deadline = Clock::now() + std::chrono::seconds{15};
            for (;;)
            {
                assert(Clock::now() < deadline);
                const auto closed = app->advanceShutdown(1);
                assert(closed);
                if (*closed)
                    break;
                std::this_thread::yield();
            }
            const auto statistics = app->rendererStatistics();
            assert(statistics.views == 0 && statistics.runtime_leases == 0 && statistics.validation_errors == 0);
            std::printf("G04 Application PASS Scene failure retains backend request=%llu; closed views=0 leases=0\n",
                        original.request);
        }
#endif
        for (unsigned phase = 0; phase != 4; ++phase)
        {
            application::EditorApplicationCreateInfo info;
            info.metadata = metadata;
            info.source = &failSource;
            info.execution = {2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}};
            info.asset_read = {64};
            info.window.visible = false;
            info.renderer.validation = true;
            if (phase == 0)
                info.window.width = 0;
            if (phase == 1)
                info.renderer.frame_capacity = 0;
            auto created = application::EditorApplication::create(info);
            assert(created && info.metadata == metadata && info.source == &failSource);
            auto app = std::move(*created);
            auto cold = app->tooling();
            assert(cold);
            unsigned stopped{}, destroyed{};
            auto installed = cold->get().install<Probe>(stopped, destroyed);
            assert(installed && cold->get().find<Probe>() == &installed->get());
            bool wrong_thread{};
            std::thread worker(
                [&]
                {
                    const auto lookup = app->tooling();
                    wrong_thread = !lookup && lookup.error().code == application::EApplicationError::WRONG_THREAD;
                });
            worker.join();
            assert(wrong_thread);
            if (phase == 3)
                assert(app->requestClose());
            const auto started = app->start();
            if (!started)
            {
                std::printf("startup phase=%u failure=%u\n", phase, unsigned(started.error().code));
                std::fflush(stdout);
            }
            assert(!started);
            assert(app->state() == (phase == 3 ? application::EApplicationState::COMPOSING
                                               : application::EApplicationState::START_FAILED));
            const auto expected = phase == 0 ? application::EApplicationError::WINDOW_FAILURE
                                             : (phase == 1 ? application::EApplicationError::RENDERER_FAILURE
                                                           : application::EApplicationError::SCENE_FAILURE);
            assert(started.error().code == (phase == 3 ? application::EApplicationError::INVALID_STATE : expected));
            assert(!app->tooling());
            if (phase != 3)
            {
                assert(cold->get().frozen());
                const auto late = cold->get().install<OtherTool>();
                assert(!late && late.error().code == application::EToolsetError::FROZEN);
            }
            assert(stopped == 0 && destroyed == 0);
            assert(app->requestClose());
            const auto status = app->shutdownStatus();
            assert(status && status->close_requested && !status->session);
            bool wrong_status_thread{};
            std::jthread status_reader([&] {
                const auto rejected = app->shutdownStatus();
                wrong_status_thread = !rejected &&
                    rejected.error().code == application::EApplicationError::WRONG_THREAD;
            });
            status_reader.join();
            assert(wrong_status_thread);
            const auto zero = app->advanceShutdown(0);
            assert(zero && !*zero && destroyed == 0);
            const auto deadline = Clock::now() + std::chrono::seconds{15};
            std::size_t attempts{};
            for (;;)
            {
                assert(Clock::now() < deadline);
                const auto closed = app->advanceShutdown(1);
                assert(closed);
                ++attempts;
                if (*closed)
                    break;
                std::this_thread::yield();
            }
            assert(stopped == 1 && destroyed == 1);
            assert(app->state() == application::EApplicationState::STOPPED);
            const auto closed_status = app->shutdownStatus();
            assert(closed_status && !closed_status->renderer && !closed_status->session &&
                   !closed_status->workspace_present && !closed_status->unattached_view_present);
            assert(!app->tooling());
            assert(app->requestClose() && *app->advanceShutdown(0) && *app->advanceShutdown(1));
            assert(!app->start());
            assert(app->rendererStatistics().validation_errors == 0);
            app.reset();
            assert(stopped == 1 && destroyed == 1);
            std::printf("Application partial startup PASS phase=%u shutdown_steps=%zu tool_stop=1 tool_destroy=1\n",
                        phase, attempts);
        }
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
}
