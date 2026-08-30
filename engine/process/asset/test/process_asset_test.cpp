#include <lux/engine/process/asset/AssetLoadSender.hpp>
#include <lux/engine/resource/asset/material/MaterialAssets.hpp>
#include <lux/engine/resource/asset/model/ModelAsset.hpp>
#include <lux/engine/resource/asset/texture/TextureAsset.hpp>

#include <Eigen/Geometry>

#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <stdexec/execution.hpp>
#include <utility>
#include <vector>

namespace
{
    [[nodiscard]] lux::asset::AssetId id(std::uint8_t tail)
    {
        std::array<std::uint8_t, 16U> bytes{};
        bytes.back() = tail;
        return lux::asset::AssetId{bytes};
    }

    template <class Asset>
    [[nodiscard]] lux::asset::AssetInfo info(std::uint8_t tail)
    {
        return lux::asset::AssetInfo{id(tail), Asset::asset_type};
    }

    [[nodiscard]] std::shared_ptr<const lux::asset::TextureAsset> makeTexture(std::uint8_t tail)
    {
        lux::rdesc::TextureInfo texture_info{};
        texture_info.width = 1;
        texture_info.height = 1;
        texture_info.channel = 4;
        texture_info.pixel_format = lux::rdesc::ETexturePixelFormat::RGBA8_UNORM;
        texture_info.color_space = lux::rdesc::ETextureColorSpace::LINEAR;
        texture_info.mip_ranges[0] = {0U, 4U, 1U, 1U};
        constexpr std::array pixels{
            std::byte{0x11U}, std::byte{0x22U}, std::byte{0x33U}, std::byte{0xFFU}
        };
        auto texture = lux::rdesc::Texture::copyOf(texture_info, pixels);
        assert(texture);
        auto asset = lux::asset::TextureAsset::create(
            info<lux::asset::TextureAsset>(tail),
            std::make_shared<const lux::rdesc::Texture>(std::move(*texture))
        );
        assert(asset);
        return *asset;
    }

    [[nodiscard]] std::shared_ptr<const lux::asset::MaterialAsset> makeMaterial(std::uint8_t tail)
    {
        auto description = std::make_shared<lux::rdesc::MaterialDescription>();
        description->gbuffer_spirv = {0x07230203U, 0x00010000U, 0U, 1U, 0U};
        description->forward_spirv = description->gbuffer_spirv;
        auto asset = lux::asset::MaterialAsset::create(
            info<lux::asset::MaterialAsset>(tail),
            std::move(description)
        );
        assert(asset);
        return *asset;
    }

    [[nodiscard]] std::shared_ptr<const lux::asset::ModelAsset> makeModel(std::uint8_t tail)
    {
        auto description = std::make_shared<lux::rdesc::ModelDescription>();
        description->root_node = 0U;
        description->primitives.push_back({id(90U), id(91U)});
        lux::rdesc::ModelNode root;
        root.local_transform = Eigen::Affine3f::Identity();
        root.primitives.push_back(0U);
        description->nodes.push_back(std::move(root));
        auto asset = lux::asset::ModelAsset::create(
            info<lux::asset::ModelAsset>(tail),
            std::move(description)
        );
        assert(asset);
        return *asset;
    }

    template <class Asset>
    [[nodiscard]] lux::asset::AssetBlob encode(const std::shared_ptr<const Asset>& asset)
    {
        const auto encoded = lux::asset::TAssetSerDeser<Asset>::encode(
            *asset,
            lux::asset::AssetEncodeLimits{1024U * 1024U}
        );
        assert(encoded);
        return lux::asset::AssetBlob::fromShared(lux::cxx::SharedBytes<>::copyOf(*encoded));
    }

    enum class EMode : std::uint8_t
    {
        IMMEDIATE,
        DEFERRED,
        STORAGE_FAILURE,
        REJECT,
    };

