#pragma once

#include <lux/engine/render/extensions/execution/ProducerFactory.hpp>
#include <vector>

namespace lux::render
{
    struct GraphPreparation final
    {
        ProducerFactory factory;
        ConfigValue configuration;
        std::vector<GraphCpuDataId> inputs;
        std::vector<GraphCpuDataId> outputs;
        std::vector<GraphPassId> inline_blocks;
        // A live preparation retains all of its GPU passes. Inline block passes are included automatically.
        std::vector<GraphPassId> passes{};
        // Each listed buffer is published once, through its linked writer or sizer.
        std::vector<GraphResourceId> gpu_buffers{};
        std::vector<GraphStorageSnapshotId> storage_inputs{};
        // Subset of storage_inputs that drives the runtime Raster program selection.
        std::vector<GraphStorageSnapshotId> program_storage_inputs{};
    };
} // namespace lux::render
