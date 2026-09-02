#pragma once

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
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
        ALLOCATION_FAILURE,
        WRONG_THREAD,
        INVALID_STATE,
        ALREADY_JOINED,
    };

    struct VfsAssetReadEndpointConfig final
    {
        std::size_t request_capacity{};
    };

    class LUX_PROCESS_ASSET_LOADING_PUBLIC VfsAssetReadEndpoint final
        : public AssetReadPort::Endpoint,
          public std::enable_shared_from_this<VfsAssetReadEndpoint>
    {
    public:
        using CreateResult = lux::cxx::expected<
            std::shared_ptr<VfsAssetReadEndpoint>,
            EVfsAssetReadEndpointError
        >;

        [[nodiscard]] static CreateResult create(
            asset::AssetVfsView vfs,
            BlockingScheduler blocking,
            VfsAssetReadEndpointConfig config
        ) noexcept;

        ~VfsAssetReadEndpoint() override;
        VfsAssetReadEndpoint(const VfsAssetReadEndpoint&) = delete;
        VfsAssetReadEndpoint& operator=(const VfsAssetReadEndpoint&) = delete;

        [[nodiscard]] AssetReadPort port() noexcept;
        void requestStop() noexcept;
        [[nodiscard]] lux::cxx::expected<void, EVfsAssetReadEndpointError> join() noexcept;

        [[nodiscard]] lux::async::SubmitResult submit(
            ReadAssetImage operation,
            void* completion_state,
            void (*complete)(void*, Outcome&&) noexcept,
            lux::async::SubmitOptions options
        ) noexcept override;

    private:
        struct Impl;

        explicit VfsAssetReadEndpoint(std::unique_ptr<Impl> impl) noexcept;

        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::process::asset_loading