    class Endpoint final
        : public lux::async::OperationPort<lux::process::asset::ReadAssetImage>::Endpoint
    {
    public:
        Endpoint(EMode mode, lux::asset::AssetBlob blob) noexcept : mode_(mode), blob_(std::move(blob))
        {
        }

        [[nodiscard]] lux::async::SubmitResult submit(
            lux::process::asset::ReadAssetImage operation,
            void* state,
            void (*complete)(void*, Outcome&&) noexcept,
            lux::async::SubmitOptions options
        ) noexcept override
        {
            ++submits;
            requested = operation.id;
            accounted_bytes = options.accounted_bytes;
            if (mode_ == EMode::REJECT)
            {
                return lux::cxx::unexpected(lux::async::ESubmitError::QUEUE_FULL);
            }
            if (mode_ == EMode::STORAGE_FAILURE)
            {
                complete(
                    state,
                    lux::cxx::unexpected(
                        lux::async::OperationFailure<lux::asset::EAssetStorageError>::domain(
                            lux::asset::EAssetStorageError::IO_FAILURE
                        )
                    )
                );
                return {};
            }
            if (mode_ == EMode::DEFERRED)
            {
                state_ = state;
                complete_ = complete;
                return {};
            }
            complete(state, Outcome{std::move(blob_)});
            return {};
        }

        void finish() noexcept
        {
            assert(complete_ != nullptr);
            const auto complete = std::exchange(complete_, nullptr);
            const auto state = std::exchange(state_, nullptr);
            complete(state, Outcome{std::move(blob_)});
        }

        std::size_t submits{};
        std::size_t accounted_bytes{};
        lux::asset::AssetId requested;

    private:
        EMode mode_{};
        lux::asset::AssetBlob blob_;
        void* state_{};
        void (*complete_)(void*, Outcome&&) noexcept{};
    };

    template <class Asset>
    struct Result final
    {
        std::shared_ptr<const Asset> value;
        std::optional<lux::process::asset::AssetLoadFailure> error;
        std::atomic_size_t completions{};
        bool stopped{};
    };

    template <class Asset>
    struct Receiver final
    {
        using receiver_concept = stdexec::receiver_t;

        void set_value(std::shared_ptr<const Asset> value) && noexcept
        {
            result->value = std::move(value);
            result->completions.fetch_add(1U, std::memory_order_release);
        }

        void set_error(lux::process::asset::AssetLoadFailure error) && noexcept
        {
            result->error = std::move(error);
            result->completions.fetch_add(1U, std::memory_order_release);
        }

        void set_stopped() && noexcept
        {
            result->stopped = true;
            result->completions.fetch_add(1U, std::memory_order_release);
        }

        [[nodiscard]] auto get_env() const noexcept
        {
            return stdexec::prop{stdexec::get_stop_token, stop};
        }

        Result<Asset>* result{};
        stdexec::inplace_stop_token stop;
    };

    template <class Asset>
    void verifyImmediate(const std::shared_ptr<const Asset>& expected)
    {
        auto endpoint = std::make_shared<Endpoint>(EMode::IMMEDIATE, encode(expected));
        Result<Asset> result;
        stdexec::inplace_stop_source stop;
        auto state = stdexec::connect(
            lux::process::asset::loadAsset<Asset>(
                lux::process::asset::AssetReadPort{endpoint},
                expected->id(),
                lux::asset::AssetDecodeLimits{1024U * 1024U, 1024U * 1024U, 4U}
            ),
            Receiver<Asset>{&result, stop.get_token()}
        );
        stdexec::start(state);
        assert(result.completions.load(std::memory_order_acquire) == 1U);
        assert(result.value && result.value->id() == expected->id());
        assert(!result.error && !result.stopped);
        assert(endpoint->requested == expected->id());
        assert(endpoint->accounted_bytes == 0U);
    }
}

