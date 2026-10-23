#pragma once

#include <exec/variant_sender.hpp>
#include <lux/engine/core/async/OperationPort.hpp>
#include <lux/engine/core/execution/PortSender.hpp>
#include <lux/engine/core/execution/Scheduler.hpp>
#include <lux/engine/process/asset_loading/AssetRead.hpp>
#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/AssetStorageError.hpp>
#include <lux/engine/resource/asset/storage/AssetProvider.hpp>
#include <stop_token>

#include <cstdint>
#include <memory>
#include <stdexec/execution.hpp>
#include <type_traits>
#include <utility>

namespace lux::process::asset_loading
{
    enum class EAssetLoadError : std::uint8_t
    {
        INVALID_ASSET_ID,
        SUBMIT_FAILURE,
        STORAGE_FAILURE,
        DECODE_FAILURE,
        EXECUTION_FAILURE,
    };

    struct AssetLoadFailure final
    {
        EAssetLoadError code{EAssetLoadError::INVALID_ASSET_ID};
        lux::async::ESubmitError submit_error{lux::async::ESubmitError::UNKNOWN_OPERATION};
        lux::asset::EAssetStorageError storage_error{lux::asset::EAssetStorageError::NOT_FOUND};
        lux::asset::AssetDecodeFailure decode;
        execution::EExecutionError execution_error{};
    };

    namespace detail
    {
        [[nodiscard]] inline AssetLoadFailure assetReadFailure(
            lux::async::TOperationFailure<lux::asset::EAssetStorageError> failure
        ) noexcept
        {
            AssetLoadFailure result;
            if (failure.isRuntime())
            {
                result.code = EAssetLoadError::SUBMIT_FAILURE;
                result.submit_error = failure.runtimeError();
            }
            else
            {
                result.code = EAssetLoadError::STORAGE_FAILURE;
                result.storage_error = failure.domainError();
            }
            return result;
        }

        template <class Asset, class StopToken>
        [[nodiscard]] auto decodeAsset(
            lux::asset::AssetId id,
            lux::asset::AssetBlob& image,
            lux::asset::AssetDecodeLimits limits,
            std::stop_token stop,
            StopToken ambient_stop
        ) noexcept
        {
            using VResult = exec::variant_sender<
                decltype(stdexec::just(std::shared_ptr<const Asset>{})),
                decltype(stdexec::just_error(AssetLoadFailure{})),
                decltype(stdexec::just_stopped())>;
            if (stop.stop_requested() || ambient_stop.stop_requested())
            {
                return VResult{stdexec::just_stopped()};
            }
            auto decoded = lux::asset::TAssetSerDeser<Asset>::decode(id, std::move(image.bytes), limits);
            if (stop.stop_requested() || ambient_stop.stop_requested())
            {
                return VResult{stdexec::just_stopped()};
            }
            if (!decoded)
            {
                AssetLoadFailure failure{EAssetLoadError::DECODE_FAILURE};
                failure.decode = decoded.error();
                return VResult{stdexec::just_error(std::move(failure))};
            }
            return VResult{stdexec::just(std::move(*decoded))};
        }
    } // namespace detail

    // Reads on the provider's execution resource, then decodes on the shared CPU pool.
    // No Main-thread delivery is implied. Accepted reads settle before cancellation releases their inputs.
    template <class Asset>
    [[nodiscard]] auto loadAsset(
        AssetReadPort read,
        execution::CpuScheduler cpu,
        lux::asset::AssetId id,
        lux::asset::AssetDecodeLimits limits,
        std::stop_token stop = {},
        lux::asset::AssetVfsView snapshot = {}
    ) noexcept
    {
        static_assert(std::is_base_of_v<lux::asset::Asset, Asset>);
        auto operation = stdexec::let_value(
            stdexec::read_env(stdexec::get_stop_token),
            [read = std::move(read), cpu, id, limits, stop, snapshot = std::move(snapshot)](auto ambient_stop) noexcept
            {
                auto image = stdexec::let_error(
                    execution::portSender(read, ReadAssetImage{id, limits.max_image_bytes, snapshot}),
                    [](lux::async::TOperationFailure<lux::asset::EAssetStorageError> failure) noexcept
                    { return stdexec::just_error(detail::assetReadFailure(failure)); }
                );
                auto decoded = stdexec::let_value(
                    stdexec::continues_on(std::move(image), cpu),
                    [id, limits, stop, ambient_stop](lux::asset::AssetBlob& value) noexcept
                    { return detail::decodeAsset<Asset>(id, value, limits, stop, ambient_stop); }
                );
                using VSource = exec::variant_sender<
                    decltype(decoded),
                    decltype(stdexec::just_error(AssetLoadFailure{})),
                    decltype(stdexec::just_stopped())>;
                if (stop.stop_requested() || ambient_stop.stop_requested())
                {
                    return VSource{stdexec::just_stopped()};
                }
                if (id.isNull())
                {
                    return VSource{stdexec::just_error(AssetLoadFailure{EAssetLoadError::INVALID_ASSET_ID})};
                }
                return VSource{std::move(decoded)};
            }
        );
        return stdexec::let_error(
            std::move(operation),
            [](auto failure) noexcept -> decltype(stdexec::just_error(AssetLoadFailure{}))
            {
                if constexpr (std::same_as<decltype(failure), std::exception_ptr>)
                {
                    std::terminate();
                }
                else
                {
                    return stdexec::just_error(std::move(failure));
                }
            }
        );
    }
} // namespace lux::process::asset_loading
