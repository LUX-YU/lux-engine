// ============================================================================
//  render_subsystem_lifecycle_probe.cpp — ECS/render create-reply ownership.
//
//  These probes deliberately split a frame into submit → dispatch → world
//  mutation → pump. A synchronous roundTrip() cannot expose the dangerous
//  interval in which the renderer has created an object but the ECS intent has
//  already left or been replaced. Every successful late reply must therefore
//  remain observable long enough to emit its compensating remove/release.
// ============================================================================

#include "HeadlessBridgeFixture.hpp"

#include <lux/engine/ecs/components/ResolvedTransform2DComponent.hpp>
#include <lux/engine/ecs/components/ResolvedTransform3DComponent.hpp>
#include <lux/engine/ecs/render/RenderExtractionResources.hpp>
#include <lux/engine/ecs/render/SceneRenderBinding.hpp>
#include <lux/engine/ecs/render/RenderSpatialTransform.hpp>
#include <lux/engine/ecs/render/components/MeshGpuCacheComponent.hpp>
#include <lux/engine/ecs/render/components/TextureGpuCacheComponent.hpp>
#include <lux/engine/ecs/render/components/PrimaryCameraTag.hpp>
#include <lux/engine/ecs/render/components/2d/Camera2DComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Camera2DCacheComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Image2DComponent.hpp>
#include <lux/engine/ecs/render/components/2d/Order2DComponents.hpp>
#include <lux/engine/ecs/tilemap/components/TilemapComponent.hpp>
#include <lux/engine/ecs/tilemap/components/TilemapBindingComponent.hpp>
#include <lux/engine/ecs/tilemap/systems/TilemapRuntime.hpp>
#include <lux/engine/ecs/pixel/components/PixelField2DComponent.hpp>
#include <lux/engine/ecs/pixel/components/PixelFieldBindingComponent.hpp>
#include <lux/engine/ecs/pixel/systems/PixelFieldRuntime.hpp>
#include <lux/engine/ecs/render/subsystems/2d/Image2DSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/2d/PixelField2DSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/2d/Tilemap2DSubsystem.hpp>
#include <lux/engine/ecs/render/subsystems/3d/MeshSubsystems.hpp>
#include <lux/engine/meta/LuxObject.hpp>
#include <lux/engine/function/render/client/core/RenderErrorRegistry.hpp>

