#include "../DevelopmentScene.hpp"
#include <cassert>
#include <chrono>
#include <cstdio>
#include <string_view>
#include <thread>
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
