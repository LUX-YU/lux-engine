#include "CostSample.hpp"
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
    assert(argc == 3);
    lux::meta::ReflectionRegistry::initRegistry();
    {
        lux::window::GlfwRuntime platform;
        assert(platform.valid());
        lux::object::ObjectMessageQueue messages;
        auto execution =
            lux::process::ExecutionRuntime::create({2, 64, 64, {64}, lux::process::BlockingSchedulerConfig{2, 64}});
        auto pak = lux::asset::PakAssetProvider::loadFromFile(argv[1]);
        assert(execution && pak);
        lux::asset::AssetVfs vfs;
        assert(vfs.mount({"/Seed", *pak, 0}) != lux::asset::kInvalidMountId);
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
        std::uint64_t cycle{};
        rendering::ViewImage image;
        rendering::EditorFramePacket pending;
        const auto deadline = Clock::now() + std::chrono::seconds{90};
        const auto poll = [&]
        {
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
            assert(window->beginFrame({{1600, 900}, float(delta), {1, 1}}) && window->drawPanes());
            assert(workspace->afterDraw(delta, {1, 1}) && session->advanceScene(update));
            auto snapshot = window->finishFrame();
            assert(snapshot);
            const auto images = workspace->frameImages();
            image = images.empty() ? rendering::ViewImage{} : images.front();
            if (!session->presentationPending())
            {
                auto sealed = renderer->sealFrame(*snapshot, images);
                assert(sealed);
                pending = std::move(*sealed);
                assert(renderer->trySubmitFrame(pending));
            }
            workspace->releaseFrameImages();
        };
        const auto drain = [&]
        {
            while (pending.valid())
            {
                poll();
                assert(renderer->trySubmitFrame(pending));
            }
            if (image.lease.valid())
                while (renderer->imageEvidence(image)->evidence == rendering::EImageEvidence::REQUESTED)
                {
                    poll();
                    std::this_thread::yield();
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
                            [](const auto &row) { return row.state == sessions::ESceneResourceState::READY; }) &&
                renderer->statistics().descriptors_created >= 2)
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds{8});
        }
        std::vector<std::byte> final_pixels;
        const auto readback = [&]
        {
            assert((image.extent == rendering::PixelExtent{width, height}));
            const auto *record = rendering::detail::ViewImageAccess::record(image);
            assert((record->camera.origin == std::array<double, 3>{6, 4, 8}));
            assert(record->wire_camera.view_matrix[12] == 0 && record->wire_camera.view_matrix[13] == 0 &&
                   record->wire_camera.view_matrix[14] == 0);
            std::vector<std::byte> pixels(std::size_t(width) * height * 4);
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
        Sample sample;
        sample.begin(renderer->statistics().frames);
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
        sample.write(argv[2], "candidate", seconds(Clock::now() - close_started));
        savePixels(argv[2], final_pixels);
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
}
