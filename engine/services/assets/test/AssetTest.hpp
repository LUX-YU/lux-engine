#pragma once

#include <lux/engine/assets/AssetRepository.hpp>
#include <lux/engine/core/execution/ExecutionResult.hpp>
#include <lux/engine/core/execution/TaskScope.hpp>
#include <atomic>
#include <cstring>
#include <lux/engine/resource/asset/CookedAssetImage.hpp>
#include <cassert>
#include <semaphore>
#include <thread>

namespace lux::asset
{
    class DecodeProbe final : public TAsset<std::thread::id>
    {
    public:
        static constexpr auto asset_type = AssetTypeId::fromName("lux.test.decode_probe");
        static constexpr std::uint32_t primary_magic = 1;
        explicit DecodeProbe(AssetId id)
            : TAsset(AssetInfo{.id = id, .type = asset_type}, std::make_shared<const std::thread::id>(std::this_thread::get_id()), {})
        {
        }
        static inline std::atomic<unsigned> decodes{};
    };

    template <> struct TAssetSerDeser<DecodeProbe> final
    {
        static cxx::expected<std::shared_ptr<const DecodeProbe>, AssetDecodeFailure>
        decode(AssetId id, cxx::SharedBytes<> bytes, AssetDecodeLimits limits) noexcept
        {
            ++DecodeProbe::decodes;
            if (bytes.empty() || bytes.data()[0] == std::byte{0xFF})
            {
                return cxx::unexpected(AssetDecodeFailure{EAssetDecodeError::INVALID_PAYLOAD, 0});
            }
            if (bytes.size() > limits.max_decoded_bytes)
            {
                return cxx::unexpected(AssetDecodeFailure{EAssetDecodeError::LIMIT_EXCEEDED, 0});
            }
            return std::make_shared<const DecodeProbe>(id);
        }
    };
}

namespace asset_test
{
    using namespace lux;
    using namespace lux::engine::assets;

    inline const asset::AssetId Id{*uuids::uuid::from_string("fedcba98-7654-3210-fedc-ba9876543210")};

    class Provider;

    class ProbeCodec final : public asset::AssetCodec
    {
    public:
        using Asset = asset::DecodeProbe;
        using Output = std::uint32_t;

        static exec::task<AssetResult<Output>> prepare(
            AssetRepository& assets,
            const std::thread::id&,
            Provider& provider,
            std::vector<AssetDataRef<void>>& dependencies
        ) noexcept;

        asset::AssetTypeId type() const noexcept override { return asset::DecodeProbe::asset_type; }
        std::uint32_t magic() const noexcept override { return asset::DecodeProbe::primary_magic; }
        cxx::type_info dataType() const noexcept override { return cxx::make_basic_type_info<std::thread::id>(); }

        cxx::expected<std::shared_ptr<const asset::Asset>, asset::AssetDecodeFailure>
        decode(asset::AssetId id, cxx::SharedBytes<> bytes, const asset::AssetDecodeLimits& limits) const noexcept override
        {
            auto decoded = asset::TAssetSerDeser<asset::DecodeProbe>::decode(id, std::move(bytes), limits);
            if (!decoded)
            {
                return cxx::unexpected(decoded.error());
            }
            return std::move(*decoded);
        }

        cxx::expected<std::vector<std::byte>, asset::AssetEncodeFailure> encode(
            const asset::AssetInfo&, const void*, std::span<const asset::AssetAuxiliaryPayload>,
            const asset::AssetEncodeLimits&
        ) const noexcept override
        {
            return std::vector<std::byte>(16, std::byte{1});
        }

        const void* data(const asset::Asset& value) const noexcept override
        {
            return &static_cast<const asset::DecodeProbe&>(value).data();
        }
    };

    inline std::vector<std::unique_ptr<asset::AssetCodec>> codecs()
    {
        std::vector<std::unique_ptr<asset::AssetCodec>> result;
        result.push_back(std::make_unique<ProbeCodec>());
        return result;
    }

    template <class Asset, class Identity>
    exec::task<AssetResult<AssetDataRef<typename Asset::data_type>>> readTask(AssetRepository& assets, Identity id)
    {
        auto use = assets.acquire<Asset>(id);
        if (!use)
        {
            co_return cxx::unexpected(use.error());
        }
        co_return co_await assets.read(*use);
    }

    template <class Asset, class Identity> auto read(AssetRepository& assets, Identity id)
    {
        return stdexec::starts_on(execution::ExecutionRuntime::instance().mainScheduler(), readTask<Asset>(assets, id));
    }

    class Provider final : public asset::IAssetProvider
    {
    public:
        std::unordered_map<asset::AssetId, asset::AssetBlob> images;
        mutable std::atomic<unsigned> reads{};
        mutable std::thread::id read_thread;
        bool blocked = false;
        bool fail_io = false;
        bool fail_write = false;
        bool unknown_write = false;
        std::uint32_t uploads = 0;
        std::vector<std::uint32_t> releases;
        std::vector<asset::AssetId> notifications;
        std::vector<asset::AssetId> refreshed;
        std::function<exec::task<AssetResult<void>>(AssetRepository&, std::vector<AssetDataRef<void>>&)> preparation;
        mutable std::binary_semaphore entered{0};
        mutable std::binary_semaphore proceed{0};

