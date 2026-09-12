#include "CostSample.hpp"
#include "TailCost.hpp"
#include "../DevelopmentScene.hpp"
#include <lux/engine/editor/ui/scene/SceneWorkspace.hpp>
#include <lux/engine/editor/rendering/detail/ViewImageLifetime.hpp>
#include <lux/engine/editor/sessions/scene/SceneResourceStatus.hpp>
#include <lux/engine/object/ObjectDispatcher.hpp>
#include <lux/engine/window/GlfwRuntime.hpp>
#include <lux/engine/resource/asset/storage/pak/PakAssetProvider.hpp>
#include <algorithm>

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
        lux::window::GlfwRuntime platform;
        assert(platform.valid());
        lux::object::ObjectMessageQueue messages;
        auto execution =
            lux::process::ExecutionRuntime::create({2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}});
        auto pak = lux::asset::PakAssetProvider::loadFromFile(argv[1]);
        assert(execution && pak);
        auto provider = std::make_shared<RecoveryProvider>();
        provider->initial = *pak;
        auto complete = lux::asset::PakAssetProvider::loadFromFile(tails ? argv[4] : argv[1]);
        assert(complete);
        provider->complete = *complete;
        assert(!tails || provider->validInputs(retry));
        lux::asset::AssetVfs vfs;
        assert(vfs.mount({"/Seed", provider, 0}) != lux::asset::kInvalidMountId);
        auto blocking = execution->blocking();
        assert(blocking);
        auto endpoint = lux::process::asset_loading::VfsAssetReadEndpoint::create(vfs.view(), *blocking, {64});
        assert(endpoint);
        auto window_result = ui::EditorWindow::create(messages.dispatcherRef(), {1600, 900, "ER1 scene cost", false});
        assert(window_result);
        auto window = std::move(*window_result);
        auto renderer_result = rendering::EditorRenderer::create(window->nativeWindow(), window->uiSession(), {});
        assert(renderer_result);
        auto renderer = std::move(*renderer_result);
        auto metadata = examples::buildDevelopmentSceneMeta();
        assert(metadata);
        auto shared_meta = std::make_shared<lux::scene::SceneMetaManager>(std::move(*metadata));
        auto source =
            examples::openDevelopmentScene({1}, messages.dispatcherRef(), *renderer, (*endpoint)->port(), shared_meta);
        assert(source);
        auto opened = sessions::SceneSession::openInspection(*source);
        assert(opened);
        auto session = std::move(*opened);
        auto view = sessions::SceneView::create(messages.dispatcherRef(), *session, *renderer);
        assert(view);
        auto created = ui::SceneWorkspace::create(*window, {1}, *session, *view);
        assert(created);
        auto workspace = std::move(*created);
        assert(workspace->activate());
        window->uiSession().setSplitLayout({"lux.scene.workspace.1.outliner", "lux.scene.workspace.1.viewport",
                                            "lux.scene.workspace.1.inspector", "lux.scene.workspace.1.resources", 210,
                                            340, 227, "lux.scene.workspace.1.toolbar"});
        auto acquired = renderer->acquire();
        assert(acquired);
        auto runtime = std::move(*acquired);
        const auto layout = [&](bool small) {
            window->uiSession().setSplitLayout({"lux.scene.workspace.1.outliner", "lux.scene.workspace.1.viewport",
                "lux.scene.workspace.1.inspector", "lux.scene.workspace.1.resources", small ? 338.0F : 210.0F,
                340, small ? 291.0F : 227.0F, "lux.scene.workspace.1.toolbar"});
        };
        std::uint64_t cycle{}, expected_frame{}, poll_count{}, completed_scene_frames{};
        rendering::ViewImage image;
        rendering::EditorFramePacket pending;
        bool submitted_scene{};
        std::optional<sessions::ResourceRequestKey> retry_key;
        bool retry_requested{};
        const auto deadline = Clock::now() + std::chrono::seconds{90};
        const auto poll = [&]
        {
            ++poll_count;
            assert(Clock::now() < deadline && execution->drainMain(64) && renderer->poll(64));
            assert(renderer->state() == rendering::ERendererState::READY);
        };
        const auto frame = [&]
        {
            assert(window->collectInput());
            poll();
            assert(!pending.valid());
            const sessions::SceneOwnerUpdate update{++cycle, delta};
            assert(session->updateAtOwnerSafePoint(update) && workspace->updateBeforeFrame());
            if (retry_key && !retry_requested)
            {
                provider->recovered.store(true, std::memory_order_release);
                const auto result = session->retryResources(*retry_key);
                assert(result || result.error().code == sessions::ESceneError::BUSY);
                retry_requested = bool(result);
                if (result)
                {
                    const auto stale = session->retryResources(*retry_key);
                    assert(!stale && stale.error().code == sessions::ESceneError::STALE_CONTENT);
                }
            }
            assert(window->beginFrame({{1600, 900}, float(delta), {1, 1}}) && window->drawPanes());
            assert(workspace->afterDraw(delta, {1, 1}) && session->advanceScene(update));
            auto snapshot = window->finishFrame();
            assert(snapshot);
            const auto images = workspace->frameImages();
            image = images.empty() ? rendering::ViewImage{} : images.front();
            expected_frame = renderer->statistics().frames;
            submitted_scene = !session->presentationPending() && image.lease.valid();
            if (!session->presentationPending())
            {
                ++expected_frame;
                auto sealed = renderer->sealFrame(*snapshot, images);
                assert(sealed);
                pending = std::move(*sealed);
                assert(renderer->trySubmitFrame(pending));
            }
            workspace->releaseFrameImages();
        };
        const auto drain = [&]
        {
            while (pending.valid() || renderer->statistics().frames < expected_frame)
            {
                poll();
                if (pending.valid())
                    assert(renderer->trySubmitFrame(pending));
                std::this_thread::yield();
            }
            if (tails && submitted_scene)
            {
                const auto* record = rendering::detail::ViewImageAccess::record(image);
                assert(record);
                while (!record->submitted.load(std::memory_order_acquire))
                {
                    poll();
                    std::this_thread::yield();
                }
                ++completed_scene_frames;
            }
        };
        for (;;)
        {
            frame();
            // Resource presentation can defer a frame before the scene is ready.
            while (pending.valid())
            {
                poll();
                assert(renderer->trySubmitFrame(pending));
            }
            auto resources = session->readResources();
            assert(resources);
            if (image.lease.valid() && image.extent == rendering::PixelExtent{width, height} &&
                (*resources)->rows.size() == 3 &&
                std::all_of((*resources)->rows.begin(), (*resources)->rows.end(),
                            [&](const auto &row) { return row.state == sessions::ESceneResourceState::READY ||
                                (retry && row.state == sessions::ESceneResourceState::FAILED); }) &&
                renderer->statistics().descriptors_created >= 2)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds{8});
        }
        std::vector<std::byte> final_pixels;
        const auto readback = [&]
        {
            const auto pixel_width = image.extent.width, pixel_height = image.extent.height;
            assert(pixel_width && pixel_height);
            const auto *record = rendering::detail::ViewImageAccess::record(image);
            assert((record->camera.origin == std::array<double, 3>{6, 4, 8}));
            assert(record->wire_camera.view_matrix[12] == 0 && record->wire_camera.view_matrix[13] == 0 &&
                   record->wire_camera.view_matrix[14] == 0);
            std::vector<std::byte> pixels(std::size_t(pixel_width) * pixel_height * 4);
            auto request = runtime.control().readbackTargetAsync(record->version->target, pixels.data(), pixels.size());
            assert(request.valid());
            const auto before = renderer->statistics().frames;
            for (unsigned i = 0; i != 8; ++i)
            {
                lux::render::RenderProgram<> verification;
                verification.kind = lux::render::ERenderProgramKind::Frame;
                while (!runtime.programs().trySubmitPrepared(verification))
                    poll();
            }
            while (renderer->statistics().frames < before + 8)
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
                TailState result;
                result.width = image.extent.width;
                result.height = image.extent.height;
                const auto resources = session->readResources();
                assert(resources);
                result.requests = (*resources)->rows.size();
                for (const auto& row : (*resources)->rows)
                {
                    result.ready += row.state == sessions::ESceneResourceState::READY;
                    result.failed += row.state == sessions::ESceneResourceState::FAILED;
                    result.serials += row.key.sequence;
                }
                const auto stats = renderer->statistics();
                result.descriptors_created = stats.descriptors_created;
                result.descriptors_retired = stats.descriptors_retired;
                result.backend_frames = stats.frames;
                return result;
            };
            const auto frames = [&] { return completed_scene_frames; };
            const auto step = [&] { frame(); return submitted_scene; };
            const auto record_handles = [&] {
                const auto owners = session->closeStatus();
                assert(owners);
                for (const auto& row : (*owners)->resources)
                    tail_report.stages.back().after.handles += row.live_handles;
            };
            if (retry)
            {
                const auto failed = session->readResources();
                assert(failed && state().failed == 1 && state().ready == 2 && provider->missing.load());
                const auto row = std::find_if((*failed)->rows.begin(), (*failed)->rows.end(), [](const auto& value) {
                    return value.state == sessions::ESceneResourceState::FAILED;
                });
                assert(row != (*failed)->rows.end() && row->asset_failure);
                assert(row->asset_failure->code == lux::process::asset_loading::EAssetLoadError::STORAGE_FAILURE);
                assert(row->asset_failure->storage_error == lux::asset::EAssetStorageError::NOT_FOUND);
                const auto old_key = row->key;
                std::printf("tail retry source=scene asset_error=%u storage_error=%u sequence=%llu\n",
                    unsigned(row->asset_failure->code), unsigned(row->asset_failure->storage_error), old_key.sequence);
                retry_key = old_key;
                tail_report.measure("retry", width, height, frames, step, drain, state, readback, poll_count);
                record_handles();
                assert(retry_requested);
                assert(row->key == old_key && row->state == sessions::ESceneResourceState::FAILED);
            }
            else
            {
                bool resized{};
                tail_report.measure("resize_down", 896, 512, frames, [&] {
                    if (!std::exchange(resized, true)) layout(true);
                    return step();
                }, drain, state, readback, poll_count);
                record_handles();
                resized = false;
                tail_report.measure("resize_restore", width, height, frames, [&] {
                    if (!std::exchange(resized, true)) layout(false);
                    return step();
                }, drain, state, readback, poll_count);
                record_handles();
                assert(tail_report.stages.back().sample.checksum == original_checksum);
            }
        }
        else
        {
            sample.begin(renderer->statistics().frames, poll_count);
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
            const auto measured_frames = renderer->statistics().frames;
            sample.wait([&] { sample.checksum = readback(); });
            assert(sample.checksum == original_checksum);
            assert(renderer->statistics().frames == measured_frames + 8);
            sample.finish(measured_frames);
        }
        const auto close_started = Clock::now();
        image = {};
        runtime = {};
        assert(workspace->beginClose() && session->beginClose());
        while (workspace || session)
        {
            poll();
            if (workspace && *workspace->advanceClose() == sessions::ECloseProgress::COMPLETE)
                workspace.reset();
            if (session && *session->advanceClose() == sessions::ECloseProgress::COMPLETE)
                session.reset();
        }
        assert(renderer->beginClose());
        while (*renderer->advanceClose() != rendering::ERenderClose::COMPLETE)
            assert(Clock::now() < deadline && renderer->poll(64));
        assert(renderer->joinStopped());
        const auto stats = renderer->statistics();
        assert(!stats.render_events && !stats.texture_misses && stats.descriptors_created == stats.descriptors_retired);
        assert(!stats.views && !stats.runtime_leases && !stats.accepted_frames);
        renderer.reset();
        assert(window->closeAfterRendererStopped());
        window.reset();
        (*endpoint)->requestStop();
        assert((*endpoint)->join());
        endpoint->reset();
        execution->requestStop();
        assert(execution->join());
        if (tails)
        {
            tail_report.closed_memory = processMemory();
            tail_report.close_wall = seconds(Clock::now() - close_started);
            tail_report.descriptors_created = stats.descriptors_created;
            tail_report.descriptors_retired = stats.descriptors_retired;
            tail_report.write(argv[2], "candidate", *provider);
        }
        else sample.write(argv[2], "candidate", seconds(Clock::now() - close_started));
        savePixels(argv[2], final_pixels);
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
}
