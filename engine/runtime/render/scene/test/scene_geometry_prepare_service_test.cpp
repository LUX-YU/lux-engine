#include <lux/engine/runtime/render/scene/SceneGeometryPrepareService.hpp>

#include <lux/engine/function/render/standard/content/ClassicMeshBatch.hpp>
#include <lux/engine/ecs/scene_format/EntitySectionCodec.hpp>
#include <lux/engine/ecs/terrain/TerrainTileCodec.hpp>
#include <lux/engine/runtime/entity_scene/SectionBlobStore.hpp>
#include <lux/engine/runtime/execution/AsyncRuntime.hpp>
#include <lux/engine/runtime/execution/AsyncScope.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/runtime/execution/testing/AsyncCloseTestDriver.hpp>

#include <stdexec/execution.hpp>
#include <uuid.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

namespace
{
    void require(bool condition) noexcept
    {
        if (!condition)
            std::abort();
    }

    [[nodiscard]] uuids::uuid uuid(const char* text)
    {
        const auto parsed = uuids::uuid::from_string(text);
        require(parsed.has_value());
        return *parsed;
    }

    [[nodiscard]] lux::asset::asset_id_t assetId(std::uint8_t seed)
    {
        std::array<std::uint8_t, 16u> bytes{};
        for (std::size_t index = 0u; index != bytes.size(); ++index)
            bytes[index] = static_cast<std::uint8_t>(seed + index);
        return lux::asset::asset_id_t{bytes};
    }

    [[nodiscard]] std::vector<std::byte> classicMeshBytes()
    {
        lux::classic_mesh::ClassicMeshBatchBlobV1 source;
        source.instances.resize(2u);
        source.instances[0].mesh_asset = assetId(1u);
        source.instances[0].material_asset = assetId(33u);
        source.instances[0].stable_pick_id = 11u;
        source.instances[1].mesh_asset = assetId(65u);
        source.instances[1].stable_pick_id = 12u;
        auto encoded = lux::classic_mesh::encodeClassicMeshBatchBlob(source);
        require(encoded.has_value());
        return std::move(*encoded);
    }

    [[nodiscard]] std::vector<std::byte> terrainBytes()
    {
        lux::terrain::TerrainTileBlobV1 source;
        source.height_min = 0.0f;
        source.height_max = 10.0f;
        source.sample_spacing = 1.0f;
        source.weight_layer_count = 0u;
        source.heights.assign(lux::terrain::kTerrainTileSampleCount, 7u);
        source.weight_planes[0].assign(
            lux::terrain::kTerrainTileWeightPlaneBytes, 0u);
        source.weight_planes[1].assign(
            lux::terrain::kTerrainTileWeightPlaneBytes, 0u);
        source.holes.assign(lux::terrain::kTerrainTileHoleBytes, 0u);
        source.min_max_pairs.assign(
            static_cast<std::size_t>(
                lux::terrain::kTerrainTileMinMaxNodeCount) * 2u,
            7u);
        source.parent_fallback_heights.assign(
            lux::terrain::kTerrainTileFallbackSampleCount, 7u);
        auto encoded = lux::terrain::encodeTerrainTileBlob(source);
        require(encoded.has_value());
        return std::move(*encoded);
    }

    struct StoredBlob final
    {
        lux::ecs::scene_format::ContentBlobRef reference;
        lux::runtime::entity_scene::ContentBlobLease lease;
    };

    [[nodiscard]] StoredBlob storeBlob(
        lux::runtime::entity_scene::SectionBlobStore& store,
        std::string_view type,
        std::uint32_t schema,
        std::vector<std::byte> bytes,
        const char* section_text,
        std::uint64_t generation)
    {
        lux::ecs::scene_format::EntitySectionAttachment attachment;
        attachment.reference.type =
            lux::ecs::scene_format::ContentTypeId{std::string{type}};
        attachment.reference.schema_version = schema;
        attachment.payload = std::move(bytes);
        attachment.reference.id = lux::ecs::scene_format::makeContentBlobId(
            attachment.reference.type,
            attachment.reference.schema_version,
            attachment.payload);
        auto acquired = store.acquire(
            std::move(attachment),
            lux::ecs::scene_format::EntitySectionId{uuid(section_text)},
            generation);
        require(acquired.has_value());
        auto reference = acquired->reference();
        return {std::move(reference), std::move(*acquired)};
    }