        cxx::expected<std::vector<asset::AssetId>, asset::EAssetStorageError>
        refresh(std::span<const std::filesystem::path>) override
        {
            return std::exchange(refreshed, {});
        }

        void contentChanged(asset::AssetId id) noexcept
        {
            assert(execution::ExecutionRuntime::instance().isMainThread());
            notifications.push_back(id);
        }

        exec::task<AssetResult<void>> release(std::uint32_t result) noexcept
        {
            releases.push_back(result);
            co_return AssetResult<void>{};
        }

        cxx::expected<void, asset::EAssetStorageError>
        replace(asset::AssetId id, std::span<const std::byte> bytes) override
        {
            if (fail_write)
            {
                return cxx::unexpected(asset::EAssetStorageError::IO_FAILURE);
            }
            images[id] = {cxx::SharedBytes<>::copyOf(bytes)};
            return unknown_write
                ? cxx::expected<void, asset::EAssetStorageError>{
                      cxx::unexpected(asset::EAssetStorageError::PUBLICATION_UNKNOWN)}
                : cxx::expected<void, asset::EAssetStorageError>{};
        }

        std::optional<asset::AssetId> resolve(std::string_view path) const override
        {
            return path == "mesh" && images.contains(Id) ? std::optional{Id} : std::nullopt;
        }

        bool contains(const asset::AssetId& id) const override { return images.contains(id); }

        cxx::expected<asset::AssetBlob, asset::EAssetStorageError>
        open(const asset::AssetId& id, std::size_t limit) const override
        {
            ++reads;
            read_thread = std::this_thread::get_id();
            if (blocked)
            {
                entered.release();
                // waitUntil parks on the Execution wake source, not on this test semaphore.
                execution::ExecutionRuntime::instance().wake();
                proceed.acquire();
            }
            if (fail_io)
            {
                return cxx::unexpected(asset::EAssetStorageError::IO_FAILURE);
            }
            const auto found = images.find(id);
            if (found == images.end())
            {
                return cxx::unexpected(asset::EAssetStorageError::NOT_FOUND);
            }
            if (found->second.bytes.size() > limit)
            {
                return cxx::unexpected(asset::EAssetStorageError::LIMIT_EXCEEDED);
            }
            return found->second;
        }

        void enumerate(const std::function<void(const asset::ProviderEntry&)>& visit) const override
        {
            for (const auto& [id, image] : images)
            {
                std::uint32_t magic = asset::DecodeProbe::primary_magic;
                if (image.bytes.size() >= 8)
                {
                    std::uint32_t version;
                    std::memcpy(&version, image.bytes.data() + 4, sizeof(version));
                    if (version == asset::kCookedAssetVersionV2)
                    {
                        std::memcpy(&magic, image.bytes.data(), sizeof(magic));
                    }
                }
                visit(asset::ProviderEntry{id, magic, "mesh", false});
            }
        }

        [[nodiscard]] lux::cxx::expected<lux::asset::ProviderEntry, lux::asset::EAssetStorageError>
        describe(lux::asset::AssetId id) const override
        {
            lux::cxx::expected<lux::asset::ProviderEntry, lux::asset::EAssetStorageError> found =
                lux::cxx::unexpected(lux::asset::EAssetStorageError::NOT_FOUND);
            enumerate([&](const lux::asset::ProviderEntry& entry)
            {
                if (entry.id == id)
                {
                    found = entry;
                }
            });
            return found;
        }

        std::optional<std::string> pathOf(const asset::AssetId& id) const override
        {
            return contains(id) ? std::optional<std::string>{"mesh"} : std::nullopt;
        }
    };

    inline exec::task<AssetResult<ProbeCodec::Output>> ProbeCodec::prepare(
        AssetRepository& assets,
        const std::thread::id&,
        Provider& provider,
        std::vector<AssetDataRef<void>>& dependencies
    ) noexcept
    {
        if (provider.preparation)
        {
            if (auto prepared = co_await provider.preparation(assets, dependencies); !prepared)
            {
                co_return cxx::unexpected(prepared.error());
            }
        }
        co_return ++provider.uploads;
    }

    template <class T, class Sender>
    auto wait(execution::ExecutionRuntime& execution, Sender sender)
    {
        std::optional<typename execution::detail::ExecutionTypes<Sender>::Result> result;
        assert(execution.spawn(stdexec::then(
            execution::asResult(stdexec::starts_on(execution.mainScheduler(), std::move(sender))),
            [&](auto value) noexcept
        {
            result.emplace(std::move(value));
        })));
        assert(execution.waitUntil([&]() noexcept { return result.has_value(); }));
        return std::move(*result);
    }

    inline asset::AssetBlob image(std::byte value = std::byte{1})
    {
        const std::array<std::byte, 16> bytes{value};
        return {cxx::SharedBytes<>::copyOf(bytes)};
    }
}
