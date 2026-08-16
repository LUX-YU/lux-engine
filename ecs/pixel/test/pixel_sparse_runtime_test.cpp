#include <lux/engine/ecs/pixel/systems/PixelFieldRuntime.hpp>

#include <cassert>
#include <cstdint>
#include <iostream>
#include <utility>
#include <vector>

namespace
{
    [[nodiscard]] uuids::uuid uuid(const char* text)
    {
        return uuids::uuid::from_string(text).value();
    }

    [[nodiscard]] lux::ecs::PixelChunkLoad emptyChunk(
        lux::ecs::PixelChunkCoord coordinate,
        bool active)
    {
        lux::ecs::PixelChunkLoad result;
        result.coordinate = coordinate;
        result.materials.assign(
            lux::ecs::PixelFieldRuntime::kChunkCellCount,
            lux::ecs::kEmptyMaterial);
        result.presentation_active = active;
        result.simulation_active = active;
        return result;
    }
}

int main()
{
    using namespace lux::ecs;

    PixelFieldRuntime runtime{{.parallelism = 2u}};
    const auto stone = runtime.materials().add({
        EMaterialPhase::SOLID, 255u, 0xffffffffu});
    const auto sand = runtime.materials().add({
        EMaterialPhase::POWDER, 200u, 0xff00ffffu});

    const auto infinite = runtime.create({
        PixelFieldId{
            uuid("80000000-0000-4000-8000-000000000001")},
        EPixelFieldExtent::INFINITE,
        {},
        0u});
    assert(!infinite.isNull());
    auto first_chunk = emptyChunk({-1'000'000, 42}, true);
    auto second_chunk = emptyChunk({1'000'000, -42}, false);
    second_chunk.materials.front() = stone;
    assert(runtime.loadChunk(infinite, std::move(first_chunk)));
    auto preparation = runtime.chunkPreparationContext(infinite);
    assert(preparation);
    auto prepared = preparePixelChunk(
        std::move(second_chunk), std::move(*preparation));
    assert(prepared);
    assert(runtime.stats().synchronous_chunk_preparations == 1u);
    assert(runtime.adoptPreparedChunk(infinite, std::move(*prepared)));
    assert(runtime.stats().synchronous_chunk_preparations == 1u);
    assert(runtime.stats().prepared_chunk_adoptions == 2u);
    assert(runtime.stats().resident_chunks == 2u);
    const PixelCellCoord prepared_cell{
        1'000'000ll * PixelFieldRuntime::kChunkSizeCells,
        -42ll * PixelFieldRuntime::kChunkSizeCells};
    assert(runtime.regionBlocked(infinite, prepared_cell, prepared_cell));
    const auto active_chunks = runtime.activeKeys(infinite);
    assert((active_chunks.size() == 1u &&
        active_chunks.front() == PixelChunkCoord{-1'000'000, 42}));
    const auto presentation_chunks = runtime.presentationKeys(infinite);
    assert((presentation_chunks.size() == 1u &&
        presentation_chunks.front() == PixelChunkCoord{-1'000'000, 42}));

    // Growing the CPU-resident window must not grow fixed-step candidate
    // visits when the simulation-active window is unchanged.
    for (std::int64_t index = 0; index < 64; ++index)
    {
        auto inactive = emptyChunk({2'000'000 + index, 100}, false);
        assert(runtime.loadChunk(infinite, std::move(inactive)));
    }
    runtime.step();
    assert(runtime.stats().resident_chunks == 66u);
    assert(runtime.stats().simulation_active_chunks == 1u);
    assert(runtime.stats().simulation_chunks_visited_last_step == 1u);
    const auto active_after_growth = runtime.activeKeys(infinite);
    const auto* active_storage = active_after_growth.data();
    for (std::uint32_t repeat = 0u; repeat != 1024u; ++repeat)
    {
        const auto repeated = runtime.activeKeys(infinite);
        assert(repeated.data() == active_storage);
        assert(repeated.size() == 1u);
    }
    assert(runtime.setChunkSimulationActive(
        infinite,
        {1'000'000, -42},
        true));
    assert(runtime.activeKeys(infinite).data() == active_storage);
    assert(runtime.setChunkSimulationActive(
        infinite,
        {1'000'000, -42},
        false));
    assert(runtime.activeKeys(infinite).data() == active_storage);
    assert(runtime.presentationKeys(infinite).size() == 1u);

    const auto retire_stats_before = runtime.stats();
    assert(runtime.discardChunk(infinite, {2'000'000, 100}));
    const auto retire_stats_after = runtime.stats();
    assert(retire_stats_after.capturing_chunk_unloads ==
        retire_stats_before.capturing_chunk_unloads);
    assert(retire_stats_after.discard_chunk_retires ==
        retire_stats_before.discard_chunk_retires + 1u);
    assert(runtime.loadChunk(
        infinite, emptyChunk({2'000'000, 100}, false)));
    assert(runtime.setChunkPresentationActive(
        infinite,
        {1'000'000, -42},
        true));
    assert(runtime.presentationKeys(infinite).size() == 2u);
    assert(runtime.setChunkPresentationActive(
        infinite,
        {1'000'000, -42},
        false));
    assert(runtime.presentationKeys(infinite).size() == 1u);

    const PixelCellCoord far_base{
        -1'000'000ll * PixelFieldRuntime::kChunkSizeCells,
        42ll * PixelFieldRuntime::kChunkSizeCells};
    PixelFieldCommand stamp;
    stamp.field = infinite;
    stamp.minimum = far_base;
    stamp.extent = {8u, 8u};
    stamp.material = stone;
    runtime.enqueue(stamp);
    runtime.applyCommands();
    assert(runtime.regionBlocked(infinite, far_base, far_base));

    const PixelCellCoord missing{
        far_base.x + PixelFieldRuntime::kChunkSizeCells,
        far_base.y};
    assert(runtime.regionBlocked(infinite, missing, missing));

    PixelChunkSnapshot snapshot;
    assert(runtime.unloadChunk(infinite, {-1'000'000, 42}, snapshot));
    assert(snapshot.delta.size() == 64u);
    assert(runtime.stats().resident_chunks == 65u);
    PixelChunkLoad restore;
    restore.coordinate = snapshot.coordinate;
    restore.materials = std::move(snapshot.materials);
    restore.temperature = std::move(snapshot.temperature);
    restore.lifetime = std::move(snapshot.lifetime);
    restore.base_digest = snapshot.base_digest;
    restore.sequence = snapshot.sequence;
    restore.delta = std::move(snapshot.delta);
    restore.presentation_active = snapshot.presentation_active;
    restore.simulation_active = snapshot.simulation_active;
    assert(runtime.loadChunk(infinite, std::move(restore)));
    assert(runtime.regionBlocked(infinite, far_base, far_base));

    PixelFieldCommand falling;
    falling.field = infinite;
    falling.minimum = {far_base.x + 16, far_base.y + 64};
    falling.extent = {4u, 4u};
    falling.material = sand;
    runtime.enqueue(falling);
    runtime.applyCommands();
    runtime.step();
    assert(runtime.cellsScannedLastStep(infinite) <=
        PixelFieldRuntime::kChunkCellCount);

    const auto huge_bounded = runtime.create({
        PixelFieldId{
            uuid("80000000-0000-4000-8000-000000000002")},
        EPixelFieldExtent::BOUNDED,
        {{-1'000'000'000, -1'000'000'000},
         {1'000'000'000, 1'000'000'000}},
        0u});
    assert(!huge_bounded.isNull());
    assert(runtime.stats().resident_chunks == 66u);
    runtime.step();
    assert(runtime.cellsScannedLastStep(huge_bounded) == 0u);

    PixelFieldFrame frame;
    frame.origin = {102'400'000.0, -102'400'000.0};
    frame.cell_size = 0.5f;
    const auto world_cell = worldToCell(
        frame,
        {102'401'025.0, -102'401'026.0});
    assert(world_cell);
    assert(world_cell->x == 2050);
    assert(world_cell->y == -2052);

    runtime.destroy(huge_bounded);
    assert(!runtime.isAlive(huge_bounded));
    const auto recycled = runtime.create({
        PixelFieldId{
            uuid("80000000-0000-4000-8000-000000000003")},
        EPixelFieldExtent::INFINITE,
        {},
        0u});
    assert(!recycled.isNull());
    assert(recycled != huge_bounded);
    assert(!runtime.setChunkSimulationActive(
        huge_bounded,
        {0, 0},
        true));

    std::cout << "sparse Pixel field tests passed\n";
    return 0;
}
