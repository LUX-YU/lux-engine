#pragma once

#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/process/world_loading/WorldStorageSource.hpp>

#include <filesystem>

namespace lux::process::world_loading
{
    struct WorldFileStorageLimits final
    {
        std::size_t requests{64};
        std::size_t range_bytes{64 * 1024 * 1024};
    };

    // Finite file-range provider on the application's blocking IO resource. CPU decoding is separate.
    // Destroying the provider closes copied sources to new requests; accepted reads retain their inputs.
    // The caller quiesces admission before destroying tasks, and joins ExecutionRuntime after accepted reads.
    class LUX_ENGINE_PROCESS_WORLD_LOADING_PUBLIC WorldFileStorageSource final
    {
    public:
        using CreateResult = lux::cxx::expected<std::unique_ptr<WorldFileStorageSource>, WorldStorageRuntimeFailure>;

        [[nodiscard]] static CreateResult create(
            std::shared_ptr<const world::WorldDescription> world,
            std::filesystem::path directory,
            execution::BlockingScheduler blocking,
            execution::TaskScope& tasks,
            WorldFileStorageLimits limits = {}
        ) noexcept;

        ~WorldFileStorageSource() noexcept;
        WorldFileStorageSource(const WorldFileStorageSource&) = delete;
        WorldFileStorageSource& operator=(const WorldFileStorageSource&) = delete;

        [[nodiscard]] WorldStorageSource source() const noexcept;

    private:
        struct Impl;
        explicit WorldFileStorageSource(std::shared_ptr<Impl> impl) noexcept;
        std::shared_ptr<Impl> impl_;
    };
} // namespace lux::process::world_loading
