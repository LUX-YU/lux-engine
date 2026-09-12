#include "CostSample.hpp"
#include "TailCost.hpp"
#include <lux/engine/editor/application/EditorApplication.hpp>
#include <lux/engine/editor/application/UiVulkanPresentation.hpp>
#include <lux/engine/editor/scene/SceneWorkbenchMeasurement.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <lux/engine/window/LuxWindow.hpp>

int main(int argc, char **argv)
{
    using namespace lux::editor;
    using namespace er1_cost;
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    assert(argc == 3 || argc == 5);
    const bool tails = argc == 5;
    const bool retry = tails && std::string_view{argv[3]} == "retry";
    assert(!tails || retry || std::string_view{argv[3]} == "resize");
    lux::meta::ReflectionRegistry::initRegistry();
    {
        auto metadata = workbench::buildDevelopmentSceneMeta();
        auto pak = lux::asset::PakAssetProvider::loadFromFile(argv[1]);
        assert(metadata && pak);
        auto provider = std::make_shared<RecoveryProvider>();
        provider->initial = *pak;
        auto complete = lux::asset::PakAssetProvider::loadFromFile(tails ? argv[4] : argv[1]);
        assert(complete);
        provider->complete = *complete;
        assert(!tails || provider->validInputs(retry));
        auto created = EditorApplication::create(
            {{2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}},
             {64},
             std::move(*metadata),
             {{"/Seed", provider, 0}},
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
        const auto layout = [&](bool small) {
            ui.setSplitLayout({"lux.scene.outline", "lux.scene.viewport", "lux-editor.entity-inspector",
                "lux.scene.resources", small ? 338.0F : 210.0F, 340, small ? 272.0F : 208.0F, "lux.scene.toolbar"});
        };
        std::uint64_t expected_frame{}, poll_count{}, completed_scene_frames{};
        bool submitted_scene{};
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
            submitted_scene = !workbench::detail::SceneWorkbenchMeasurement::read(*scene).presentation_pending;
            expected_frame = presenter->diagnostics().frames + submitted_scene;
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
            if (state.ready == (retry ? 2 : 3) && state.failed == (retry ? 1 : 0) && state.settled == 3 &&
                state.resources == 3 && !state.resizing && state.width == width &&
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
            if (tails && submitted_scene) ++completed_scene_frames;
        };
        std::vector<std::byte> final_pixels;
        const auto readback = [&]
        {
            auto state = workbench::detail::SceneWorkbenchMeasurement::read(*scene);
            assert(state.width && state.height && !state.resizing);
            std::vector<std::byte> pixels(std::size_t(state.width) * state.height * 4);
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
        TailReport tail_report;
        tail_report.initial_checksum = original_checksum;
        Sample sample;
        if (tails)
        {
            const auto state = [&] {
                const auto view = workbench::detail::SceneWorkbenchMeasurement::read(*scene);
                const auto stats = presenter->diagnostics();
                return TailState{view.width, view.height, view.ready, view.failed, view.resources, view.live_handles,
                    view.serial_sum, stats.descriptors_created, stats.descriptors_retired, view.resizing, stats.frames};
            };
            const auto frames = [&] { return completed_scene_frames; };
            const auto step = [&] { frame(); return submitted_scene; };
            if (retry)
            {
                const auto failed = workbench::detail::SceneWorkbenchMeasurement::read(*scene);
                assert(failed.failed == 1 && failed.ready == 2 && failed.settled == 3 && provider->missing.load());
                assert(failed.asset_error == unsigned(lux::process::asset_loading::EAssetLoadError::STORAGE_FAILURE));
                assert(failed.storage_error == unsigned(lux::asset::EAssetStorageError::NOT_FOUND));
                std::printf("tail retry source=legacy asset_error=%u storage_error=%u serial_sum=%llu\n",
                    failed.asset_error, failed.storage_error, failed.serial_sum);
                // The existing production retry button is the legacy entry point. Fixed layout,
                // no private state writer and no injected model mutation. This is not OS input evidence.
                unsigned click_step{};
                tail_report.measure("retry", width, height, frames, [&] {
                    if (click_step == 0)
                    {
                        provider->recovered.store(true, std::memory_order_release);
                        ui.feedInput(lux::ui::UiWindowFocus{true});
                        ui.feedInput(lux::ui::UiPointerMove{{50, 720}});
                    }
                    if (click_step == 1) ui.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, true});
                    if (click_step == 2) ui.feedInput(lux::ui::UiPointerButton{lux::ui::EPointerButton::LEFT, false});
                    ++click_step;
                    return step();
                }, drain, state, readback, poll_count);
                assert(state().serials == failed.serial_sum + 1);
            }
            else
            {
                bool resized{};
                tail_report.measure("resize_down", 896, 512, frames, [&] {
                    if (!std::exchange(resized, true)) layout(true);
                    return step();
                }, drain, state, readback, poll_count);
                resized = false;
                tail_report.measure("resize_restore", width, height, frames, [&] {
                    if (!std::exchange(resized, true)) layout(false);
                    return step();
                }, drain, state, readback, poll_count);
                assert(tail_report.stages.back().sample.checksum == original_checksum);
            }
        }
        else
        {
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
        }
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
        if (tails)
        {
            tail_report.closed_memory = processMemory();
            tail_report.close_wall = seconds(Clock::now() - close_started);
            tail_report.descriptors_created = stats.descriptors_created;
            tail_report.descriptors_retired = stats.descriptors_retired;
            tail_report.write(argv[2], "legacy", *provider);
        }
        else sample.write(argv[2], "legacy", seconds(Clock::now() - close_started));
        savePixels(argv[2], final_pixels);
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
}