    template <class Operation>
    [[nodiscard]] lux::async::OperationOutcome<Operation> run(
        lux::exec::AsyncRuntime& runtime,
        lux::exec::AsyncScope& scope,
        lux::exec::AsyncExecuteSender<Operation> sender,
        std::thread::id& completion_thread)
    {
        lux::exec::testing::CloseEpoch progress{runtime};
        std::optional<lux::async::OperationOutcome<Operation>> outcome;
        auto pipeline = std::move(sender) |
            stdexec::then(
                [&outcome, &completion_thread, &progress](
                    lux::async::OperationOutcome<Operation> value) mutable noexcept
                {
                    completion_thread = std::this_thread::get_id();
                    outcome.emplace(std::move(value));
                    progress.notify();
                });
        // The submission is the action under test, not a debug-only check.
        // Keeping it inside assert() removes the entire expression in
        // RelWithDebInfo and leaves the epoch driver waiting for a completion
        // that can never exist.
        if (!lux::exec::spawn(scope, std::move(pipeline)))
            std::abort();
        progress.drive([&outcome]() noexcept
        {
            return outcome.has_value();
        });
        return std::move(*outcome);
    }

} // namespace

int main()
{
    using namespace lux::runtime;

    lux::runtime::entity_scene::SectionBlobStore blobs;
    auto classic = storeBlob(
        blobs,
        lux::classic_mesh::kClassicMeshBatchContentTypeName,
        lux::classic_mesh::kClassicMeshBatchSchemaVersion,
        classicMeshBytes(),
        "91000000-0000-4000-8000-000000000001",
        1u);
    auto terrain = storeBlob(
        blobs,
        lux::terrain::kTerrainTileContentTypeName,
        lux::terrain::kTerrainTileSchemaVersion,
        terrainBytes(),
        "91000000-0000-4000-8000-000000000002",
        1u);

    // Input bytes fit, but the conservative decoded+wire working set does
    // not. Admission must reject before any worker allocation begins.
    {
        lux::exec::AsyncRuntimeBuilder tiny_builder;
        const auto encoded_bytes = classic.lease.bytes().size();
        auto tiny = SceneGeometryPrepareService::addTo(
            tiny_builder,
            SceneGeometryPrepareConfig{
                .classic_mesh = {1u, encoded_bytes + 1u, 1u},
                .terrain = {1u, 16u * 1024u * 1024u, 1u}});
        require(tiny.has_value());
        auto rejected = tiny->classicMeshClient().execute(
            PrepareClassicMeshBatch{
                classic.lease.bytes(), classic.reference, 1u});
        require(!rejected);
        require(rejected.error() ==
            lux::async::ESubmitError::BYTE_BUDGET_EXHAUSTED);
        tiny->close();
    }

    lux::exec::AsyncRuntimeBuilder builder;
    auto service_result = SceneGeometryPrepareService::addTo(
        builder,
        SceneGeometryPrepareConfig{
            .classic_mesh = {1u, 16u * 1024u * 1024u, 1u},
            .terrain = {2u, 32u * 1024u * 1024u, 1u}});
    require(service_result.has_value());
    auto service = std::move(*service_result);
    auto plan = std::move(builder).compile();
    require(plan.has_value());
    lux::exec::AsyncRuntime runtime{
        std::move(*plan),
        lux::exec::AsyncRuntimeConfig{
            .blocking_io_threads = 1u,
            .background_cpu_concurrency = 1u}};
    lux::exec::AsyncScope scope{runtime};
    const auto owner_thread = std::this_thread::get_id();
    auto classic_client = service.classicMeshClient();
    auto terrain_client = service.terrainClient();

    // A reservation lives in the not-yet-started sender. It therefore bounds
    // the complete queued/running interval instead of only endpoint storage.
    {
        auto held = classic_client.execute(PrepareClassicMeshBatch{
            classic.lease.bytes(), classic.reference, 2u});
        require(held.has_value());
        auto saturated = classic_client.execute(PrepareClassicMeshBatch{
            classic.lease.bytes(), classic.reference, 3u});
        require(!saturated);
        require(
            saturated.error() == lux::async::ESubmitError::QUEUE_FULL);
        const auto snapshot = service.snapshot();
        require(snapshot.classic_mesh.active_requests == 1u);
        require(snapshot.classic_mesh.active_bytes >
            classic.lease.bytes().size());
    }
    require(service.snapshot().classic_mesh.active_requests == 0u);

    std::thread::id completion_thread;
    auto classic_sender = classic_client.execute(PrepareClassicMeshBatch{
        classic.lease.bytes(), classic.reference, 4u});
    require(classic_sender.has_value());
    auto classic_outcome = run(
        runtime, scope, std::move(*classic_sender), completion_thread);
    require(completion_thread == owner_thread);
    require(classic_outcome.has_value());
    require(classic_outcome->request_generation == 4u);
    require(classic_outcome->decoded->instances.size() == 2u);
    require(classic_outcome->wire->size() == 2u);

    auto terrain_sender = terrain_client.execute(PrepareTerrainTile{
        terrain.lease.bytes(), terrain.reference, 5u});
    require(terrain_sender.has_value());
    auto terrain_outcome = run(
        runtime, scope, std::move(*terrain_sender), completion_thread);
    require(completion_thread == owner_thread);
    require(terrain_outcome.has_value());
    require(terrain_outcome->request_generation == 5u);
    require(static_cast<bool>(terrain_outcome->wire));
    require(!terrain_outcome->wire->empty());

    // The typed service independently authenticates bytes even when a caller
    // bypasses SectionBlobStore's normal resolved-reference path.
    auto forged_reference = classic.reference;
    forged_reference.id.digest[0] ^= std::byte{0xffu};
    auto forged_sender = classic_client.execute(PrepareClassicMeshBatch{
        classic.lease.bytes(), std::move(forged_reference), 6u});
    require(forged_sender.has_value());
    auto forged = run(
        runtime, scope, std::move(*forged_sender), completion_thread);
    require(!forged);
    require(!forged.error().isRuntime());
    require(forged.error().domainError().code ==
        ESceneGeometryPrepareError::CONTENT_MISMATCH);

    require(service.snapshot().classic_mesh.active_requests == 0u);
    require(service.snapshot().classic_mesh.active_bytes == 0u);
    require(service.snapshot().terrain.active_requests == 0u);
    require(service.snapshot().terrain.active_bytes == 0u);
    lux::exec::testing::closeScope(scope, runtime);

    // Move-assignment closes the displaced admission generation.
    lux::exec::AsyncRuntimeBuilder displaced_builder;
    auto displaced = SceneGeometryPrepareService::addTo(displaced_builder);
    require(displaced.has_value());
    auto displaced_client = displaced->classicMeshClient();
    *displaced = std::move(service);
    require(!displaced_client);
    service = std::move(*displaced);

    service.close();
    require(!classic_client);
    auto after_close = classic_client.execute(PrepareClassicMeshBatch{
        classic.lease.bytes(), classic.reference, 7u});
    require(!after_close);
    require(after_close.error() ==
        lux::async::ESubmitError::FEATURE_CLOSING);
    const auto closed = service.snapshot();
    require(closed.closing);
    require(closed.classic_mesh.active_requests == 0u);
    require(closed.terrain.active_requests == 0u);

    const auto close_report = lux::exec::testing::closeRuntime(runtime);
    require(close_report.clean());
    return 0;
}
