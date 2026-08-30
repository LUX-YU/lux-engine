#include <lux/engine/spatial/Spatial2DPartitionIndex.hpp>

#include <array>
#include <atomic>
#include <cassert>
#include <cstdlib>
#include <limits>
#include <new>

namespace
{
    std::atomic_size_t allocations{};
}

void* operator new(std::size_t size)
{
    allocations.fetch_add(1U, std::memory_order_relaxed);
    if (void* memory = std::malloc(size); memory != nullptr)
    {
        return memory;
    }
    throw std::bad_alloc{};
}

void* operator new[](std::size_t size)
{
    return ::operator new(size);
}

void operator delete(void* memory) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory) noexcept
{
    std::free(memory);
}

void operator delete(void* memory, std::size_t) noexcept
{
    std::free(memory);
}

void operator delete[](void* memory, std::size_t) noexcept
{
    std::free(memory);
}

int main()
{
    using namespace lux::spatial;

    assert(!Spatial2DPartitionIndex::create({}, 1.0, {}));
    assert(!Spatial2DPartitionIndex::create({}, -1.0, {{{0, 0}, {0U}}}));
    assert(!Spatial2DPartitionIndex::create(
        {},
        1.0,
        {{{0, 0}, {std::numeric_limits<std::uint32_t>::max()}}}
    ));
    assert(!Spatial2DPartitionIndex::create({}, 1.0, {{{0, 0}, {0U}}, {{0, 0}, {1U}}}));
    assert(!Spatial2DPartitionIndex::create({}, 1.0, {{{0, 0}, {0U}}, {{1, 0}, {0U}}}));

    auto index = Spatial2DPartitionIndex::create(
        {},
        32.0,
        {
            {{1, 0}, {12U}},
            {{-1, 0}, {10U}},
            {{0, 0}, {11U}},
            {{0, 2}, {13U}}
        }
    );
    assert(index);
    assert((index->entries().front().coordinate == lux::math::GridCoord2i64{-1, 0}));
    assert((index->coordinate({-0.25, 0.0}).value() == lux::math::GridCoord2i64{-1, 0}));
    assert((index->coordinate({32.0, 0.0}).value() == lux::math::GridCoord2i64{1, 0}));
    assert(index->find(lux::math::GridCoord2i64{0, 0})->value == 11U);
    assert(!index->find(lux::math::Position2d{0.0, 32.0}).value());

    std::array<lux::partition::PartitionOrdinal, 1U> half_open{};
    auto half_open_count = index->query({{0.0, 0.0}, {32.0, 32.0}}, half_open);
    assert(half_open_count && *half_open_count == 1U && half_open[0].value == 11U);

    // The window crosses three cells but the finite catalog has a hole at x=2.
    // Existing entries are returned instead of failing the whole edge query.
    std::array<lux::partition::PartitionOrdinal, 4U> sparse{};
    auto sparse_count = index->query({{-32.0, 0.0}, {96.0, 32.0}}, sparse);
    assert(sparse_count && *sparse_count == 3U);
    assert(sparse[0].value == 10U && sparse[1].value == 11U && sparse[2].value == 12U);

    std::array<lux::partition::PartitionOrdinal, 3U> too_small{{{55U}, {56U}, {57U}}};
    auto capacity = index->query({{-32.0, 0.0}, {96.0, 32.0}}, too_small);
    assert(!capacity);
    assert(capacity.error().code == ESpatial2DPartitionIndexError::OUTPUT_CAPACITY_EXCEEDED);
    assert(capacity.error().required_capacity == 4U);
    assert(too_small[0].value == 55U && too_small[1].value == 56U && too_small[2].value == 57U);

    const std::size_t allocations_before = allocations.load(std::memory_order_relaxed);
    for (std::size_t iteration{}; iteration < 1000U; ++iteration)
    {
        assert(index->query({{-32.0, 0.0}, {96.0, 32.0}}, sparse));
    }
    assert(allocations.load(std::memory_order_relaxed) == allocations_before);

    auto far = Spatial2DPartitionIndex::create(
        {1.0e12, -1.0e12},
        0.125,
        {{{1, -1}, {1U}}}
    );
    assert(far);
    const auto far_coordinate = far->coordinate({1.0e12 + 0.125, -1.0e12 - 0.125});
    assert((far_coordinate && *far_coordinate == lux::math::GridCoord2i64{1, -1}));

    const auto overflow = index->coordinate({std::numeric_limits<double>::max(), 0.0});
    assert(!overflow && overflow.error().code == ESpatial2DPartitionIndexError::COORDINATE_OVERFLOW);
    assert(!index->query({{0.0, 0.0}, {0.0, 1.0}}, sparse));
    return 0;
}
