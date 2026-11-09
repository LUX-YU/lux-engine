#pragma once

#include <lux/engine/scene/SceneStreamingData.hpp>
#include <lux/engine/world/storage/visibility.h>
#include <lux/engine/error/Error.hpp>
#include <lux/cxx/compile_time/expected.hpp>

namespace lux::scene
{
    // Queries immutable build/decoded metadata. Output is reusable scratch, not an installed demand.
    // Publish a new demand only on success. No IO, Registry access, or per-node allocation occurs here.
    [[nodiscard]] LUX_ENGINE_WORLD_STORAGE_PUBLIC cxx::expected<void, error::Error>
    querySceneGroups(
        const SceneStreamingData& data, std::uint32_t geometry, const spatial::Spatial3DQueryBounds& bounds,
        std::vector<std::uint32_t>& output
    ) noexcept;
} // namespace lux::scene