int main()
{
    const auto texture = makeTexture(1U);
    const auto material = makeMaterial(2U);
    const auto model = makeModel(3U);
    verifyImmediate(texture);
    verifyImmediate(material);
    verifyImmediate(model);

    auto deferred_endpoint = std::make_shared<Endpoint>(EMode::DEFERRED, encode(texture));
    Result<lux::asset::TextureAsset> deferred;
    stdexec::inplace_stop_source deferred_stop;
    auto deferred_state = stdexec::connect(
        lux::process::asset::loadAsset<lux::asset::TextureAsset>(
            lux::process::asset::AssetReadPort{deferred_endpoint},
            texture->id(),
            lux::asset::AssetDecodeLimits{1024U * 1024U, 1024U * 1024U, 4U}
        ),
        Receiver<lux::asset::TextureAsset>{&deferred, deferred_stop.get_token()}
    );
    stdexec::start(deferred_state);
    assert(deferred.completions.load(std::memory_order_acquire) == 0U);
    deferred_endpoint->finish();
    assert(deferred.completions.load(std::memory_order_acquire) == 1U);
    assert(deferred.value && deferred.value->data().pixels().size() == 4U);

    Result<lux::asset::TextureAsset> wrong_type;
    stdexec::inplace_stop_source wrong_type_stop;
    auto wrong_type_state = stdexec::connect(
        lux::process::asset::loadAsset<lux::asset::TextureAsset>(
            lux::process::asset::AssetReadPort{
                std::make_shared<Endpoint>(EMode::IMMEDIATE, encode(material))
            },
            material->id(),
            lux::asset::AssetDecodeLimits{1024U * 1024U, 1024U * 1024U, 4U}
        ),
        Receiver<lux::asset::TextureAsset>{&wrong_type, wrong_type_stop.get_token()}
    );
    stdexec::start(wrong_type_state);
    assert(wrong_type.error && wrong_type.error->code == lux::process::asset::EAssetLoadError::DECODE_FAILURE);

    Result<lux::asset::TextureAsset> mismatched;
    stdexec::inplace_stop_source mismatched_stop;
    auto mismatched_state = stdexec::connect(
        lux::process::asset::loadAsset<lux::asset::TextureAsset>(
            lux::process::asset::AssetReadPort{
                std::make_shared<Endpoint>(EMode::IMMEDIATE, encode(texture))
            },
            id(77U),
            lux::asset::AssetDecodeLimits{1024U * 1024U, 1024U * 1024U, 4U}
        ),
        Receiver<lux::asset::TextureAsset>{&mismatched, mismatched_stop.get_token()}
    );
    stdexec::start(mismatched_state);
    assert(mismatched.error);
    assert(mismatched.error->decode.code == lux::asset::EAssetDecodeError::ASSET_ID_MISMATCH);

    const auto complete_blob = encode(texture);
    auto truncated_blob = lux::asset::AssetBlob::fromShared(
        lux::cxx::SharedBytes<>::copyOf(complete_blob.bytes.view().first(8U))
    );
    Result<lux::asset::TextureAsset> truncated;
    stdexec::inplace_stop_source truncated_stop;
    auto truncated_state = stdexec::connect(
        lux::process::asset::loadAsset<lux::asset::TextureAsset>(
            lux::process::asset::AssetReadPort{
                std::make_shared<Endpoint>(EMode::IMMEDIATE, std::move(truncated_blob))
            },
            texture->id(),
            lux::asset::AssetDecodeLimits{1024U * 1024U, 1024U * 1024U, 4U}
        ),
        Receiver<lux::asset::TextureAsset>{&truncated, truncated_stop.get_token()}
    );
    stdexec::start(truncated_state);
    assert(truncated.error && truncated.error->decode.code == lux::asset::EAssetDecodeError::TRUNCATED);

    Result<lux::asset::TextureAsset> limited;
    stdexec::inplace_stop_source limited_stop;
    auto limited_state = stdexec::connect(
        lux::process::asset::loadAsset<lux::asset::TextureAsset>(
            lux::process::asset::AssetReadPort{
                std::make_shared<Endpoint>(EMode::IMMEDIATE, encode(texture))
            },
            texture->id(),
            lux::asset::AssetDecodeLimits{1U, 1U, 0U}
        ),
        Receiver<lux::asset::TextureAsset>{&limited, limited_stop.get_token()}
    );
    stdexec::start(limited_state);
    assert(limited.error && limited.error->decode.code == lux::asset::EAssetDecodeError::LIMIT_EXCEEDED);

    Result<lux::asset::TextureAsset> rejected;
    stdexec::inplace_stop_source rejected_stop;
    auto rejected_state = stdexec::connect(
        lux::process::asset::loadAsset<lux::asset::TextureAsset>(
            lux::process::asset::AssetReadPort{
                std::make_shared<Endpoint>(EMode::REJECT, lux::asset::AssetBlob{})
            },
            texture->id(),
            lux::asset::AssetDecodeLimits{1024U, 1024U, 0U}
        ),
        Receiver<lux::asset::TextureAsset>{&rejected, rejected_stop.get_token()}
    );
    stdexec::start(rejected_state);
    assert(rejected.error && rejected.error->code == lux::process::asset::EAssetLoadError::SUBMIT_FAILURE);
    assert(rejected.error->submit_error == lux::async::ESubmitError::QUEUE_FULL);

    Result<lux::asset::TextureAsset> storage_failure;
    stdexec::inplace_stop_source storage_stop;
    auto storage_state = stdexec::connect(
        lux::process::asset::loadAsset<lux::asset::TextureAsset>(
            lux::process::asset::AssetReadPort{
                std::make_shared<Endpoint>(EMode::STORAGE_FAILURE, lux::asset::AssetBlob{})
            },
            texture->id(),
            lux::asset::AssetDecodeLimits{1024U, 1024U, 0U}
        ),
        Receiver<lux::asset::TextureAsset>{&storage_failure, storage_stop.get_token()}
    );
    stdexec::start(storage_state);
    assert(storage_failure.error);
    assert(storage_failure.error->code == lux::process::asset::EAssetLoadError::STORAGE_FAILURE);
    assert(storage_failure.error->storage_error == lux::asset::EAssetStorageError::IO_FAILURE);

    Result<lux::asset::TextureAsset> invalid;
    stdexec::inplace_stop_source invalid_stop;
    auto invalid_state = stdexec::connect(
        lux::process::asset::loadAsset<lux::asset::TextureAsset>(
            {},
            {},
            lux::asset::AssetDecodeLimits{1024U, 1024U, 0U}
        ),
        Receiver<lux::asset::TextureAsset>{&invalid, invalid_stop.get_token()}
    );
    stdexec::start(invalid_state);
    assert(invalid.error && invalid.error->code == lux::process::asset::EAssetLoadError::INVALID_ASSET_ID);

    Result<lux::asset::TextureAsset> stopped;
    stdexec::inplace_stop_source stop;
    static_cast<void>(stop.request_stop());
    auto stopped_state = stdexec::connect(
        lux::process::asset::loadAsset<lux::asset::TextureAsset>(
            lux::process::asset::AssetReadPort{
                std::make_shared<Endpoint>(EMode::IMMEDIATE, encode(texture))
            },
            texture->id(),
            lux::asset::AssetDecodeLimits{1024U * 1024U, 1024U * 1024U, 4U}
        ),
        Receiver<lux::asset::TextureAsset>{&stopped, stop.get_token()}
    );
    stdexec::start(stopped_state);
    assert(stopped.stopped && stopped.completions.load(std::memory_order_acquire) == 1U);

    auto late_endpoint = std::make_shared<Endpoint>(EMode::DEFERRED, encode(texture));
    Result<lux::asset::TextureAsset> late_stopped;
    stdexec::inplace_stop_source late_stop;
    auto late_state = stdexec::connect(
        lux::process::asset::loadAsset<lux::asset::TextureAsset>(
            lux::process::asset::AssetReadPort{late_endpoint},
            texture->id(),
            lux::asset::AssetDecodeLimits{1024U * 1024U, 1024U * 1024U, 4U}
        ),
        Receiver<lux::asset::TextureAsset>{&late_stopped, late_stop.get_token()}
    );
    stdexec::start(late_state);
    static_cast<void>(late_stop.request_stop());
    late_endpoint->finish();
    assert(late_stopped.stopped);
    assert(late_stopped.completions.load(std::memory_order_acquire) == 1U);
    return 0;
}
