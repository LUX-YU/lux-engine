#pragma once

#include <chrono>
#include <exec/task.hpp>
#include <functional>
#include <lux/engine/assets/AssetDataRef.hpp>
#include <lux/engine/assets/AssetRef.hpp>
#include <lux/engine/assets/AssetRetentionPolicy.hpp>
#include <lux/engine/assets/AssetUsage.hpp>
#include <lux/engine/assets/visibility.h>
#include <lux/engine/core/execution/ExecutionRuntime.hpp>
#include <lux/engine/error/Error.hpp>
#include <lux/engine/resource/asset/AssetCodec.hpp>
#include <lux/engine/resource/asset/storage/AssetVfs.hpp>
#include <memory>
#include <optional>
#include <vector>

namespace lux::platform
{
    class FileWatcher;
    struct FileEvent;
} // namespace lux::platform

namespace lux::engine::assets
{
    template <class T> using AssetResult = cxx::expected<T, error::Error>;

    inline constexpr auto kAssetRepositoryClosed = error::errorId("lux.assets.repository.closed");
    inline constexpr auto kAssetTypeMismatch = error::errorId("lux.assets.type_mismatch");
    inline constexpr auto kAssetCancelled = error::errorId("lux.assets.request.cancelled");
    inline constexpr auto kAssetReadFailure = error::errorId("lux.assets.read");
    inline constexpr auto kAssetDecodeFailure = error::errorId("lux.assets.decode");
    inline constexpr auto kAssetWriteFailure = error::errorId("lux.assets.write");
    inline constexpr auto kAssetEncodeFailure = error::errorId("lux.assets.encode");
    inline constexpr auto kAssetBusy = error::errorId("lux.assets.busy");
    inline constexpr auto kAssetChanged = error::errorId("lux.assets.changed");

    // Main owns identity/cache publication. Blocking IO and codecs run on the shared ExecutionRuntime.
    // Main-thread outer destruction settles accepted work and releases unused results.
    // All external references and borrowed providers must obey this owner boundary.
    class LUX_ENGINE_ASSETS_PUBLIC AssetRepository final
    {
    public:
        using CreateResult = AssetResult<std::unique_ptr<AssetRepository>>;

        [[nodiscard]] static CreateResult create(
            asset::AssetVfsView source,
            asset::AssetDecodeLimits limits,
            std::unique_ptr<AssetRetentionPolicy> policy = {},
            std::vector<std::unique_ptr<asset::AssetCodec>> additional_codecs = {}
        ) noexcept;

        ~AssetRepository() noexcept;
        AssetRepository(const AssetRepository&) = delete;
        AssetRepository& operator=(const AssetRepository&) = delete;

        // Cold identity lookup. A reference keeps asset usage, not decoded CPU data.
        template <class Asset> [[nodiscard]] AssetResult<AssetRef<Asset>> acquire(asset::AssetId id) noexcept
        {
            auto record = acquireRecord(id, Asset::asset_type, cxx::make_basic_type_info<typename Asset::data_type>());
            if (!record)
            {
                return cxx::unexpected(record.error());
            }
            return AssetRef<Asset>{**record};
        }

        template <class Asset> [[nodiscard]] AssetResult<AssetRef<Asset>> acquire(std::string_view path) noexcept
        {
            return acquire<Asset>(resolve(path));
        }

        // Main only, for a live reference from this repository. The reference and receiver outlive the
        // connection. Callbacks only invalidate consumer inputs: no repository/Registry reentry or IO.
        template <auto Callback, class Asset, class Receiver>
        [[nodiscard]] entt::scoped_connection
        observe(const AssetRef<Asset>& asset, Receiver& receiver) noexcept
        {
            (void)revision(asset);
            return entt::sink{detail::assetContentChanged(*asset.record_)}.template connect<Callback>(receiver);
        }

        // Main-only content qualification. Old snapshots remain readable after becoming non-current.
        template <class Asset>
        [[nodiscard]] std::uint64_t revision(const AssetRef<Asset>& asset) const noexcept
        {
            return revision(*asset.record_);
        }

        template <class Data>
        [[nodiscard]] std::uint64_t revision(const AssetDataRef<Data>& snapshot) const noexcept
        {
            return revision(*snapshot.record_);
        }

        template <class Data>
        [[nodiscard]] bool isCurrent(const AssetDataRef<Data>& snapshot) const noexcept
        {
            return isCurrent(*snapshot.record_);
        }

        template <class Asset> [[nodiscard]] auto read(const AssetRef<Asset>& asset) noexcept
        {
            using Data = typename Asset::data_type;
            // Identity is captured by value before suspension; no borrow of the caller's reference variable.
            return stdexec::then(
                stdexec::starts_on(
                    execution::ExecutionRuntime::instance().mainScheduler(),
                    readData(AssetRef<asset::Asset>{asset})
                ),
                [](auto value) noexcept -> AssetResult<AssetDataRef<Data>>
                {
                    if (!value)
                    {
                        return cxx::unexpected(value.error());
                    }
                    return AssetDataRef<Data>{*value->record_};
                }
            );
        }

