#pragma once

#include <lux/engine/ecs/Entity.hpp>
#include <lux/engine/error/Error.hpp>
#include <lux/engine/spatial/Spatial3DPartitionIndex.hpp>
#include <lux/engine/world/composition/visibility.h>
#include <lux/cxx/compile_time/expected.hpp>

#include <memory>
#include <span>
#include <vector>

namespace lux::world
{
    // Identity-only notification published by professional producers after their index maintenance.

    // Main-thread spatial data. The professional producer maintains it before consumers query this frame.
    // Bounds are half-open with positive extent. Returned pointers last only until the next mutation.
    class LUX_ENGINE_WORLD_COMPOSITION_PUBLIC WorldSpatialIndex final
    {
    public:
        using Bounds = spatial::Spatial3DQueryBounds;
        using Result = cxx::expected<void, error::Error>;

        WorldSpatialIndex() noexcept;
        ~WorldSpatialIndex() noexcept;
        WorldSpatialIndex(const WorldSpatialIndex&) = delete;
        WorldSpatialIndex& operator=(const WorldSpatialIndex&) = delete;

        // Input errors preserve the entire index and output; successful calls append coverage changes.
        // Input ranges must not borrow index storage or the output vector.
        [[nodiscard]] Result
        updateBatch(
            std::span<const ecs::Entity> entities,
            std::span<const Bounds> bounds,
            double coverage_margin,
            std::vector<ecs::Entity>& coverage_changed
        ) noexcept;
        
        void removeBatch(std::span<const ecs::Entity> entities, std::vector<ecs::Entity>& coverage_changed) noexcept;

        // Returns conservative coverage intersections, not exact geometry tests. Each region occupies
        // offsets[i]..offsets[i+1]; output is replaced only after all regions qualify.
        [[nodiscard]] Result queryBatch(
            std::span<const Bounds> regions,
            std::vector<std::size_t>& offsets,
            std::vector<ecs::Entity>& objects
        ) const noexcept;
        [[nodiscard]] const Bounds* actualBounds(ecs::Entity entity) const noexcept;
        [[nodiscard]] const Bounds* coverageBounds(ecs::Entity entity) const noexcept;

    private:
        struct Impl;
        std::unique_ptr<Impl> impl_;
    };
}
