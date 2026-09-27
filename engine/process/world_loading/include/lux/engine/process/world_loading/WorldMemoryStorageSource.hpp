#pragma once
#include <lux/engine/process/world_loading/WorldStorageSource.hpp>
#include <span>

namespace lux::process::world_loading
{
    [[nodiscard]] LUX_ENGINE_PROCESS_WORLD_LOADING_PUBLIC lux::cxx::
        expected<WorldStorageSource, WorldStorageRuntimeFailure>
        makeWorldMemoryStorageSource(
            std::shared_ptr<const world::WorldDescription> world,
            std::span<const lux::cxx::SharedBytes<>> volumes
        ) noexcept;
}