        template <class Asset>
        [[nodiscard]] auto write(const AssetRef<Asset>& asset, const typename Asset::data_type& data) noexcept
        {
            return stdexec::starts_on(
                execution::ExecutionRuntime::instance().mainScheduler(),
                writeData(AssetRef<asset::Asset>{asset}, &data)
            );
        }

        // Owns a source reference through completion; only the requested range is read on blocking IO.
        template <class Data>
        [[nodiscard]] auto
        readRange(const AssetDataRef<Data>& source, std::uint64_t offset, std::uint64_t size) noexcept
        {
            return stdexec::starts_on(
                execution::ExecutionRuntime::instance().mainScheduler(),
                readSourceRange(AssetDataRef<void>{*source.record_}, offset, size)
            );
        }

        // The accepted operation and source borrow run to their real publication outcome.
        // operation must publish last; the Writer borrow cannot escape its coroutine.
        template <class Asset>
        [[nodiscard]] auto withContentWriter(
            const AssetRef<Asset>& asset,
            const AssetDataRef<typename Asset::data_type>& base,
            asset::EAssetContentWrite mode,
            std::function<exec::task<AssetResult<void>>(asset::AssetContentWriter&)> operation
        ) noexcept
        {
            return stdexec::starts_on(
                execution::ExecutionRuntime::instance().mainScheduler(),
                stdexec::write_env(
                    runContentWrite(
                        AssetRef<asset::Asset>{asset},
                        AssetDataRef<void>{base},
                        mode,
                        {},
                        std::move(operation)
                    ),
                    stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
                )
            );
        }

        template <class Asset>
        [[nodiscard]] auto createContent(
            std::string path,
            asset::AssetInfo info,
            std::function<exec::task<AssetResult<void>>(asset::AssetContentWriter&)> operation
        ) noexcept
        {
            return stdexec::then(
                stdexec::starts_on(
                    execution::ExecutionRuntime::instance().mainScheduler(),
                    stdexec::write_env(
                        createContentRecord(
                            std::move(path),
                            std::move(info),
                            Asset::asset_type,
                            cxx::make_basic_type_info<typename Asset::data_type>(),
                            std::move(operation)
                        ),
                        stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
                    )
                ),
                [](AssetResult<AssetRef<asset::Asset>> created) noexcept -> AssetResult<AssetRef<Asset>>
                {
                    if (!created)
                    {
                        return cxx::unexpected(created.error());
                    }
                    return AssetRef<Asset>{*created->record_};
                }
            );
        }

        // Cold registration borrows Providers and dependencies through repository retirement, including
        // failed release retries. Their addresses must remain stable; never register a temporary or Reaction.
        // The professional assembly owns them before Context and destroys them after Context's settlement.
        // Provider destructors must not access the already-destroyed Context. Registration acquires no ownership.
        // Definition::prepare may use other registered purposes, but must not recursively request itself.
        template <class Definition, class Provider, class... Dependencies>
        [[nodiscard]] AssetResult<void> registerUse(Provider& provider, Dependencies&... dependencies) noexcept
        {
            using Asset = typename Definition::Asset;
            using Data = typename Asset::data_type;
            using Output = typename Definition::Output;
            return registerPreparation(
                cxx::make_basic_type_info<Definition>(),
                &provider,
                [this,
                 &provider,
                 &dependencies...](detail::AssetRecord& asset, detail::AssetDataRecord& record) noexcept -> Operation
                {
                    configureUse(
                        record,
                        cxx::make_basic_type_info<Output>(),
                        +[](const void* value) noexcept { delete static_cast<const Output*>(value); },
                        [&provider](const void* value) noexcept -> Operation
                        { co_return co_await provider.release(*static_cast<const Output*>(value)); }
                    );
                    auto loaded = co_await readRecord(asset);
                    if (!loaded)
                    {
                        co_return cxx::unexpected(loaded.error());
                    }
                    if (detail::dataType(*loaded->record_) != cxx::make_basic_type_info<Data>())
                    {
                        co_return cxx::unexpected(error::Error{kAssetTypeMismatch});
                    }
                    auto preparation = [&]() noexcept
                    {
                        if constexpr (requires { &Definition::prepare; })
                        {
                            // An explicitly declared prepare must satisfy its real call signature.
                            return Definition::prepare(
                                *this,
                                *static_cast<const Data*>(detail::dataAddress(*loaded->record_)),
                                provider,
                                dependencies...,
                                useDependencies(record)
                            );
                        }
                        else
                        {
                            static_assert(std::same_as<Data, typename Provider::Input>);
                            static_assert(sizeof...(Dependencies) == 0);
                            return provider.upload(*static_cast<const Data*>(detail::dataAddress(*loaded->record_)));
                        }
                    };
                    auto result = co_await preparation();
                    if (!result)
                    {
                        co_return cxx::unexpected(result.error());
                    }
                    co_return adoptUseData(record, new Output(std::move(*result)));
                }
            );
        }

