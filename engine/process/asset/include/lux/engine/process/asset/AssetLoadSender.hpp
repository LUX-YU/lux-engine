#pragma once

#include <lux/engine/core/async/OperationPort.hpp>
#include <lux/engine/process/PortSender.hpp>
#include <lux/engine/resource/asset/Asset.hpp>
#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/resource/asset/AssetStorageError.hpp>
#include <lux/engine/resource/asset/storage/AssetProvider.hpp>

#include <cstdint>
#include <memory>
#include <stdexec/execution.hpp>
#include <type_traits>
#include <utility>

namespace lux::process::asset
{
    struct ReadAssetImage final
    {
        using Value = lux::asset::AssetBlob;
        using Error = lux::asset::EAssetStorageError;

        lux::asset::AssetId id;
    };

    using AssetReadPort = lux::async::OperationPort<ReadAssetImage>;

    enum class EAssetLoadError : std::uint8_t
    {
        INVALID_ASSET_ID,
        SUBMIT_FAILURE,
        STORAGE_FAILURE,
        DECODE_FAILURE,
    };

    struct AssetLoadFailure final
    {
        EAssetLoadError code{EAssetLoadError::INVALID_ASSET_ID};
        lux::async::ESubmitError submit_error{lux::async::ESubmitError::UNKNOWN_OPERATION};
        lux::asset::EAssetStorageError storage_error{lux::asset::EAssetStorageError::NOT_FOUND};
        lux::asset::AssetDecodeFailure decode;
    };

    namespace detail
    {
        template <class Owner>
        struct AssetReadReceiver final
        {
            using receiver_concept = stdexec::receiver_t;

            void set_value(lux::asset::AssetBlob value) && noexcept
            {
                owner->readValue(std::move(value));
            }

            void set_error(lux::async::OperationFailure<lux::asset::EAssetStorageError> failure) && noexcept
            {
                owner->readError(std::move(failure));
            }

            void set_stopped() && noexcept
            {
                owner->readStopped();
            }

            [[nodiscard]] stdexec::empty_env get_env() const noexcept
            {
                return {};
            }

            Owner* owner{};
        };

        template <class ConcreteAsset>
        class AssetLoadSender final
        {
            static_assert(std::is_base_of_v<lux::asset::Asset, ConcreteAsset>);

        public:
            using sender_concept = stdexec::sender_t;
            using completion_signatures = stdexec::completion_signatures<
                stdexec::set_value_t(std::shared_ptr<const ConcreteAsset>),
                stdexec::set_error_t(AssetLoadFailure),
                stdexec::set_stopped_t()
            >;

            AssetLoadSender(
                AssetReadPort read,
                lux::asset::AssetId id,
                lux::asset::AssetDecodeLimits limits
            ) noexcept
                : read_(std::move(read)), id_(id), limits_(limits)
            {
            }

            template <class Receiver>
            class State final
            {
            public:
                using operation_state_concept = stdexec::operation_state_t;
                using ReadReceiver = AssetReadReceiver<State>;
                using ReadSender = decltype(lux::process::portSender(
                    std::declval<AssetReadPort>(),
                    std::declval<ReadAssetImage>()
                ));
                using ReadState = decltype(stdexec::connect(
                    std::declval<ReadSender>(),
                    std::declval<ReadReceiver>()
                ));

                State(
                    AssetReadPort read,
                    lux::asset::AssetId id,
                    lux::asset::AssetDecodeLimits limits,
                    Receiver receiver
                )
                    : receiver_(std::move(receiver)),
                      id_(id),
                      limits_(limits),
                      read_state_(stdexec::connect(
                          lux::process::portSender(std::move(read), ReadAssetImage{id}),
                          ReadReceiver{this}
                      ))
                {
                }

                State(const State&) = delete;
                State& operator=(const State&) = delete;
                State(State&&) = delete;
                State& operator=(State&&) = delete;

                void start() & noexcept
                {
                    const auto token = stdexec::get_stop_token(stdexec::get_env(receiver_));
                    if (token.stop_requested())
                    {
                        stdexec::set_stopped(std::move(receiver_));
                        return;
                    }
                    if (id_.isNull())
                    {
                        stdexec::set_error(
                            std::move(receiver_),
                            AssetLoadFailure{EAssetLoadError::INVALID_ASSET_ID}
                        );
                        return;
                    }
                    stdexec::start(read_state_);
                }

                void readValue(lux::asset::AssetBlob value) noexcept
                {
                    const auto token = stdexec::get_stop_token(stdexec::get_env(receiver_));
                    if (token.stop_requested())
                    {
                        stdexec::set_stopped(std::move(receiver_));
                        return;
                    }

                    auto decoded = lux::asset::TAssetSerDeser<ConcreteAsset>::decode(
                        id_,
                        std::move(value.bytes),
                        limits_
                    );
                    if (token.stop_requested())
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

                void readError(lux::async::OperationFailure<lux::asset::EAssetStorageError> failure) noexcept
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

            private:
                Receiver receiver_;
                lux::asset::AssetId id_;
                lux::asset::AssetDecodeLimits limits_;
                ReadState read_state_;
            };

            template <class Receiver>
            [[nodiscard]] State<std::decay_t<Receiver>> connect(Receiver&& receiver) &&
            {
                return State<std::decay_t<Receiver>>{
                    std::move(read_),
                    id_,
                    limits_,
                    std::forward<Receiver>(receiver)
                };
            }

            [[nodiscard]] stdexec::empty_env get_env() const noexcept
            {
                return {};
            }

        private:
            AssetReadPort read_;
            lux::asset::AssetId id_;
            lux::asset::AssetDecodeLimits limits_;
        };
    }

    template <class ConcreteAsset>
    [[nodiscard]] auto loadAsset(
        AssetReadPort read,
        lux::asset::AssetId id,
        lux::asset::AssetDecodeLimits limits
    ) noexcept
    {
        return detail::AssetLoadSender<ConcreteAsset>(std::move(read), id, limits);
    }
}
