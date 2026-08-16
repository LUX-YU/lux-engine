#include <lux/engine/ecs/tilemap/systems/TilemapRuntime.hpp>

#include <cassert>
#include <cstdint>
#include <utility>
#include <vector>

int main()
{
    using namespace lux::ecs;
    TilemapRuntime runtime;
    const auto handle = runtime.create({
        TilemapId{uuids::uuid::from_string(
            "71000000-0000-4000-8000-000000000001").value()}});
    assert(handle.valid());

    TileChunkLoad near_chunk;
    near_chunk.coordinate = {-1, 0};
    near_chunk.tiles.assign(
        TilemapRuntime::kChunkTileCount,
        lux::rdesc::kEmptyTile);
    assert(runtime.loadChunk(handle, std::move(near_chunk)));

    TileChunkLoad far_chunk;
    far_chunk.coordinate = {1'000'000, -1'000'000};
    far_chunk.tiles.assign(TilemapRuntime::kChunkTileCount, 3u);
    assert(runtime.loadChunk(handle, std::move(far_chunk)));
    assert(runtime.stats().resident_chunks == 2u);
    auto active = runtime.activeKeys(handle);
    assert(active.size() == 2u);
    assert(runtime.setChunkActive(handle, {1'000'000, -1'000'000}, false));
    active = runtime.activeKeys(handle);
    assert((active.size() == 1u &&
        active.front() == TileChunkCoord{-1, 0}));
    for (std::int64_t index = 0; index < 64; ++index)
    {
        TileChunkLoad inactive;
        inactive.coordinate = {index + 10, index + 20};
        inactive.tiles.assign(
            TilemapRuntime::kChunkTileCount,
            lux::rdesc::kEmptyTile);
        inactive.active = false;
        assert(runtime.loadChunk(handle, std::move(inactive)));
    }
    active = runtime.activeKeys(handle);
    assert(active.size() == 1u);
    assert(runtime.stats().active_enumeration_chunks_visited_last == 1u);
    const auto* active_storage = active.data();
    for (std::uint32_t repeat = 0u; repeat != 1024u; ++repeat)
    {
        const auto repeated = runtime.activeKeys(handle);
        assert(repeated.data() == active_storage);
        assert(repeated.size() == 1u);
    }
    assert(runtime.setChunkActive(handle, {10, 20}, true));
    assert(runtime.activeKeys(handle).data() == active_storage);
    assert(runtime.setChunkActive(handle, {10, 20}, false));
    assert(runtime.activeKeys(handle).data() == active_storage);
    assert(runtime.tileAt(handle, {-1, 0}) == lux::rdesc::kEmptyTile);
    assert(runtime.setTile(handle, {-1, 0}, 7u));
    assert(runtime.tileAt(handle, {-1, 0}) == 7u);

    TileChunkRenderExport exported;
    assert(runtime.exportDirty(handle, {-1, 0}, exported));
    assert(!exported.empty());
    runtime.confirmExport(
        handle,
        {-1, 0},
        exported.content_revision,
        true);
    exported = {};
    assert(runtime.exportDirty(handle, {-1, 0}, exported));
    assert(exported.empty());

    TileChunkSnapshot snapshot;
    assert(runtime.unloadChunk(handle, {-1, 0}, snapshot));
    assert(runtime.stats().resident_chunks == 65u);
    assert(snapshot.delta.size() == 1u);

    TileChunkLoad duplicate;
    duplicate.coordinate = {3, 4};
    duplicate.tiles.assign(
        TilemapRuntime::kChunkTileCount,
        lux::rdesc::kEmptyTile);
    duplicate.delta = {{1u, 2u, 3u}, {1u, 2u, 4u}};
    assert(!runtime.loadChunk(handle, std::move(duplicate)));

    runtime.destroy(handle);
    assert(!runtime.isAlive(handle));
    const auto recycled = runtime.create({
        TilemapId{uuids::uuid::from_string(
            "71000000-0000-4000-8000-000000000002").value()}});
    assert(recycled.valid());
    assert(recycled != handle);
    assert(!runtime.setChunkActive(handle, {0, 0}, true));
    TileChunkLoad discarded;
    discarded.coordinate = {9, -9};
    discarded.tiles.assign(TilemapRuntime::kChunkTileCount, 2u);
    assert(runtime.loadChunk(recycled, std::move(discarded)));
    assert(runtime.discardChunk(recycled, {9, -9}));
    assert(!runtime.discardChunk(recycled, {9, -9}));
    return 0;
}
