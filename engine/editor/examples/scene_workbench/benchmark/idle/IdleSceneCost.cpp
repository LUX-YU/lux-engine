#include "../CostSample.hpp"
#include <lux/engine/scene/Scene.hpp>
#include <lux/engine/simulation/ecs/Visual.hpp>
#include <lux/engine/editor/sessions/scene/detail/SceneResources.hpp>
#include "../../DevelopmentScene.hpp"
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
    assert(argc == 4);
    const auto count = std::stoull(argv[3]);
    assert(count == 64 || count == 1024);
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
        // Construct the complete input before relinquishing all Scene write aliases.
        source->scene->registry().clear();
        source->labels.clear();
        source->initial_selection.reset();
        source->resource_capacity = count;
        for (std::size_t i = 0; i != count; ++i)
        {
            const auto entity = source->scene->registry().create();
            source->scene->registry().emplace<lux::simulation::ecs::Mesh3D>(entity);
        }
        auto opened = sessions::SceneSession::openInspection(*source);
        assert(opened);
        auto session = std::move(*opened);
        std::uint64_t cycle{}, total_checksum{};
        const auto deadline = Clock::now() + std::chrono::seconds{90};
        const auto poll = [&]
        {
            assert(Clock::now() < deadline && execution->drainMain(64) && renderer->poll(64));
            assert(renderer->state() == rendering::ERendererState::READY);
        };
        const auto update = [&] { assert(session->updateAtOwnerSafePoint({++cycle, 0})); };
        update();
        const auto original = session->readResources();
        assert(original && (*original)->rows.size() == count);
        assert(std::all_of((*original)->rows.begin(), (*original)->rows.end(),
                           [](const auto &row) { return row.state == sessions::ESceneResourceState::UNREFERENCED; }));
        const auto finishCycle = [&]
        {
            const auto snapshot = session->readResources();
            assert(snapshot && snapshot->get() == original->get());
            assert((*snapshot)->revision == (*original)->revision);
            total_checksum += (*snapshot)->rows.size() + (*snapshot)->revision;
            assert(session->advanceScene({cycle, 0}));
            poll();
        };
        finishCycle();
        for (unsigned i = 0; i != warmup; ++i)
        {
            update();
            finishCycle();
        }
        double owner_wall{}, other_wall{};
        std::uint64_t owner_cycles{};
        for (unsigned i = 0; i != measured; ++i)
        {
            const auto started = Clock::now();
            const auto first_cycles = cycles();
            update();
            owner_cycles += cycles() - first_cycles;
            owner_wall += seconds(Clock::now() - started);
            const auto other_started = Clock::now();
            finishCycle();
            other_wall += seconds(Clock::now() - other_started);
        }
        assert(total_checksum == (warmup + measured + 1) * (count + (*original)->revision));
        const auto close_started = Clock::now();
        assert(session->beginClose());
        while (*session->advanceClose() != sessions::ECloseProgress::COMPLETE)
            poll();
        session.reset();
        assert(renderer->beginClose());
        while (*renderer->advanceClose() != rendering::ERenderClose::COMPLETE)
            assert(Clock::now() < deadline && renderer->poll(64));
        assert(renderer->joinStopped());
        const auto stats = renderer->statistics();
        assert(!stats.views && !stats.runtime_leases && !stats.accepted_frames && !stats.render_events);
        assert(stats.descriptors_created == stats.descriptors_retired);
        renderer.reset();
        assert(window->closeAfterRendererStopped());
        window.reset();
        (*endpoint)->requestStop();
        assert((*endpoint)->join());
        endpoint->reset();
        execution->requestStop();
        assert(execution->join());
        const auto close_wall = seconds(Clock::now() - close_started);
        using Request = sessions::detail::ResourceRequest;
        using MeshResult = sessions::detail::AssetResult<lux::asset::MeshAsset>;
        using MaterialResult = sessions::detail::AssetResult<lux::asset::MaterialAsset>;
        // Typed payload floor only: no claim about allocator metadata, shared control blocks, Registry,
        // Outline, labels, RenderSystem, driver memory, or process RSS. Snapshot is held through close.
        const auto payload_floor = count * (sizeof(Request) + sizeof(MeshResult) + sizeof(MaterialResult) +
                                            sizeof(std::unique_ptr<Request>) + sizeof(sessions::SceneResourceRow));
        std::ofstream out(argv[2]);
        out.precision(12);
        out << "{\n\"configuration\":\"RelWithDebInfo\",\n\"mode\":\"unreferenced-owner-idle\","
            << "\n\"mesh_count\":" << count << ",\n\"view_count\":0,\n\"warmup\":" << warmup
            << ",\n\"iterations\":" << measured << ",\n\"owner_update_seconds\":" << owner_wall
            << ",\n\"owner_update_cycles\":" << owner_cycles << ",\n\"advance_poll_seconds\":" << other_wall
            << ",\n\"close_seconds\":" << close_wall << ",\n\"checksum\":" << total_checksum
            << ",\n\"unchanged_snapshot\":true,\n\"association_map_lookups_per_cycle_by_source\":" << count
            << ",\n\"association_index_value_payload_floor_bytes\":"
            << count * sizeof(std::pair<const lux::simulation::ecs::Entity, Request*>)
            << ",\n\"association_index_bucket_and_node_overhead_included\":false"
            << ",\n\"resource_typed_payload_floor_bytes\":" << payload_floor
            << ",\n\"snapshot_payload_bytes_held_through_close\":" << count * sizeof(sessions::SceneResourceRow)
            << ",\n\"close_completed\":true\n}\n";
        assert(out.good());
    }
    lux::meta::ReflectionRegistry::destroyRegistry();
}
