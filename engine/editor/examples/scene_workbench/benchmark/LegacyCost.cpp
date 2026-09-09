#include "CostSample.hpp"
#include <lux/engine/editor/application/EditorApplication.hpp>
#include <lux/engine/editor/application/UiVulkanPresentation.hpp>
#include <lux/engine/editor/scene/SceneWorkbenchMeasurement.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <lux/engine/window/LuxWindow.hpp>

int main(int argc, char **argv)
{
    using namespace lux::editor;
    using namespace er1_cost;
    assert(argc == 3);
    lux::meta::ReflectionRegistry::initRegistry();
    {
        auto metadata = workbench::buildDevelopmentSceneMeta();
        auto pak = lux::asset::PakAssetProvider::loadFromFile(argv[1]);
        assert(metadata && pak);
        auto created = EditorApplication::create(
            {{2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}},
             {64},
             std::move(*metadata),
             {{"/Seed", *pak, 0}},
             EditorPresentationConfig{
                 1600, 900, "ER1 legacy cost", 3, 8, 8, 16 * 1024 * 1024, {32, 8192, 2, 8, 1024}, false, false}});
        assert(created && (*created)->start());
        auto app = std::move(*created);
        auto context = app->context();
        assert(context);
        auto *presenter = dynamic_cast<application::detail::UiVulkanPresentation *>(app->sceneViewRenderPort());
        assert(presenter);
        auto built = workbench::SceneWorkbench::create(context->get(), *presenter);
        assert(built);
        auto scene = std::move(*built);
        auto &ui = context->get().ui();
        ui.setSplitLayout({"lux.scene.outline", "lux.scene.viewport", "lux-editor.entity-inspector",
                           "lux.scene.resources", 210, 340, 208, "lux.scene.toolbar"});
        auto acquired = presenter->acquire();
        assert(acquired);
        auto runtime = std::move(*acquired);
        std::uint64_t expected_frame{}, poll_count{};
        const auto deadline = Clock::now() + std::chrono::seconds{90};
        const auto poll = [&]
        {
            ++poll_count;
            assert(Clock::now() < deadline && app->drainMain(64));
            presenter->pump();
            assert(!presenter->stopping());
        };
        const auto frame = [&]
        {
            lux::window::LuxWindow::pollEvents();
            poll();
            scene->beforeUiFrame();
            auto draw = ui.beginFrame({{1600, 900}, float(delta), {1, 1}});
            draw.drawPanes();
            scene->afterUiFrame(delta, {1, 1});
            draw.finish();
            expected_frame = presenter->diagnostics().frames + 1;
            assert(presenter->present(ui));
        };
        for (;;)
        {
            frame();
            const auto state = workbench::detail::SceneWorkbenchMeasurement::read(*scene);
            if (state.ready == 3 && !state.resizing && presenter->diagnostics().descriptors_created >= 2)
            {
                std::printf("baseline ready target=%ux%u resources=%zu frames=%llu\n", state.width, state.height,
                            state.ready, presenter->diagnostics().frames);
                std::fflush(stdout);
                assert(state.width == width && state.height == height);
            }
            if (state.ready == 3 && state.resources == 3 && !state.resizing && state.width == width &&
                state.height == height && presenter->diagnostics().descriptors_created >= 2)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds{8});
        }
        const auto drain = [&]
        {
            while (presenter->framePending() || presenter->diagnostics().frames < expected_frame)
            {
                poll();
                // The original presenter retries a retained frame through present(), not pump().
                if (presenter->framePending())
                    assert(presenter->present(ui));
                std::this_thread::yield();
            }
        };
        std::vector<std::byte> final_pixels;
        const auto readback = [&]
        {
            auto state = workbench::detail::SceneWorkbenchMeasurement::read(*scene);
            assert(state.width == width && state.height == height && state.ready == 3 && !state.resizing);
            std::vector<std::byte> pixels(std::size_t(width) * height * 4);
            auto request = runtime.control().readbackTargetAsync(state.target, pixels.data(), pixels.size());
            assert(request.valid());
            const auto before = presenter->diagnostics().frames;
            for (unsigned i = 0; i != 8; ++i)
            {
                lux::render::RenderProgram<> verification;
                verification.kind = lux::render::ERenderProgramKind::Frame;
                while (!runtime.programs().trySubmitPrepared(verification))
                    poll();
            }
            while (presenter->diagnostics().frames < before + 8)
                poll();
            while (!request.isReady())
            {
                poll();
                std::this_thread::yield();
            }
            const auto result = request.tryResult();
            assert(result && result->get().status == 0 && result->get().bytes_written == pixels.size());
            final_pixels = std::move(pixels);
            return checksum(final_pixels);
        };
        drain();
        for (unsigned i = 0; i != warmup; ++i)
        {
            const auto tick = Clock::now();
            frame();
            drain();
            std::this_thread::sleep_until(tick + std::chrono::milliseconds{8});
        }
        const auto original_checksum = readback();
        Sample sample;
        sample.begin(presenter->diagnostics().frames, poll_count);
        for (unsigned i = 0; i != measured; ++i)
        {
            const auto tick = Clock::now();
            sample.work(frame);
            sample.wait(
                [&]
                {
                    drain();
                    std::this_thread::sleep_until(tick + std::chrono::milliseconds{8});
                });
            ++sample.iterations;
        }
        while (presenter->diagnostics().frames < sample.frames_before + measured)
            poll();
        const auto measured_frames = presenter->diagnostics().frames;
        sample.wait([&] { sample.checksum = readback(); });
        assert(sample.checksum == original_checksum);
        assert(presenter->diagnostics().frames == measured_frames + 8);
        sample.finish(measured_frames);
        const auto close_started = Clock::now();
        runtime = {};
        scene->requestClose();
        while (!scene->advanceClose())
        {
            poll();
            std::this_thread::yield();
        }
        scene.reset();
        presenter->requestStop();
        assert(presenter->join());
        const auto stats = presenter->diagnostics();
        assert(!stats.render_events && !stats.texture_misses && stats.descriptors_created == stats.descriptors_retired);
        assert(app->shutdown());
        app.reset();
        sample.write(argv[2], "legacy", seconds(Clock::now() - close_started));
        savePixels(argv[2], final_pixels);
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
}
