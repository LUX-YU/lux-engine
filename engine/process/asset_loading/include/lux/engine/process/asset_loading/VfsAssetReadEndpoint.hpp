#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/process/asset_loading/AssetLoadSender.hpp>
#include <lux/engine/process/asset_loading/visibility.h>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>

namespace lux::process::asset_loading
{
    enum class EVfsAssetReadEndpointError : std::uint8_t
    {
        INVALID_ARGUMENT,
    };

    struct VfsAssetReadEndpointConfig final
    {
        std::size_t request_capacity{};
    };

    class LUX_PROCESS_ASSET_LOADING_PUBLIC VfsAssetReadEndpoint final
    {
    public:
        using CreateResult = lux::cxx::expected<std::unique_ptr<VfsAssetReadEndpoint>, EVfsAssetReadEndpointError>;

        [[nodiscard]] static CreateResult create(
            asset::AssetVfsView vfs,
            execution::BlockingScheduler blocking,
            execution::ExecutionRuntime& execution,
            VfsAssetReadEndpointConfig config,
            std::stop_token stop = {}
        ) noexcept;

        // Runtime outlives accepted reads and their callback tails. A supplied group token cancels
        // queued reads; a running blocking read completes before returning its result.
        // Destruction closes copied ports without waiting; requests own their inputs and completion state.
        ~VfsAssetReadEndpoint() noexcept;
        VfsAssetReadEndpoint(const VfsAssetReadEndpoint&) = delete;
        VfsAssetReadEndpoint& operator=(const VfsAssetReadEndpoint&) = delete;
        VfsAssetReadEndpoint(VfsAssetReadEndpoint&&) = delete;
        VfsAssetReadEndpoint& operator=(VfsAssetReadEndpoint&&) = delete;

        [[nodiscard]] AssetReadPort port() const noexcept;

    private:
        struct Impl;

        explicit VfsAssetReadEndpoint(std::shared_ptr<Impl> impl) noexcept;

        std::shared_ptr<Impl> impl_;
    };
} // namespace lux::process::asset_loading