#include <cstdio>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace
{
    constexpr float kDt = 1.0f / 60.0f;
    int g_failures = 0;

    struct DiagnosticCapture
    {
        DiagnosticCapture()
        {
            lux::ecs::setRenderBridgeDiagnosticSink(
                [this](std::string_view message)
                {
                    messages.emplace_back(message);
                }
            );
        }

        ~DiagnosticCapture()
        {
            lux::ecs::setRenderBridgeDiagnosticSink({});
        }

        [[nodiscard]] std::size_t count(std::string_view needle) const
        {
            std::size_t result = 0;
            for (const auto& message : messages)
                if (message.find(needle) != std::string::npos)
                    ++result;
            return result;
        }

        std::vector<std::string> messages;
    };

    void check(bool condition, const char* message)
    {
        std::fprintf(stderr, "[%s] %s\n", condition ? " ok " : "FAIL", message);
        if (!condition)
            ++g_failures;
    }

    template <class Payload>
    [[nodiscard]] std::optional<Payload> recordedPayload(
        const lux::bridgetest::Recorder& recorder,
        const char* operation,
        std::size_t index = 0u)
    {
        if (recorder.count(operation) <= index)
            return std::nullopt;
        return recorder.payload<Payload>(operation, index);
    }

    template <class System>
    void update(
        System& system,
        lux::meta::EntityRegistry& registry,
        lux::bridgetest::HeadlessBridgeFixture& fixture,
        lux::ecs::SceneRenderBinding& binding,
        lux::ecs::ActiveRenderView* supplied_view = nullptr)
    {
        lux::ecs::ActiveRenderView fallback_view{fixture.view()};
        auto& active_view = supplied_view ? *supplied_view : fallback_view;
        lux::ecs::RenderSubsystemContext context{
            registry, {}, binding, active_view, kDt, 0};
        system.prepare(context);
        system.update(context);
    }

    template <class System>
    void frame(
        System& system,
        lux::meta::EntityRegistry& registry,
        lux::bridgetest::HeadlessBridgeFixture& fixture,
        lux::ecs::SceneRenderBinding& binding,
        lux::ecs::ActiveRenderView* supplied_view = nullptr)
    {
        update(system, registry, fixture, binding, supplied_view);
        fixture.roundTrip();
    }

    template <class System>
    void frame(
        System& system,
        lux::meta::EntityRegistry& registry,
        lux::bridgetest::HeadlessBridgeFixture& fixture)
    {
        lux::ecs::SceneRenderBinding binding{
            fixture.session(), fixture.control(),
            fixture.uploadClientForTest(), fixture.scene()};
        binding.setCatalog(fixture.features());
        frame(system, registry, fixture, binding);
    }

    template <class System>
    void close(
        System& system,
        lux::meta::EntityRegistry& registry,
        lux::bridgetest::HeadlessBridgeFixture& fixture,
        lux::ecs::SceneRenderBinding& binding)
    {
        lux::ecs::ActiveRenderView active_view{fixture.view()};
        lux::ecs::RenderSubsystemContext context{
            registry, {}, binding, active_view, kDt, 0};
        system.prepare(context);
        system.close(context);
    }

    lux::meta::entity_id makeImage(lux::meta::EntityRegistry& registry)
    {
        const auto entity = registry.create();
        registry.emplace<lux::ecs::Image2DComponent>(entity);
        registry.emplace<lux::ecs::ResolvedTransform2DComponent>(entity);
        return entity;
    }

    lux::meta::entity_id makeMesh(lux::meta::EntityRegistry& registry)
    {
        const auto entity = registry.create();
        auto& mesh = registry.emplace<lux::ecs::MeshComponent>(entity);
        mesh.visible = true;

        registry.emplace<lux::ecs::ResolvedTransform3DComponent>(entity);

        registry.emplace<lux::ecs::MeshGpuCacheComponent>(
            entity,
            lux::ecs::MeshGpuCacheComponent{
                lux::render::RMeshHandle{1u, 1u},
                lux::render::RMaterialHandle{1u, 1u},
                {},
                {}
            }
        );
        return entity;
    }

    lux::meta::entity_id makeTilemap(
        lux::meta::EntityRegistry& registry,
        lux::ecs::TilemapRuntime& runtime)
    {
        const auto id = lux::ecs::TilemapId{
            uuids::uuid::from_string(
                "70000000-0000-4000-8000-000000000001").value()};
        const auto handle = runtime.create({id});
        lux::ecs::TileChunkLoad chunk;
        chunk.coordinate = {-1, 0};
        chunk.tiles.assign(
            lux::ecs::TilemapRuntime::kChunkTileCount,
            lux::rdesc::kEmptyTile);
        (void)runtime.loadChunk(handle, std::move(chunk));

        const auto entity = registry.create();
        auto& tilemap = registry.emplace<lux::ecs::TilemapComponent>(entity);
        tilemap.id = id;
        registry.emplace<lux::ecs::TilemapBindingComponent>(
            entity,
            lux::ecs::TilemapBindingComponent{handle});
        tilemap.tileset_cols = 2;
        tilemap.tileset_rows = 2;

        registry.emplace<lux::ecs::ResolvedTransform2DComponent>(entity);
        registry.emplace<lux::ecs::TextureGpuCacheComponent>(
            entity,
            lux::ecs::TextureGpuCacheComponent{
                lux::render::RTextureHandle{100u, 1u},
                {}
            }
        );
        return entity;
    }

    lux::meta::entity_id makePixelField(
        lux::meta::EntityRegistry& registry,
        lux::ecs::PixelFieldHandle field)
    {
        const auto entity = registry.create();
        auto& component =
            registry.emplace<lux::ecs::PixelField2DComponent>(entity);
        registry.emplace<lux::ecs::PixelFieldBindingComponent>(
            entity,
            lux::ecs::PixelFieldBindingComponent{field, false});
        component.cell_size = 1.f;

        registry.emplace<lux::ecs::ResolvedTransform2DComponent>(entity);
        return entity;
    }

    [[nodiscard]] lux::ecs::PixelFieldHandle makeRuntimeField(
        lux::ecs::PixelFieldRuntime& runtime,
        const char* id)
    {
        const lux::ecs::PixelFieldDesc description{
            lux::ecs::PixelFieldId{
                uuids::uuid::from_string(id).value()},
            lux::ecs::EPixelFieldExtent::BOUNDED,
            {{0, 0}, {0, 0}},
            0u};
        const auto field = runtime.create(description);
        lux::ecs::PixelChunkLoad chunk;
        chunk.coordinate = {0, 0};
        chunk.materials.assign(
            lux::ecs::PixelFieldRuntime::kChunkCellCount,
            lux::ecs::kEmptyMaterial);
        chunk.presentation_active = true;
        chunk.simulation_active = true;
        (void)runtime.loadChunk(field, std::move(chunk));
        return field;
    }

    void testImageDestroyBeforePump()
    {
        lux::bridgetest::HeadlessBridgeFixture fixture;
        lux::meta::EntityRegistry registry;
        fixture.registerCanvas2DOps();

        lux::ecs::SceneRenderBinding binding{
            fixture.session(), fixture.control(),
            fixture.uploadClientForTest(), fixture.scene()};
        binding.setCatalog(fixture.features());
        lux::ecs::Image2DSubsystem images{};
        images.onAdded(lux::ecs::SystemSetupContext{registry, {}});
        fixture.beginFrame();

        const auto entity = makeImage(registry);
        update(images, registry, fixture, binding);
        fixture.submit();
        fixture.dispatch();
        check(fixture.recorder().created_images.size() == 1,
              "Image/destroy-before-pump: server created one image");
        const auto created = fixture.recorder().created_images.empty()
            ? lux::render::Image2DHandle{}
            : fixture.recorder().created_images.front();

        registry.destroy(entity);
        check(fixture.recorder().removed_images.empty(),
              "Image/destroy-before-pump: observer emits no render command");
        fixture.pump();
        fixture.beginFrame();

        frame(images, registry, fixture);
        check(fixture.recorder().removed_images.size() == 1 &&
                  fixture.recorder().removed_images.front() == created,
              "Image/destroy-before-pump: late handle is removed at update safe point");
        frame(images, registry, fixture);
        check(fixture.recorder().removed_images.size() == 1,
              "Image/destroy-before-pump: compensation is exactly once");
    }

    void testImageLeaveAndReenterBeforePump()
    {
        lux::bridgetest::HeadlessBridgeFixture fixture;
        lux::meta::EntityRegistry registry;
        fixture.registerCanvas2DOps();

        lux::ecs::SceneRenderBinding binding{
            fixture.session(), fixture.control(),
            fixture.uploadClientForTest(), fixture.scene()};
        binding.setCatalog(fixture.features());
        lux::ecs::Image2DSubsystem images{};
        images.onAdded(lux::ecs::SystemSetupContext{registry, {}});
        fixture.beginFrame();

        const auto entity = makeImage(registry);
        update(images, registry, fixture, binding);
        fixture.submit();
        fixture.dispatch();
        check(fixture.recorder().created_images.size() == 1,
              "Image/reenter: first intent reached the server");
        const auto first = fixture.recorder().created_images.empty()
            ? lux::render::Image2DHandle{}
            : fixture.recorder().created_images.front();

        registry.remove<lux::ecs::Image2DComponent>(entity);
        registry.emplace<lux::ecs::Image2DComponent>(entity);
        fixture.pump();
        fixture.beginFrame();

        frame(images, registry, fixture);
        check(fixture.recorder().removed_images.size() == 1 &&
                  fixture.recorder().removed_images.front() == first,
              "Image/reenter: stale first reply is compensated");
        check(fixture.recorder().created_images.size() == 2,
              "Image/reenter: replacement intent creates a fresh instance");
        const auto second = fixture.recorder().created_images.size() > 1
            ? fixture.recorder().created_images[1]
            : lux::render::Image2DHandle{};
        check(second != first,
              "Image/reenter: replacement does not adopt the stale handle");

        // Publish the second completion, then prove normal teardown still owns it.
        frame(images, registry, fixture);
        registry.destroy(entity);
        frame(images, registry, fixture);
        check(fixture.recorder().removed_images.size() == 2 &&
                  fixture.recorder().removed_images[1] == second,
              "Image/reenter: replacement instance has one normal owner");
    }

    void testImageRetryableDispatchThenPermanentReply()
    {
        DiagnosticCapture diagnostics;
        lux::bridgetest::HeadlessBridgeFixture fixture;
        lux::meta::EntityRegistry registry;
        fixture.registerCanvas2DOps();
        fixture.recorder().add_image_dispatch_error =
            lux::render::renderError<
                lux::render::err::memory::CapacityExhausted>();

        lux::ecs::SceneRenderBinding binding{
            fixture.session(), fixture.control(),
            fixture.uploadClientForTest(), fixture.scene()};
        binding.setCatalog(fixture.features());
        lux::ecs::Image2DSubsystem images{};
        images.onAdded(lux::ecs::SystemSetupContext{registry, {}});
        fixture.beginFrame();
        (void)makeImage(registry);

        frame(images, registry, fixture);   // dispatch failure reply
        frame(images, registry, fixture);   // classify + start backoff
        for (int i = 0; i < 10; ++i)
            frame(images, registry, fixture);
        check(fixture.recorder().count("AddImage2D") == 1,
              "Image/dispatch-retry: retryable error does not hot-loop");

        fixture.recorder().add_image_dispatch_error = {};
        fixture.recorder().add_image_status =
            lux::render::ECanvas2DCreateStatus::InvalidConfiguration;
        for (int i = 0;
             i < 130 && fixture.recorder().count("AddImage2D") < 2;
             ++i)
        {
            frame(images, registry, fixture);
        }
        frame(images, registry, fixture);   // consume the permanent reply
        for (int i = 0; i < 10; ++i)
            frame(images, registry, fixture);

        check(fixture.recorder().count("AddImage2D") == 2,
              "Image/dispatch-retry: bounded retry reaches the server once");
        check(diagnostics.count("Image2DSubsystem") == 2,
              "Image/failure-closure: dispatch and permanent reply report once each");
    }

    void testMeshDestroyBeforePump()
    {
        lux::bridgetest::HeadlessBridgeFixture fixture;
        lux::meta::EntityRegistry registry;
        fixture.registerMeshStackOps();

        lux::ecs::ActiveRenderView active_view{fixture.view()};
        lux::ecs::SceneRenderBinding binding{
            fixture.session(), fixture.control(),
            fixture.uploadClientForTest(), fixture.scene()};
        binding.setCatalog(fixture.features());
        lux::ecs::MeshSubsystem meshes{};
        meshes.onAdded(lux::ecs::SystemSetupContext{registry, {}});
        fixture.beginFrame();

        const auto entity = makeMesh(registry);
        update(meshes, registry, fixture, binding, &active_view);
        fixture.submit();
        fixture.dispatch();
        check(fixture.recorder().created_objects.size() == 1,
              "Mesh/destroy-before-pump: server created one object");
        const auto created = fixture.recorder().created_objects.empty()
            ? lux::render::RenderObjectHandle{}
            : fixture.recorder().created_objects.front();

        registry.destroy(entity);
        check(fixture.recorder().count("RemoveMeshInstance") == 0,
              "Mesh/destroy-before-pump: observer emits no render command");
        fixture.pump();
        fixture.beginFrame();

        frame(meshes, registry, fixture, binding, &active_view);
        check(fixture.recorder().count("RemoveMeshInstance") == 1,
              "Mesh/destroy-before-pump: late object is removed");
        if (fixture.recorder().count("RemoveMeshInstance") == 1)
        {
            const auto removed = fixture.recorder().payload<
                lux::render::RemoveMeshInstancePayload>(
                    "RemoveMeshInstance",
                    0
                );
            check(removed.object == created,
                  "Mesh/destroy-before-pump: compensation targets returned object");
        }
    }

    void testMeshRetryableDispatchThenPermanentReply()
    {
        DiagnosticCapture diagnostics;
        lux::bridgetest::HeadlessBridgeFixture fixture;
        lux::meta::EntityRegistry registry;
        fixture.registerMeshStackOps();
        fixture.recorder().add_instance_dispatch_error =
            lux::render::renderError<
                lux::render::err::memory::CapacityExhausted>();

        lux::ecs::ActiveRenderView active_view{fixture.view()};
        lux::ecs::SceneRenderBinding binding{
            fixture.session(), fixture.control(),
            fixture.uploadClientForTest(), fixture.scene()};
        binding.setCatalog(fixture.features());
        lux::ecs::MeshSubsystem meshes{};
        meshes.onAdded(lux::ecs::SystemSetupContext{registry, {}});
        fixture.beginFrame();
        (void)makeMesh(registry);

        frame(meshes, registry, fixture, binding, &active_view);
        frame(meshes, registry, fixture, binding, &active_view);
        for (int i = 0; i < 10; ++i)
            frame(meshes, registry, fixture, binding, &active_view);
        check(fixture.recorder().count("AddMeshInstance") == 1,
              "Mesh/dispatch-retry: retryable error does not hot-loop");

        fixture.recorder().add_instance_dispatch_error = {};
        fixture.recorder().add_instance_status =
            lux::render::MeshInstanceCreateStatus::InvalidConfiguration;
        for (int i = 0;
             i < 130 && fixture.recorder().count("AddMeshInstance") < 2;
             ++i)
        {
            frame(meshes, registry, fixture, binding, &active_view);
        }
        frame(meshes, registry, fixture, binding, &active_view);
        for (int i = 0; i < 10; ++i)
            frame(meshes, registry, fixture, binding, &active_view);

        check(fixture.recorder().count("AddMeshInstance") == 2,
              "Mesh/dispatch-retry: bounded retry reaches the server once");
        check(diagnostics.count("MeshInstanceSubsystem") == 2,
              "Mesh/failure-closure: dispatch and permanent reply report once each");
    }

    void testSparseTilemapAtlasLifecycle()
    {
        lux::bridgetest::HeadlessBridgeFixture fixture;
        lux::meta::EntityRegistry registry;
        lux::ecs::TilemapRuntime runtime;
        fixture.registerCanvas2DOps();
        fixture.registerPersistentTextureOps();

        const auto entity = makeTilemap(registry, runtime);
        lux::ecs::SceneRenderBinding binding{
            fixture.session(),
            fixture.control(),
            fixture.uploadClientForTest(),
            fixture.scene()};
        binding.setCatalog(fixture.features());
        lux::ecs::Tilemap2DSubsystem tilemaps{&runtime};
        tilemaps.onAdded(lux::ecs::SystemSetupContext{registry, {}});
        fixture.beginFrame();

        frame(tilemaps, registry, fixture, binding); // shared atlas
        frame(tilemaps, registry, fixture, binding); // chunk instance
        frame(tilemaps, registry, fixture, binding); // full chunk upload
        check(
            fixture.recorder().created_textures.size() == 1u,
            "Tile/sparse: all chunks share one fixed index atlas");
        check(
            fixture.recorder().created_tiles.size() == 1u &&
                tilemaps.residentChunks() == 1u,
            "Tile/sparse: only the resident negative-coordinate chunk is mirrored");
        if (!fixture.recorder().created_tiles.empty())
        {
            const auto payload = fixture.recorder().payload<
                lux::render::AddTile2DPayload>("AddTile2D", 0u);
            check(
                payload.data.tiles_w == 256u &&
                    payload.data.tiles_h == 256u,
                "Tile/sparse: canvas instance is one logical 256x256 chunk");
        }

        const auto handle =
            registry.get<lux::ecs::TilemapBindingComponent>(entity).runtime;
        check(
            runtime.setTile(handle, {-1, 0}, 3u),
            "Tile/sparse: signed world-cell edits reach the resident chunk");
        const auto uploads_before =
            fixture.recorder().count("UpdateTextureRegions");
        frame(tilemaps, registry, fixture, binding);
        frame(tilemaps, registry, fixture, binding);
        check(
            fixture.recorder().count("UpdateTextureRegions") > uploads_before,
            "Tile/sparse: a local edit uploads a bounded atlas region");

        registry.destroy(entity);
        frame(tilemaps, registry, fixture, binding);
        check(
            tilemaps.freeSlots() == 64u,
            "Tile/sparse: entity teardown returns its atlas slot exactly once");
    }

    void testCanvasLargePositionWire()
    {
        lux::bridgetest::HeadlessBridgeFixture fixture;
        lux::meta::EntityRegistry registry;
        lux::ecs::TilemapRuntime tile_runtime;
        lux::ecs::PixelFieldRuntime pixel_runtime;
        fixture.registerCanvas2DOps();
        fixture.registerPersistentTextureOps();

        constexpr lux::spatial::GridCoord2i64 kFarTile{
            1'000'000'000,
            -1'000'000'000};
        constexpr lux::spatial::Position2D kFarPosition{
            static_cast<double>(kFarTile.x) *
                lux::ecs::kRenderSpatialTileSize,
            static_cast<double>(kFarTile.y) *
                lux::ecs::kRenderSpatialTileSize};
        const auto camera = registry.create();
        registry.emplace<lux::ecs::PrimaryCameraTag>(camera);
        registry.emplace<lux::ecs::Camera2DComponent>(camera);
        auto& camera_world =
            registry.emplace<lux::ecs::ResolvedTransform2DComponent>(camera);
        camera_world.position = kFarPosition;
        // This probe isolates render-subsystem lifecycle behavior, so the
        // ordinary Camera2DSystem is intentionally absent. Supply the derived
        // cache fact it would have published before the render phase.
        auto& camera_cache =
            registry.emplace<lux::ecs::Camera2DCacheComponent>(camera);
        camera_cache.render_origin = kFarPosition;

        const auto image = makeImage(registry);
        registry.get<lux::ecs::ResolvedTransform2DComponent>(image).position =
            kFarPosition;
        const auto tile = makeTilemap(registry, tile_runtime);
        registry.get<lux::ecs::ResolvedTransform2DComponent>(tile).position =
            kFarPosition;
        auto& y_sort = registry.emplace<lux::ecs::YSort2DComponent>(tile);
        y_sort.scale = 1.0e-9f;
        const auto field = makeRuntimeField(
            pixel_runtime,
            "10000000-0000-4000-8000-0000000000f0");
        const auto pixel = makePixelField(registry, field);
        registry.get<lux::ecs::ResolvedTransform2DComponent>(pixel).position =
            kFarPosition;

        lux::ecs::SceneRenderBinding binding{
            fixture.session(),
            fixture.control(),
            fixture.uploadClientForTest(),
            fixture.scene()};
        binding.setCatalog(fixture.features());
        lux::ecs::Image2DSubsystem images;
        lux::ecs::Tilemap2DSubsystem tilemaps{&tile_runtime};
        lux::ecs::PixelField2DSubsystem pixels{&pixel_runtime};
        images.onAdded(lux::ecs::SystemSetupContext{registry, {}});
        tilemaps.onAdded(lux::ecs::SystemSetupContext{registry, {}});
        pixels.onAdded(lux::ecs::SystemSetupContext{registry, {}});
        fixture.beginFrame();

        for (int attempt = 0; attempt != 8; ++attempt)
        {
            update(images, registry, fixture, binding);
            update(tilemaps, registry, fixture, binding);
            update(pixels, registry, fixture, binding);
            fixture.roundTrip();
        }

        const auto image_payload = recordedPayload<
            lux::render::AddImage2DPayload>(
                fixture.recorder(), "AddImage2D");
        check(
            image_payload &&
                image_payload->data.page_delta[0] == kFarTile.x &&
                image_payload->data.page_delta[1] == kFarTile.y,
            "Canvas/large-position: image keeps exact signed page delta");

        const auto tile_payload = recordedPayload<
            lux::render::AddTile2DPayload>(
                fixture.recorder(), "AddTile2D");
        check(
            tile_payload &&
                tile_payload->data.page_delta[0] == kFarTile.x - 1 &&
                tile_payload->data.page_delta[1] == kFarTile.y &&
                tile_payload->data.m[4] > 0.0f,
            "Canvas/large-position: signed tile chunk is canonically split");
        check(
            tile_payload && tile_payload->priority > 1000.0f,
            "Canvas/y-sort: priority uses absolute Y across the old 1024 boundary");

        const auto pixel_payload = recordedPayload<
            lux::render::AddPixelField2DPayload>(
                fixture.recorder(), "AddPixelField2D");
        check(
            pixel_payload &&
                pixel_payload->data.page_delta[0] == kFarTile.x &&
                pixel_payload->data.page_delta[1] == kFarTile.y,
            "Canvas/large-position: pixel chunk keeps exact signed page delta");
    }

    void testPixelFieldSwapKeepsPendingSlotOwned()
    {
        lux::bridgetest::HeadlessBridgeFixture fixture;
        lux::meta::EntityRegistry registry;
        lux::ecs::PixelFieldRuntime runtime;
        fixture.registerCanvas2DOps();
        fixture.registerPersistentTextureOps();

        const auto first_field = makeRuntimeField(
            runtime, "10000000-0000-4000-8000-000000000001");
        const auto second_field = makeRuntimeField(
            runtime, "10000000-0000-4000-8000-000000000002");
        const auto entity = makePixelField(registry, first_field);

        lux::ecs::SceneRenderBinding binding{
            fixture.session(), fixture.control(),
            fixture.uploadClientForTest(), fixture.scene()};
        binding.setCatalog(fixture.features());
        lux::ecs::PixelField2DSubsystem pixels{&runtime};
        pixels.onAdded(lux::ecs::SystemSetupContext{registry, {}});
        fixture.beginFrame();

        frame(pixels, registry, fixture);   // atlas + palette texture replies
        frame(pixels, registry, fixture);   // palette upload reply

        // The first AddPixelField has reached the server, but its reply remains
        // queued. Open the next frame without pumping so the replacement must
        // coexist with that abandoned generation.
        update(pixels, registry, fixture, binding);
        fixture.submit();
        fixture.dispatch();
        check(fixture.recorder().created_pixels.size() == 1,
              "Pixel/field-generation: first chunk instance reached the server");
        const auto stale_instance = fixture.recorder().created_pixels.front();
        const auto first_add = fixture.recorder().payload<
            lux::render::AddPixelField2DPayload>("AddPixelField2D", 0);

        registry.patch<lux::ecs::PixelFieldBindingComponent>(
            entity,
            [second_field](auto& binding) { binding.field = second_field; });
        fixture.beginFrame();
        update(pixels, registry, fixture, binding);
        check(pixels.freeSlots() == 62,
              "Pixel/field-generation: pending old slot is not returned early");
        fixture.submit();
        fixture.dispatch();
        check(fixture.recorder().created_pixels.size() == 2,
              "Pixel/field-generation: replacement starts before old reply lands");
        const auto replacement_instance = fixture.recorder().created_pixels[1];
        const auto second_add = fixture.recorder().payload<
            lux::render::AddPixelField2DPayload>("AddPixelField2D", 1);
        check(first_add.data.atlas_x != second_add.data.atlas_x ||
                  first_add.data.atlas_y != second_add.data.atlas_y,
              "Pixel/field-generation: replacement cannot reuse pending atlas slot");

        fixture.pump();
        fixture.beginFrame();
        frame(pixels, registry, fixture);
        check(fixture.recorder().removed_pixels.size() == 1 &&
                  fixture.recorder().removed_pixels.front() == stale_instance,
              "Pixel/field-generation: abandoned late instance is removed");
        check(pixels.freeSlots() == 63,
              "Pixel/field-generation: old slot returns only after completion");

        registry.destroy(entity);
        frame(pixels, registry, fixture);
        check(fixture.recorder().removed_pixels.size() == 2 &&
                  fixture.recorder().removed_pixels[1] == replacement_instance,
              "Pixel/field-generation: replacement has one normal owner");
        check(pixels.freeSlots() == 64,
              "Pixel/field-generation: teardown returns every atlas slot exactly once");
    }

    void testPixelFieldSwapKeepsUploadingSlotOwned()
    {
        lux::bridgetest::HeadlessBridgeFixture fixture;
        lux::meta::EntityRegistry registry;
        lux::ecs::PixelFieldRuntime runtime;
        fixture.registerCanvas2DOps();
        fixture.registerPersistentTextureOps();

        const auto first_field = makeRuntimeField(
            runtime, "20000000-0000-4000-8000-000000000001");
        const auto second_field = makeRuntimeField(
            runtime, "20000000-0000-4000-8000-000000000002");
        const auto entity = makePixelField(registry, first_field);

        lux::ecs::SceneRenderBinding binding{
            fixture.session(), fixture.control(),
            fixture.uploadClientForTest(), fixture.scene()};
        binding.setCatalog(fixture.features());
        lux::ecs::PixelField2DSubsystem pixels{&runtime};
        pixels.onAdded(lux::ecs::SystemSetupContext{registry, {}});
        fixture.beginFrame();

        frame(pixels, registry, fixture);   // atlas + palette create
        frame(pixels, registry, fixture);   // palette upload
        frame(pixels, registry, fixture);   // first chunk create

        // Adopt the first instance and dispatch its initial full-slot upload,
        // but deliberately keep that upload reply queued.
        update(pixels, registry, fixture, binding);
        fixture.submit();
        fixture.dispatch();

        registry.patch<lux::ecs::PixelFieldBindingComponent>(
            entity,
            [second_field](auto& binding) { binding.field = second_field; });
        fixture.beginFrame();
        update(pixels, registry, fixture, binding);
        check(pixels.freeSlots() == 62,
              "Pixel/upload-ABA: replacement reserves a different slot");
        fixture.submit();
        fixture.dispatch();

        // The next safe point retires the old Live. Its instance can be
        // removed now, but its atlas coordinates remain leased until the old
        // region-upload acknowledgement lands.
        fixture.beginFrame();
        update(pixels, registry, fixture, binding);
        check(pixels.freeSlots() == 62,
              "Pixel/upload-ABA: old slot is not reusable while upload is in flight");
        fixture.submit();
        fixture.dispatch();
        fixture.pump();

        fixture.beginFrame();
        update(pixels, registry, fixture, binding);
        check(pixels.freeSlots() == 63,
              "Pixel/upload-ABA: old slot returns after its final upload settles");

        registry.destroy(entity);
        frame(pixels, registry, fixture);
        frame(pixels, registry, fixture);
        check(pixels.freeSlots() == 64,
              "Pixel/upload-ABA: replacement upload also releases exactly once");
        check(pixels.slotProtocolErrors() == 0,
              "Pixel/upload-ABA: slot accounting never underflows");
    }

    void testPixelSlotWaitsForConcurrentUploads()
    {
        lux::bridgetest::HeadlessBridgeFixture fixture;
        lux::meta::EntityRegistry registry;
        lux::ecs::PixelFieldRuntime runtime;
        fixture.registerCanvas2DOps();
        fixture.registerPersistentTextureOps();

        const auto first_field = makeRuntimeField(
            runtime, "30000000-0000-4000-8000-000000000001");
        const auto second_field = makeRuntimeField(
            runtime, "30000000-0000-4000-8000-000000000002");
        const auto entity = makePixelField(registry, first_field);

        lux::ecs::SceneRenderBinding binding{
            fixture.session(), fixture.control(),
            fixture.uploadClientForTest(), fixture.scene()};
        binding.setCatalog(fixture.features());
        lux::ecs::PixelField2DSubsystem pixels{&runtime};
        pixels.onAdded(lux::ecs::SystemSetupContext{registry, {}});
        fixture.beginFrame();

        frame(pixels, registry, fixture);   // shared textures
        frame(pixels, registry, fixture);   // palette upload
        frame(pixels, registry, fixture);   // chunk create

        // Keep the initial full-slot upload reply queued.
        update(pixels, registry, fixture, binding);
        fixture.submit();
        fixture.dispatch();

        auto* const ledger = runtime.dirtyLedger(first_field, {0, 0});
        if (ledger)
            ledger->markDirty(0u, 0u, 1u, 1u);

        // Issue a second upload for the same slot before either reply reaches
        // the request owner.
        fixture.beginFrame();
        update(pixels, registry, fixture, binding);
        fixture.submit();
        fixture.dispatch();
        check(
            ledger != nullptr &&
                fixture.recorder().count("UpdateTextureRegions") >= 3,
            "Pixel/upload-count: two updates can be in flight for one slot"
        );

        registry.patch<lux::ecs::PixelFieldBindingComponent>(
            entity,
            [second_field](auto& binding) { binding.field = second_field; });
        fixture.beginFrame();
        update(pixels, registry, fixture, binding);
        check(
            pixels.freeSlots() == 62,
            "Pixel/upload-count: retired slot stays owned while both replies wait"
        );
        fixture.submit();
        fixture.dispatch();

        // Both acknowledgements arrive together. The counter reaches zero
        // once, so the retired slot is returned exactly once.
        fixture.pump();
        fixture.beginFrame();
        update(pixels, registry, fixture, binding);
        check(
            pixels.freeSlots() == 63,
            "Pixel/upload-count: slot returns only after every update settles"
        );
        check(
            pixels.slotProtocolErrors() == 0,
            "Pixel/upload-count: concurrent completions preserve counter invariants"
        );

        registry.destroy(entity);
        frame(pixels, registry, fixture);
        frame(pixels, registry, fixture);
        check(
            pixels.freeSlots() == 64,
            "Pixel/upload-count: replacement teardown returns the final slot once"
        );
    }

    void testPixelSubsystemCancelRestoresDirtyExport()
    {
        lux::bridgetest::HeadlessBridgeFixture fixture;
        lux::meta::EntityRegistry registry;
        lux::ecs::PixelFieldRuntime runtime;
        fixture.registerCanvas2DOps();
        fixture.registerPersistentTextureOps();

        const auto field = makeRuntimeField(
            runtime, "40000000-0000-4000-8000-000000000001");
        (void)makePixelField(registry, field);
        auto* ledger = runtime.dirtyLedger(field, {0, 0});

        lux::ecs::SceneRenderBinding binding{
            fixture.session(), fixture.control(),
            fixture.uploadClientForTest(), fixture.scene()};
        binding.setCatalog(fixture.features());
        fixture.beginFrame();
        {
            lux::ecs::PixelField2DSubsystem pixels{&runtime};
            pixels.onAdded(lux::ecs::SystemSetupContext{registry, {}});
            frame(pixels, registry, fixture);   // atlas + palette textures
            frame(pixels, registry, fixture);   // palette upload
            frame(pixels, registry, fixture);   // chunk instance reply

            // Adopt the chunk and take the initial full-content export, but do
            // not submit it. Destruction must return that ledger ticket.
            update(pixels, registry, fixture, binding);
            check(ledger != nullptr && !ledger->hasDirty(),
                  "Pixel/upload-cancel: submitted export is held in flight");
            close(pixels, registry, fixture, binding);
        }

        check(ledger != nullptr && ledger->hasDirty(),
              "Pixel/upload-cancel: subsystem teardown restores unacknowledged dirt");
        const auto retry = runtime.exportDirty(
            field,
            {0, 0},
            lux::ecs::PixelExportBudget{0u, 0u}
        );
        check(!retry.empty(),
              "Pixel/upload-cancel: a future bridge can export the restored region");
        if (!retry.empty())
            runtime.confirmExport(
                field,
                {0, 0},
                retry.content_revision,
                false);

        fixture.roundTrip();
        check(fixture.recorder().destroyed_textures.size() == 2,
              "Pixel/upload-cancel: bridge-owned atlas and palette are released");
    }

    void testPixelSharedTextureCapacityUsesBoundedRetry()
    {
        lux::bridgetest::HeadlessBridgeFixture fixture;
        lux::meta::EntityRegistry registry;
        lux::ecs::PixelFieldRuntime runtime;
        fixture.registerCanvas2DOps();
        fixture.registerPersistentTextureOps();

        const auto field = makeRuntimeField(
            runtime, "50000000-0000-4000-8000-000000000001");
        (void)makePixelField(registry, field);
        fixture.recorder().persistent_texture_create_status =
            static_cast<std::uint32_t>(
                lux::render::ERegionUploadStatus::CapacityExhausted
            );

        lux::ecs::SceneRenderBinding binding{
            fixture.session(), fixture.control(),
            fixture.uploadClientForTest(), fixture.scene()};
        binding.setCatalog(fixture.features());
        lux::ecs::PixelField2DSubsystem pixels{&runtime};
        pixels.onAdded(lux::ecs::SystemSetupContext{registry, {}});
        fixture.beginFrame();

        frame(pixels, registry, fixture);
        frame(pixels, registry, fixture);
        const auto first_attempts =
            fixture.recorder().count("CreatePersistentTexture2D");
        for (int i = 0; i < 10; ++i)
            frame(pixels, registry, fixture);
        check(first_attempts == 2 &&
                  fixture.recorder().count("CreatePersistentTexture2D") == 2,
              "Pixel/texture-capacity: failure does not hot-loop every frame");

        fixture.recorder().persistent_texture_create_status = 0;
        for (int i = 0; i < 125; ++i)
            frame(pixels, registry, fixture);
        check(fixture.recorder().count("CreatePersistentTexture2D") == 4 &&
                  fixture.recorder().created_textures.size() == 2,
              "Pixel/texture-capacity: bounded retry recovers when capacity returns");
    }

    void testPixelPermanentUploadFailureIsLatched()
    {
        lux::bridgetest::HeadlessBridgeFixture fixture;
        lux::meta::EntityRegistry registry;
        lux::ecs::PixelFieldRuntime runtime;
        fixture.registerCanvas2DOps();
        fixture.registerPersistentTextureOps();

        const auto field = makeRuntimeField(
            runtime, "60000000-0000-4000-8000-000000000001");
        (void)makePixelField(registry, field);
        auto* ledger = runtime.dirtyLedger(field, {0, 0});

        lux::ecs::SceneRenderBinding binding{
            fixture.session(), fixture.control(),
            fixture.uploadClientForTest(), fixture.scene()};
        binding.setCatalog(fixture.features());
        lux::ecs::PixelField2DSubsystem pixels{&runtime};
        pixels.onAdded(lux::ecs::SystemSetupContext{registry, {}});
        fixture.beginFrame();

        frame(pixels, registry, fixture);   // shared textures
        frame(pixels, registry, fixture);   // palette upload
        frame(pixels, registry, fixture);   // chunk create

        fixture.recorder().texture_region_update_status =
            static_cast<std::uint32_t>(
                lux::render::ERegionUploadStatus::InvalidHandle
            );
        frame(pixels, registry, fixture);   // initial atlas upload
        frame(pixels, registry, fixture);   // consume refusal and latch
        const auto attempts = fixture.recorder().count("UpdateTextureRegions");
        for (int i = 0; i < 20; ++i)
            frame(pixels, registry, fixture);

        check(fixture.recorder().count("UpdateTextureRegions") == attempts,
              "Pixel/upload-permanent: structural refusal is not retried per frame");
        check(ledger != nullptr && ledger->hasDirty(),
              "Pixel/upload-permanent: rejected content remains dirty");
    }
} // namespace

int main()
{
    testImageDestroyBeforePump();
    testImageLeaveAndReenterBeforePump();
    testImageRetryableDispatchThenPermanentReply();
    testMeshDestroyBeforePump();
    testMeshRetryableDispatchThenPermanentReply();
    testSparseTilemapAtlasLifecycle();
    testCanvasLargePositionWire();
    testPixelFieldSwapKeepsPendingSlotOwned();
    testPixelFieldSwapKeepsUploadingSlotOwned();
    testPixelSlotWaitsForConcurrentUploads();
    testPixelSubsystemCancelRestoresDirtyExport();
    testPixelSharedTextureCapacityUsesBoundedRetry();
    testPixelPermanentUploadFailureIsLatched();

    std::fprintf(
        stderr,
        "\n%s (%d failure(s))\n",
        g_failures == 0 ? "LIFECYCLE PROBE PASSED" : "LIFECYCLE PROBE FAILED",
        g_failures
    );
    return g_failures == 0 ? 0 : 1;
}
