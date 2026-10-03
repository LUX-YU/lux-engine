#pragma once

#include <lux/engine/core/async/OperationPort.hpp>
#include <lux/engine/process/PortSender.hpp>
#include <lux/engine/process/ExecutionRuntime.hpp>
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
    struct ReadAssetImage final
    {
        using Value = lux::asset::AssetBlob;
        using Error = lux::asset::EAssetStorageError;

        lux::asset::AssetId id;
        std::size_t max_bytes{SIZE_MAX};
    };

    using AssetReadPort = lux::async::TOperationPort<ReadAssetImage>;

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
        EExecutionError execution_error{};
    };

    namespace detail
    {
        template <class Owner, class Environment> struct TAssetReadReceiver final
        {
            using receiver_concept = stdexec::receiver_t;

            void set_value(lux::asset::AssetBlob value) && noexcept
            {
                owner->readValue(std::move(value));
            }

            void set_error(lux::async::TOperationFailure<lux::asset::EAssetStorageError> failure) && noexcept
            {
                owner->readError(std::move(failure));
            }

            void set_error(EExecutionError failure) && noexcept
            {
                owner->executionError(failure);
            }

            void set_stopped() && noexcept
            {
                owner->readStopped();
            }

            [[nodiscard]] Environment get_env() const noexcept
            {
                return environment;
            }

            Owner* owner{};
            Environment environment;
        };

        template <class ConcreteAsset> class TAssetLoadSender final
        {
            static_assert(std::is_base_of_v<lux::asset::Asset, ConcreteAsset>);

        public:
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<
                stdexec::set_value_t(std::shared_ptr<const ConcreteAsset>),
                stdexec::set_error_t(AssetLoadFailure),
                stdexec::set_stopped_t()>;

            TAssetLoadSender(
                AssetReadPort read,
                CpuScheduler cpu,
                lux::asset::AssetId id,
                lux::asset::AssetDecodeLimits limits,
                std::stop_token stop
            ) noexcept
                : read_(std::move(read)), cpu_(std::move(cpu)), id_(id), limits_(limits), stop_(stop)
            {}

            template <class Receiver> class TState final
            {
            public:
                using operation_state_concept = stdexec::operation_state_t;
                using ReadReceiver = TAssetReadReceiver<TState, stdexec::env_of_t<Receiver>>;
                using ReadSender = decltype(stdexec::continues_on(
                    lux::process::portSender(std::declval<AssetReadPort>(), std::declval<ReadAssetImage>()),
                    std::declval<CpuScheduler>()
                ));
                using ReadState = decltype(stdexec::connect(std::declval<ReadSender>(), std::declval<ReadReceiver>()));

                TState(
                    AssetReadPort read,
                    CpuScheduler cpu,
                    lux::asset::AssetId id,
                    lux::asset::AssetDecodeLimits limits,
                    std::stop_token stop,
                    Receiver receiver
                )
                    : receiver_(std::move(receiver)), id_(id), limits_(limits), stop_(stop),
                      read_state_(stdexec::connect(
                          stdexec::continues_on(
                              lux::process::portSender(std::move(read), ReadAssetImage{id, limits.max_image_bytes}),
                              std::move(cpu)
                          ),
                          ReadReceiver{this, stdexec::get_env(receiver_)}
                      ))
                {}

                TState(const TState&) = delete;
                TState& operator=(const TState&) = delete;
                TState(TState&&) = delete;
                TState& operator=(TState&&) = delete;

                void start() & noexcept
                {
                    const auto token = stdexec::get_stop_token(stdexec::get_env(receiver_));
                    if (stop_.stop_requested() || token.stop_requested())
                    {
                        stdexec::set_stopped(std::move(receiver_));
                        return;
                    }
                    if (id_.isNull())
                    {
                        stdexec::set_error(std::move(receiver_), AssetLoadFailure{EAssetLoadError::INVALID_ASSET_ID});
                        return;
                    }
                    stdexec::start(read_state_);
                }

                void readValue(lux::asset::AssetBlob value) noexcept
                {
                    const auto token = stdexec::get_stop_token(stdexec::get_env(receiver_));
                    if (stop_.stop_requested() || token.stop_requested())
                    {
                        stdexec::set_stopped(std::move(receiver_));
                        return;
                    }

                    auto decoded =
                        lux::asset::TAssetSerDeser<ConcreteAsset>::decode(id_, std::move(value.bytes), limits_);
                    if (stop_.stop_requested() || token.stop_requested())
                    {
                        stdexec::set_stopped(std::move(receiver_));
                        return;
                    }
                    if (!decoded)
                    {
                        AssetLoadFailure failure{EAssetLoadError::DECODE_FAILURE};
                        failure.decode = decoded.error();
                        stdexec::set_error(std::move(receiver_), std::move(failure));
                        return;
                    }
                    stdexec::set_value(std::move(receiver_), std::move(*decoded));
                }

                void readError(lux::async::TOperationFailure<lux::asset::EAssetStorageError> failure) noexcept
                {
                    AssetLoadFailure mapped;
                    if (failure.isRuntime())
                    {
                        mapped.code = EAssetLoadError::SUBMIT_FAILURE;
                        mapped.submit_error = failure.runtimeError();
                    }
                    else
                    {
                        mapped.code = EAssetLoadError::STORAGE_FAILURE;
                        mapped.storage_error = failure.domainError();
                    }
                    stdexec::set_error(std::move(receiver_), std::move(mapped));
                }

                void readStopped() noexcept
                {
                    stdexec::set_stopped(std::move(receiver_));
                }

                void executionError(EExecutionError error) noexcept
                {
                    AssetLoadFailure failure{EAssetLoadError::EXECUTION_FAILURE};
                    failure.execution_error = error;
                    stdexec::set_error(std::move(receiver_), std::move(failure));
                }

            private:
                Receiver receiver_;
                lux::asset::AssetId id_;
                lux::asset::AssetDecodeLimits limits_;
                std::stop_token stop_;
                ReadState read_state_;
            };

            template <class Receiver> [[nodiscard]] TState<std::decay_t<Receiver>> connect(Receiver&& receiver) &&
            {
                return TState<std::decay_t<Receiver>>{
                    std::move(read_),
                    std::move(cpu_),
                    id_,
                    limits_,
                    stop_,
                    std::forward<Receiver>(receiver)
                };
            }

            [[nodiscard]] stdexec::empty_env get_env() const noexcept
            {
                return {};
            }

        private:
            AssetReadPort read_;
            CpuScheduler cpu_;
            lux::asset::AssetId id_;
            lux::asset::AssetDecodeLimits limits_;
            std::stop_token stop_;
        };
    } // namespace detail

    template <class ConcreteAsset>
    [[nodiscard]] auto loadAsset(
        AssetReadPort read,
        CpuScheduler cpu,
        lux::asset::AssetId id,
        lux::asset::AssetDecodeLimits limits,
        std::stop_token stop = {}
    ) noexcept
    {
        return detail::TAssetLoadSender<ConcreteAsset>(std::move(read), std::move(cpu), id, limits, stop);
    }
} // namespace lux::process::asset_loading