        template <class Definition, class Provider>
        [[nodiscard]] auto use(const AssetRef<typename Definition::Asset>& asset, Provider& provider) noexcept
        {
            using Output = typename Definition::Output;
            return stdexec::then(
                stdexec::starts_on(
                    execution::ExecutionRuntime::instance().mainScheduler(),
                    useData(AssetRef<asset::Asset>{asset}, cxx::make_basic_type_info<Definition>(), &provider)
                ),
                [](AssetResult<AssetDataRef<void>> result) noexcept -> AssetResult<AssetDataRef<Output>>
                {
                    if (!result)
                    {
                        return cxx::unexpected(result.error());
                    }
                    if (detail::dataType(*result->record_) != cxx::make_basic_type_info<Output>())
                    {
                        return cxx::unexpected(error::Error{kAssetTypeMismatch});
                    }
                    return AssetDataRef<Output>{*result->record_};
                }
            );
        }

        // Main-thread observation; the callback must not mutate or reenter the repository.
        void forEachUse(
            asset::AssetId asset,
            const std::function<void(cxx::type_info, const void*, std::size_t, bool, bool)>& visitor
        ) const noexcept;

        [[nodiscard]] asset::AssetId resolve(std::string_view path) const noexcept;
        [[nodiscard]] std::optional<AssetUsage> usage(asset::AssetId id) const noexcept;
        [[nodiscard]] const asset::AssetDecodeLimits& decodeLimits() const noexcept;
        void collect() noexcept;

        // Event bytes are borrowed until completion. FileWatcher itself keeps its synchronous API.
        [[nodiscard]] exec::task<AssetResult<void>> refresh(std::span<const platform::FileEvent> events) noexcept;
        [[nodiscard]] AssetResult<void>
        watch(platform::FileWatcher& watcher, std::chrono::milliseconds period) noexcept;

        [[nodiscard]] bool empty() const noexcept;

    private:
        [[nodiscard]] std::uint64_t revision(const detail::AssetRecord& record) const noexcept;
        [[nodiscard]] std::uint64_t revision(const detail::AssetDataRecord& record) const noexcept;
        [[nodiscard]] bool isCurrent(const detail::AssetDataRecord& record) const noexcept;
        [[nodiscard]] exec::task<AssetResult<void>> settleAndRelease() noexcept;
        struct Impl;
        explicit AssetRepository(std::unique_ptr<Impl> impl) noexcept;
        [[nodiscard]] AssetResult<detail::AssetRecord*>
        acquireRecord(asset::AssetId, asset::AssetTypeId, cxx::type_info data_type) noexcept;
        [[nodiscard]] exec::task<AssetResult<AssetDataRef<asset::Asset>>>
        readData(AssetRef<asset::Asset> asset) noexcept;
        [[nodiscard]] exec::task<AssetResult<void>> writeData(AssetRef<asset::Asset> asset, const void* data) noexcept;
        [[nodiscard]] exec::task<AssetResult<cxx::SharedBytes<>>>
        readSourceRange(AssetDataRef<void> source, std::uint64_t offset, std::uint64_t size) noexcept;
        [[nodiscard]] exec::task<AssetResult<void>> runContentWrite(
            AssetRef<asset::Asset> asset,
            AssetDataRef<void> base,
            asset::EAssetContentWrite mode,
            std::string path,
            std::function<exec::task<AssetResult<void>>(asset::AssetContentWriter&)> operation
        ) noexcept;
        [[nodiscard]] exec::task<AssetResult<AssetRef<asset::Asset>>> createContentRecord(
            std::string path,
            asset::AssetInfo info,
            asset::AssetTypeId type,
            cxx::type_info data_type,
            std::function<exec::task<AssetResult<void>>(asset::AssetContentWriter&)> operation
        ) noexcept;
        [[nodiscard]] AssetResult<void> finishContentWrite(
            detail::AssetRecord& record,
            asset::EAssetPublication publication,
            AssetResult<void> outcome
        ) noexcept;
        using Operation = exec::task<AssetResult<void>>;
        using Release = std::function<Operation(const void*)>;
        static void configureUse(
            detail::AssetDataRecord& record,
            cxx::type_info type,
            void (*destroy)(const void*) noexcept,
            Release release
        ) noexcept;
        static std::vector<AssetDataRef<void>>& useDependencies(detail::AssetDataRecord& record) noexcept;
        [[nodiscard]] AssetResult<void> adoptUseData(detail::AssetDataRecord& record, const void* data) noexcept;
        using Prepare = std::function<Operation(detail::AssetRecord&, detail::AssetDataRecord&)>;
        [[nodiscard]] AssetResult<void>
        registerPreparation(cxx::type_info type, const void* provider, Prepare) noexcept;
        [[nodiscard]] exec::task<AssetResult<AssetDataRef<asset::Asset>>> readRecord(detail::AssetRecord&) noexcept;
        [[nodiscard]] exec::task<AssetResult<AssetDataRef<void>>>
        useData(AssetRef<asset::Asset> asset, cxx::type_info type, const void* provider) noexcept;
        std::unique_ptr<Impl> impl_;
    };
} // namespace lux::engine::assets
