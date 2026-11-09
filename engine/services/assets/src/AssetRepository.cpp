#include <lux/engine/assets/AssetRepository.hpp>

#include <atomic>
#include <exec/create.hpp>
#include <exec/split.hpp>
#include <lux/engine/core/execution/CompletionWork.hpp>
#include <lux/engine/core/execution/ExecutionResult.hpp>
#include <lux/engine/core/execution/TaskScope.hpp>
#include <lux/engine/error/ErrorRegistry.hpp>
#include <lux/engine/log/Log.hpp>
#if defined(LUX_ASSET_FILEWATCH)
#include <lux/engine/filewatch/FileWatcher.hpp>
#endif
#include <algorithm>
#include <limits>
#include <lux/cxx/core/scope_exit.hpp>
#include <unordered_map>
#include <unordered_set>

namespace lux::engine::assets
{
    namespace
    {
        constexpr error::ErrorDescriptor errors[]{
            {"lux.assets.repository.closed", "Asset repository is closing"},
            {"lux.assets.type_mismatch", "Asset type does not match"},
            {"lux.assets.request.cancelled", "Asset operation was cancelled"},
            {"lux.assets.read", "Asset storage error {0}", error::ERecovery::NEEDS_INPUT, {error::EArgument::UNSIGNED}},
            {"lux.assets.decode",
             "Asset decode error {0} at byte {1}",
             error::ERecovery::NEEDS_INPUT,
             {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
            {"lux.assets.write",
             "Asset replacement error {0}",
             error::ERecovery::NEEDS_INPUT,
             {error::EArgument::UNSIGNED}},
            {"lux.assets.encode",
             "Asset encode error {0} at byte {1}",
             error::ERecovery::NEEDS_INPUT,
             {error::EArgument::UNSIGNED, error::EArgument::UNSIGNED}},
            {"lux.assets.busy", "Asset write, directory watcher or use registration already active"},
            {"lux.assets.changed", "Asset source changed before the operation could be adopted"}
        };

        error::Error assetError(const execution::ExecutionFailure<error::Error>& failure) noexcept
        {
            if (const auto* domain = failure.domainFailure())
            {
                return *domain;
            }
            if (const auto* scheduling = failure.executionFailure())
            {
                return execution::toError(*scheduling);
            }
            return {kAssetCancelled};
        }

        AssetResult<void> normalize(execution::ExecutionResult<void, error::Error> result) noexcept
        {
            return result ? AssetResult<void>{} : AssetResult<void>{cxx::unexpected(assetError(result.error()))};
        }

        AssetResult<void> copyOutcome(const AssetResult<void>& result) noexcept
        {
            return result;
        }
    } // namespace

    namespace detail
    {
        // Stable records owned by the repository. Last-reference notifications only schedule Main work.
        struct AssetRecord
        {
            asset::AssetId id;
            std::atomic<std::size_t> references{0};
            std::atomic<std::size_t> data_readers{0};
            std::atomic<std::size_t> use_readers{0};
            execution::CompletionWork::Request changed;
            entt::sigh<void(asset::AssetId)> content_changed;
            std::uint64_t source_revision = 0;
        };

        struct AssetDataRecord
        {
            using Result = cxx::expected<void, error::Error>;
            using Operation = exec::task<Result>;
            using SharedOperation = decltype(exec::split(stdexec::write_env(
                stdexec::then(
                    execution::asResult(
                        stdexec::starts_on(std::declval<execution::RunLoop::Scheduler>(), std::declval<Operation>())
                    ),
                    &normalize
                ),
                stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
            )));

            AssetRecord& asset;
            std::shared_ptr<const asset::Asset> backing;
            std::optional<asset::AssetReadSource> source;
            const void* data = nullptr;
            cxx::type_info data_type;
            std::atomic<std::size_t> readers{0};

            // Empty for CPU snapshots. A use owns its result until asynchronous release succeeds.
            cxx::type_info use_type;
            const void* provider = nullptr;
            std::uint64_t source_revision = 0;
            void (*destroy)(const void*) noexcept = nullptr;
            std::function<Operation(const void*)> release;
            std::vector<AssetDataRef<void>> dependencies;
            std::optional<SharedOperation> operation;
            std::size_t waiters = 0;
            std::size_t unused_index = std::numeric_limits<std::size_t>::max();
            bool preparing = false;
            bool releasing = false;
            std::optional<error::Error> release_failure;

            AssetDataRecord(
                AssetRecord& asset,
                std::shared_ptr<const asset::Asset> backing,
                const void* data,
                cxx::type_info data_type
            ) noexcept;
            explicit AssetDataRecord(AssetRecord& asset) noexcept;
            ~AssetDataRecord() noexcept;
            AssetDataRecord(const AssetDataRecord&) = delete;
            AssetDataRecord& operator=(const AssetDataRecord&) = delete;
        };
    } // namespace detail

    detail::AssetDataRecord::AssetDataRecord(
        AssetRecord& asset,
        std::shared_ptr<const asset::Asset> backing,
        const void* data,
        cxx::type_info data_type
    ) noexcept
        : asset(asset), backing(std::move(backing)), data(data), data_type(data_type),
          source_revision(asset.source_revision)
    {
    }

    detail::AssetDataRecord::AssetDataRecord(AssetRecord& asset) noexcept : asset(asset) {}

    detail::AssetDataRecord::~AssetDataRecord() noexcept
    {
        if (destroy && data)
        {
            destroy(data);
        }
    }

    void detail::retainAsset(AssetRecord& record) noexcept
    {
        record.references.fetch_add(1, std::memory_order_relaxed);
    }

    void detail::releaseAsset(AssetRecord& record) noexcept
    {
        // Copy the notifier before publishing zero; Main may then reclaim the record.
        auto changed = record.changed;
        if (record.references.fetch_sub(1, std::memory_order_acq_rel) == 1)
        {
            changed.request();
        }
    }

    asset::AssetId detail::assetIdentity(const AssetRecord& record) noexcept
    {
        return record.id;
    }

    entt::sigh<void(asset::AssetId)>& detail::assetContentChanged(AssetRecord& record) noexcept
    {
        return record.content_changed;
    }

    void detail::retainData(AssetDataRecord& record) noexcept
    {
        const bool first = record.readers.fetch_add(1, std::memory_order_relaxed) == 0;
        auto& readers = record.use_type.isValid() ? record.asset.use_readers : record.asset.data_readers;
        readers.fetch_add(1, std::memory_order_relaxed);
        if (first)
        {
            record.asset.changed.request();
        }
    }

    void detail::releaseData(AssetDataRecord& record) noexcept
    {
        auto& asset = record.asset;
        auto changed = asset.changed;
        auto& readers = record.use_type.isValid() ? asset.use_readers : asset.data_readers;
        // Parent remains pinned until the aggregate decrement. Neither record is touched afterwards.
        const bool last = record.readers.fetch_sub(1, std::memory_order_acq_rel) == 1;
        readers.fetch_sub(1, std::memory_order_acq_rel);
        if (last)
        {
            changed.request();
        }
    }

    detail::AssetRecord& detail::assetRecord(AssetDataRecord& record) noexcept
    {
        return record.asset;
    }

    const void* detail::dataAddress(const AssetDataRecord& record) noexcept
    {
        return record.data;
    }

    cxx::type_info detail::dataType(const AssetDataRecord& record) noexcept
    {
        return record.data_type;
    }

    const asset::Asset* detail::originalAsset(const AssetDataRecord& record) noexcept
    {
        return record.backing.get();
    }

    void AssetRepository::configureUse(
        detail::AssetDataRecord& record,
        cxx::type_info type,
        void (*destroy)(const void*) noexcept,
        Release release
    ) noexcept
    {
        record.data_type = type;
        record.destroy = destroy;
        record.release = std::move(release);
    }

    std::vector<AssetDataRef<void>>& AssetRepository::useDependencies(detail::AssetDataRecord& record) noexcept
    {
        return record.dependencies;
    }

    struct AssetRepository::Impl final
    {
        using Operation = exec::task<AssetResult<void>>;
        using SharedRead = decltype(exec::split(stdexec::write_env(
            stdexec::then(
                execution::asResult(
                    stdexec::starts_on(std::declval<execution::RunLoop::Scheduler>(), std::declval<Operation>())
                ),
                &normalize
            ),
            stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
        )));

        struct Record final : detail::AssetRecord
        {
            Impl& repository;
            asset::AssetCodec& codec;
            execution::CompletionWork notification;
            std::vector<std::unique_ptr<detail::AssetDataRecord>> snapshots;
            detail::AssetDataRecord* current = nullptr;
            std::unordered_map<std::string_view, std::unordered_map<const void*, detail::AssetDataRecord*>> uses;
            std::optional<SharedRead> reading;
            std::size_t waiters = 0;
            bool loading = false;
            bool writing = false;
            std::size_t unused_index = std::numeric_limits<std::size_t>::max();

            Record(Impl& repository, asset::AssetId identity, asset::AssetCodec& codec) noexcept
                : repository(repository), codec(codec), notification(repository.execution, this, notify)
            {
                id = identity;
                changed = notification.requester();
            }

            static void notify(void* context) noexcept
            {
                auto& record = *static_cast<Record*>(context);
                record.repository.reclaim(record);
            }
        };

        struct Waiting final
        {
            Record& record;

            explicit Waiting(Record& record) noexcept : record(record)
            {
                ++record.waiters;
            }

            ~Waiting() noexcept
            {
                --record.waiters;
                record.changed.request();
            }
        };

        struct ChangeSubscription final
        {
            ChangeSubscription* next;
            ChangeSubscription** previous;
            void* receiver;
            void (*complete)(void*) noexcept;

            ChangeSubscription(Impl& repository, void* receiver, void (*complete)(void*) noexcept) noexcept
                : next(repository.changes), previous(&repository.changes), receiver(receiver), complete(complete)
            {
                if (next)
                {
                    next->previous = &this->next;
                }
                repository.changes = this;
            }

            ~ChangeSubscription() noexcept
            {
                unlink();
            }

            void unlink() noexcept
            {
                if (previous)
                {
                    *previous = next;
                    if (next)
                    {
                        next->previous = previous;
                    }
                    previous = nullptr;
                }
            }
        };

        Impl(
            asset::AssetVfsView source,
            asset::AssetDecodeLimits limits,
            std::unique_ptr<AssetRetentionPolicy> policy
        ) noexcept
            : execution(execution::ExecutionRuntime::instance()), tasks_(execution), source(std::move(source)),
              limits(limits), policy(std::move(policy))
        {
        }

        auto whenChanged() noexcept
        {
            return exec::create<stdexec::set_value_t()>(
                [this](auto& operation) noexcept
                {
                    using Receiver = std::remove_reference_t<decltype(operation.receiver)>;
                    return ChangeSubscription{
                        *this,
                        &operation.receiver,
                        +[](void* receiver) noexcept
                        { stdexec::set_value(std::move(*static_cast<Receiver*>(receiver))); }
                    };
                }
            );
        }

        void changed() noexcept
        {
            auto* ready = std::exchange(changes, nullptr);
            if (ready)
            {
                ready->previous = &ready;
            }
            while (ready)
            {
                auto* current = ready;
                current->unlink();
                current->complete(current->receiver);
            }
        }

        void reclaim(Record& record) noexcept
        {
            // Retirement may be suspended inside a release callback while iterating these records.
            if (tasks_.stopToken().stop_requested())
            {
                changed();
                return;
            }
            const bool idle = !record.loading && !record.writing && record.waiters == 0;
            if (!policy && idle && record.current && record.current->readers.load(std::memory_order_acquire) == 0)
            {
                record.current = nullptr;
            }
            std::erase_if(
                record.snapshots,
                [&](const auto& data) noexcept
                {
                    const bool finished_use = !data->use_type.isValid() || (!data->release && !data->preparing &&
                                                                            !data->releasing && data->waiters == 0);
                    const bool unused = data->readers.load(std::memory_order_acquire) == 0;
                    return data.get() != record.current && finished_use && unused;
                }
            );
            updateUnused(record);
            if (!tasks_.stopToken().stop_requested())
            {
                for (auto& data : record.snapshots)
                {
                    updateUnusedUse(*data);
                    const bool expired = !isCurrent(*data);
                    if (canRelease(*data) && !data->release_failure && (!policy || expired))
                    {
                        startUseOperation(*data, releaseUse(record, *data), false);
                    }
                }
            }
            changed();
        }

        void updateUnused(Record& record) noexcept
        {
            constexpr auto absent = std::numeric_limits<std::size_t>::max();
            const bool has_work = record.writing || record.loading || record.waiters != 0;
            const bool unused = !tasks_.stopToken().stop_requested() && !has_work && record.current &&
                                record.current->readers.load(std::memory_order_acquire) == 0;
            if (unused && record.unused_index == absent)
            {
                record.unused_index = candidates.size();
                candidates.push_back(record.id);
            }
            else if (!unused && record.unused_index != absent)
            {
                const auto moved = candidates.back();
                candidates[record.unused_index] = moved;
                records.find(moved)->second->unused_index = record.unused_index;
                candidates.pop_back();
                record.unused_index = absent;
            }
        }

        Operation load(Record& record) noexcept
        {
            const auto revision = record.source_revision;
            auto scheduler = execution.blockingIoScheduler();
            if (!scheduler)
            {
                co_return cxx::unexpected(execution::toError(scheduler.error()));
            }
            auto opened = co_await execution::asResult(stdexec::then(
                stdexec::schedule(*scheduler),
                [this, id = record.id](
                ) noexcept -> AssetResult<std::pair<asset::AssetBlob, std::optional<asset::AssetReadSource>>>
                {
                    auto physical = source.openSource(id);
                    if (physical)
                    {
                        auto bytes = physical->readRoot(limits.max_image_bytes);
                        if (!bytes)
                        {
                            return cxx::unexpected(
                                error::Error{kAssetReadFailure, {static_cast<std::uint64_t>(bytes.error())}}
                            );
                        }
                        return std::pair{std::move(*bytes), std::optional{std::move(*physical)}};
                    }
                    if (physical.error() != asset::EAssetStorageError::UNSUPPORTED)
                    {
                        return cxx::unexpected(
                            error::Error{kAssetReadFailure, {static_cast<std::uint64_t>(physical.error())}}
                        );
                    }
                    // Ordinary immutable providers retain their existing complete-asset read path.
                    // A decoded compound asset without a physical source is rejected before publication below.
                    auto bytes = source.open(id, limits.max_image_bytes);
                    if (!bytes)
                    {
                        return cxx::unexpected(
                            error::Error{kAssetReadFailure, {static_cast<std::uint64_t>(bytes.error())}}
                        );
                    }
                    return std::pair{std::move(*bytes), std::optional<asset::AssetReadSource>{}};
                }
            ));
            if (!opened)
            {
                co_return cxx::unexpected(assetError(opened.error()));
            }
            auto decoded = co_await stdexec::continues_on(
                execution::asResult(stdexec::then(
                    stdexec::schedule(execution.cpuScheduler()),
                    [this, &record, bytes = std::move(opened->first.bytes)](
                    ) mutable noexcept -> AssetResult<std::shared_ptr<const asset::Asset>>
                    {
                        auto result = record.codec.decode(record.id, std::move(bytes), limits);
                        if (!result)
                        {
                            return cxx::unexpected(error::Error{
                                kAssetDecodeFailure,
                                {static_cast<std::uint64_t>(result.error().code), result.error().offset}
                            });
                        }
                        return std::move(*result);
                    }
                )),
                execution.mainScheduler()
            );
            if (!decoded)
            {
                co_return cxx::unexpected(assetError(decoded.error()));
            }
            const auto member = record.codec.contentFile(**decoded);
            if (!member.empty())
            {
                if (!opened->second)
                {
                    co_return cxx::unexpected(error::Error{
                        kAssetReadFailure,
                        {static_cast<std::uint64_t>(asset::EAssetStorageError::UNSUPPORTED)}
                    });
                }
                auto bound = co_await stdexec::continues_on(
                    execution::asResult(stdexec::then(
                        stdexec::schedule(*scheduler),
                        [&physical = *opened->second, name = std::string(member)]() noexcept -> AssetResult<void>
                        {
                            auto result = physical.bindContent(name);
                            if (!result)
                            {
                                return cxx::unexpected(
                                    error::Error{kAssetReadFailure, {static_cast<std::uint64_t>(result.error())}}
                                );
                            }
                            return {};
                        }
                    )),
                    execution.mainScheduler()
                );
                if (!bound)
                {
                    co_return cxx::unexpected(assetError(bound.error()));
                }
            }
            if (record.source_revision != revision)
            {
                co_return cxx::unexpected(error::Error{kAssetChanged});
            }
            const auto* data = record.codec.data(**decoded);
            auto snapshot =
                std::make_unique<detail::AssetDataRecord>(record, std::move(*decoded), data, record.codec.dataType());
            snapshot->source = std::move(opened->second);
            record.current = snapshot.get();
            record.snapshots.push_back(std::move(snapshot));
            co_return AssetResult<void>{};
        }

        void startRead(Record& record) noexcept
        {
            record.loading = true;
            record.reading.emplace(exec::split(stdexec::write_env(
                stdexec::then(
                    execution::asResult(stdexec::starts_on(execution.mainScheduler(), load(record))),
                    &normalize
                ),
                stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
            )));
            auto complete = stdexec::then(
                execution::asResult(stdexec::then(*record.reading, &copyOutcome)),
                [&record](auto) noexcept
                {
                    record.loading = false;
                    record.changed.request();
                }
            );
            auto accepted = tasks_.spawn(stdexec::write_env(
                std::move(complete),
                stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
            ));
            if (!accepted)
            {
                record.loading = false;
                record.reading.reset();
            }
        }

        static bool canRelease(const detail::AssetDataRecord& data) noexcept
        {
            return data.use_type.isValid() && data.release && !data.preparing && !data.releasing && data.waiters == 0 &&
                   data.readers.load(std::memory_order_acquire) == 0;
        }

        void updateUnusedUse(detail::AssetDataRecord& data) noexcept
        {
            if (!data.use_type.isValid())
            {
                return;
            }
            constexpr auto absent = std::numeric_limits<std::size_t>::max();
            const bool unused = canRelease(data) && !data.release_failure;
            auto& group = unused_uses[data.use_type.name()][data.provider];
            if (unused && data.unused_index == absent)
            {
                data.unused_index = group.size();
                group.push_back(&data);
            }
            else if (!unused && data.unused_index != absent)
            {
                group[data.unused_index] = group.back();
                group.back()->unused_index = data.unused_index;
                group.pop_back();
                data.unused_index = absent;
            }
        }

        void collectUses() noexcept
        {
            for (auto& [type, providers] : unused_uses)
            {
                for (auto& [provider, group] : providers)
                {
                    if (group.empty())
                    {
                        continue;
                    }
                    use_candidates.clear();
                    for (auto* data : group)
                    {
                        use_candidates.push_back(data->asset.id);
                    }
                    selected.clear();
                    policy->selectReleases(group.front()->use_type, provider, use_candidates, selected);
                    const std::unordered_set<asset::AssetId> chosen(selected.begin(), selected.end());
                    for (std::size_t i = group.size(); i != 0; --i)
                    {
                        auto& data = *group[i - 1];
                        if (chosen.contains(data.asset.id) && canRelease(data))
                        {
                            auto& record = static_cast<Record&>(data.asset);
                            startUseOperation(data, releaseUse(record, data), false);
                        }
                    }
                }
            }
        }

        Operation releaseUse(Record& asset, detail::AssetDataRecord& data) noexcept
        {
            auto released = data.data ? co_await data.release(data.data) : AssetResult<void>{};
            if (!released)
            {
                data.release_failure = released.error();
                co_return cxx::unexpected(released.error());
            }
            if (data.destroy && data.data)
            {
                data.destroy(std::exchange(data.data, nullptr));
            }
            // The reverse index borrows dependency records; unlink before dropping their references.
            for (const auto& dependency : data.dependencies)
            {
                if (!dependency)
                {
                    continue; // A rejected preparation can still own an output that needs release.
                }
                const auto found = dependents.find(&dependency.record_->asset);
                if (found != dependents.end())
                {
                    found->second.erase(&data);
                    if (found->second.empty())
                    {
                        dependents.erase(found);
                    }
                }
            }
            data.dependencies.clear();
            data.release = {};
            data.release_failure.reset();
            auto& providers = asset.uses.at(data.use_type.name());
            const auto current = providers.find(data.provider);
            if (current != providers.end() && current->second == &data)
            {
                providers.erase(current);
            }
            co_return AssetResult<void>{};
        }

        void startUseOperation(detail::AssetDataRecord& data, Operation operation, bool preparing) noexcept
        {
            data.preparing = preparing;
            data.releasing = !preparing;
            updateUnusedUse(data);
            data.operation.emplace(exec::split(stdexec::write_env(
                stdexec::then(
                    execution::asResult(stdexec::starts_on(execution.mainScheduler(), std::move(operation))),
                    &normalize
                ),
                stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
            )));
            // Only this repository owns the accepted root. Subscribers can cancel independently.
            auto accepted = tasks_.spawn(stdexec::write_env(
                stdexec::then(
                    execution::asResult(stdexec::then(*data.operation, &copyOutcome)),
                    [&data](auto) noexcept
                    {
                        data.preparing = false;
                        data.releasing = false;
                        data.asset.changed.request();
                    }
                ),
                stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
            ));
            if (!accepted)
            {
                std::terminate(); // Admission is Main-serialized and checked before accepting this operation.
            }
        }

        // Main-only scratch is reused. Queries do not suspend or invoke consumer callbacks.
        bool isCurrent(const detail::AssetDataRecord& data) noexcept
        {
            const bool stale = data.source_revision != data.asset.source_revision;
            const bool unavailable = !data.data || data.preparing || data.releasing;
            if (stale || unavailable || data.dependencies.empty())
            {
                return !stale && !unavailable;
            }
            dependency_work.clear();
            dependency_visited.clear();
            dependency_work.push_back(&data);
            for (std::size_t i = 0; i < dependency_work.size(); ++i)
            {
                const auto& current = *dependency_work[i];
                if (!dependency_visited.insert(&current).second)
                {
                    continue;
                }
                const bool stale = current.source_revision != current.asset.source_revision;
                const bool unavailable = !current.data || current.preparing || current.releasing;
                if (stale || unavailable)
                {
                    return false;
                }
                for (const auto& dependency : current.dependencies)
                {
                    dependency_work.push_back(dependency.record_);
                }
            }
            return true;
        }

        AssetResult<void> validateDependencies(const detail::AssetDataRecord& data) noexcept
        {
            dependency_work.clear();
            dependency_visited.clear();
            for (const auto& dependency : data.dependencies)
            {
                if (!dependency)
                {
                    return cxx::unexpected(execution::toError(execution::EExecutionError::INVALID_ARGUMENT));
                }
                dependency_work.push_back(dependency.record_);
            }
            if (data.source_revision != data.asset.source_revision)
            {
                return cxx::unexpected(error::Error{kAssetChanged});
            }
            for (std::size_t i = 0; i < dependency_work.size(); ++i)
            {
                const auto& current = *dependency_work[i];
                const bool foreign = &static_cast<const Record&>(current.asset).repository != this;
                if (foreign || &current.asset == &data.asset)
                {
                    return cxx::unexpected(execution::toError(execution::EExecutionError::INVALID_ARGUMENT));
                }
                if (!dependency_visited.insert(&current).second)
                {
                    continue;
                }
                const bool stale = current.source_revision != current.asset.source_revision;
                const bool unavailable = !current.data || current.preparing || current.releasing;
                if (stale || unavailable)
                {
                    return cxx::unexpected(error::Error{kAssetChanged});
                }
                for (const auto& dependency : current.dependencies)
                {
                    dependency_work.push_back(dependency.record_);
                }
            }
            return {};
        }

        void invalidateDependents(Record& record) noexcept
        {
            content_changes.clear();
            content_visited.clear();
            content_changes.push_back(&record);
            content_visited.insert(&record);
            for (std::size_t i = 0; i < content_changes.size(); ++i)
            {
                auto& current = *content_changes[i];
                current.changed.request();
                const auto found = dependents.find(&current);
                if (found == dependents.end())
                {
                    continue;
                }
                for (auto* dependent : found->second)
                {
                    auto* parent = &static_cast<Record&>(dependent->asset);
                    if (content_visited.insert(parent).second)
                    {
                        content_changes.push_back(parent);
                    }
                }
            }
        }

        void publishContentChanges() noexcept
        {
            // Source and every derived qualification are now consistent. Receivers only mark inputs dirty.
            for (auto* record : content_changes)
            {
                record->content_changed.publish(record->id);
            }
            content_changes.clear();
            content_visited.clear();
        }

        void invalidate(Record& record) noexcept
        {
            if (record.source_revision == UINT64_MAX)
            {
                std::terminate(); // Content identity must never wrap back to a retained snapshot's revision.
            }
            ++record.source_revision;
            record.current = nullptr;
            invalidateDependents(record);
            publishContentChanges();
        }

#if defined(LUX_ASSET_FILEWATCH)
        Operation
        observe(AssetRepository& repository, platform::FileWatcher& watcher, std::chrono::milliseconds period) noexcept
        {
            while (!tasks_.stopToken().stop_requested())
            {
                auto waited = co_await stdexec::continues_on(
                    execution::asResult(execution.timer().after(period)),
                    execution.mainScheduler()
                );
                if (!waited)
                {
                    if (waited.error().isCancelled())
                    {
                        co_return cxx::unexpected(error::Error{kAssetCancelled});
                    }
                    if (const auto* scheduling = waited.error().executionFailure())
                    {
                        co_return cxx::unexpected(execution::toError(*scheduling));
                    }
                    auto failure = execution::EExecutionError::BACKEND_FAILURE;
                    switch (*waited.error().domainFailure())
                    {
                    case execution::ETimerError::INVALID_ARGUMENT:
                        failure = execution::EExecutionError::INVALID_ARGUMENT;
                        break;
                    case execution::ETimerError::CAPACITY_EXCEEDED:
                        failure = execution::EExecutionError::CAPACITY_EXCEEDED;
                        break;
                    case execution::ETimerError::STOPPING:
                        failure = execution::EExecutionError::STOPPING;
                        break;
                    case execution::ETimerError::ALLOCATION_FAILURE:
                        std::terminate();
                    default:
                        break;
                    }
                    co_return cxx::unexpected(execution::toError(failure));
                }
                if (tasks_.stopToken().stop_requested())
                {
                    co_return AssetResult<void>{};
                }
                auto events = watcher.drain();
                if (!events.empty())
                {
                    auto result = co_await repository.refresh(events);
                    if (!result)
                    {
                        co_return cxx::unexpected(result.error());
                    }
                }
            }
            co_return AssetResult<void>{};
        }
#endif

        bool hasOperations() const noexcept
        {
            if (pending_refreshes != 0)
            {
                return true;
            }
            return std::ranges::any_of(
                records,
                [](const auto& entry) noexcept
                {
                    const auto& record = *entry.second;
                    return record.loading || record.writing || record.waiters != 0 ||
                           std::ranges::any_of(
                               record.snapshots,
                               [](const auto& data) noexcept
                               { return data->preparing || data->releasing || data->waiters != 0; }
                           );
                }
            );
        }

        Operation releaseResources() noexcept
        {
            for (auto& [id, record] : records)
            {
                record->current = nullptr;
            }
            // Shutdown only: release unused derived results before waiting for their dependent references.
            for (;;)
            {
                bool pending = false;
                bool released_any = false;
                for (auto& [id, record] : records)
                {
                    for (auto& data : record->snapshots)
                    {
                        if (canRelease(*data))
                        {
                            data->releasing = true;
                            auto released = co_await releaseUse(*record, *data);
                            data->releasing = false;
                            if (!released)
                            {
                                co_return cxx::unexpected(released.error());
                            }
                            released_any = true;
                        }
                        pending = pending || bool(data->release);
                    }
                    pending = pending || record->references.load(std::memory_order_acquire) != 0 ||
                              record->data_readers.load(std::memory_order_acquire) != 0 ||
                              record->use_readers.load(std::memory_order_acquire) != 0 ||
                              !record->content_changed.empty();
                }
                if (!pending)
                {
                    break;
                }
                if (!released_any)
                {
                    // All operations have settled. Remaining references cannot disappear through this owner.
                    co_return cxx::unexpected(error::Error{kAssetBusy});
                }
            }
            // Never destroy CompletionWork from inside its own notification stack.
            co_await stdexec::schedule(execution.mainScheduler());
            unused_uses.clear();
            dependents.clear();
            records.clear();
            candidates.clear();
            selected.clear();
            co_return AssetResult<void>{};
        }

        execution::ExecutionRuntime& execution;
        execution::TaskScope tasks_;
        asset::AssetVfsView source;
        asset::AssetDecodeLimits limits;
        std::unique_ptr<AssetRetentionPolicy> policy;
        std::vector<std::unique_ptr<asset::AssetCodec>> codecs;
        std::unordered_map<std::uint32_t, asset::AssetCodec*> by_magic;
        std::unordered_map<asset::AssetId, std::unique_ptr<Record>> records;
        std::unordered_map<std::string_view, std::unordered_map<const void*, AssetRepository::Prepare>> preparations;
        std::unordered_map<std::string_view, std::unordered_map<const void*, std::vector<detail::AssetDataRecord*>>>
            unused_uses;
        std::vector<asset::AssetId> use_candidates;
        std::unordered_map<detail::AssetRecord*, std::unordered_set<detail::AssetDataRecord*>> dependents;
        std::vector<const detail::AssetDataRecord*> dependency_work;
        std::unordered_set<const detail::AssetDataRecord*> dependency_visited;
        std::vector<Record*> content_changes;
        std::unordered_set<detail::AssetRecord*> content_visited;
        ChangeSubscription* changes = nullptr;
        std::vector<asset::AssetId> candidates;
        std::vector<asset::AssetId> selected;
        bool watching = false;
        std::size_t pending_refreshes = 0;
    };

    AssetResult<void> AssetRepository::adoptUseData(detail::AssetDataRecord& record, const void* data) noexcept
    {
        // Accepted output is owned even when it cannot be published; normal release still runs exactly once.
        record.data = data;
        if (auto valid = impl_->validateDependencies(record); !valid)
        {
            auto& providers = static_cast<Impl::Record&>(record.asset).uses.at(record.use_type.name());
            const auto current = providers.find(record.provider);
            if (current != providers.end() && current->second == &record)
            {
                providers.erase(current);
            }
            return valid;
        }
        for (const auto& dependency : record.dependencies)
        {
            impl_->dependents[&dependency.record_->asset].insert(&record);
        }
        return {};
    }

    std::uint64_t AssetRepository::revision(const detail::AssetRecord& record) const noexcept
    {
        if (!impl_->execution.isMainThread() || &static_cast<const Impl::Record&>(record).repository != impl_.get())
        {
            std::terminate(); // Queries require a live reference from this repository at Main's safe point.
        }
        return record.source_revision;
    }

    std::uint64_t AssetRepository::revision(const detail::AssetDataRecord& record) const noexcept
    {
        (void)revision(record.asset);
        return record.source_revision;
    }

    bool AssetRepository::isCurrent(const detail::AssetDataRecord& record) const noexcept
    {
        (void)revision(record.asset);
        return impl_->isCurrent(record);
    }

    AssetRepository::AssetRepository(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    AssetRepository::~AssetRepository() noexcept
    {
        auto& execution = impl_->execution;
        if (!execution.isMainThread())
        {
            std::terminate();
        }
        std::optional<AssetResult<void>> completed;
        auto submitted = execution.spawn(stdexec::write_env(
            stdexec::then(
                execution::asResult(stdexec::starts_on(execution.mainScheduler(), settleAndRelease())),
                [&completed](auto result) noexcept { completed.emplace(normalize(std::move(result))); }
            ),
            stdexec::prop{stdexec::get_stop_token, stdexec::never_stop_token{}}
        ));
        if (!submitted)
        {
            std::terminate();
        }
        auto waited = execution.waitUntil([&completed]() noexcept { return completed.has_value(); });
        if (!waited || !*completed)
        {
            std::terminate(); // Final destruction requires all external references and accepted work to be settled.
        }
    }

    AssetRepository::CreateResult AssetRepository::create(
        asset::AssetVfsView source,
        asset::AssetDecodeLimits limits,
        std::unique_ptr<AssetRetentionPolicy> policy,
        std::vector<std::unique_ptr<asset::AssetCodec>> additional_codecs
    ) noexcept
    {
        auto& execution = execution::ExecutionRuntime::instance();
        if (!execution.isMainThread())
        {
            return cxx::unexpected(execution::toError(execution::EExecutionError::WRONG_THREAD));
        }
        const bool invalid_limits = limits.max_image_bytes == 0 || limits.max_decoded_bytes == 0;
        if (!source || invalid_limits)
        {
            return cxx::unexpected(execution::toError(execution::EExecutionError::INVALID_STATE));
        }
        auto scheduler = execution.blockingIoScheduler();
        if (!scheduler)
        {
            return cxx::unexpected(execution::toError(scheduler.error()));
        }
        auto registered = error::ErrorRegistry::instance().registerTypes(errors);
        if (!registered)
        {
            return cxx::unexpected(registered.error());
        }
        auto impl = std::make_unique<Impl>(std::move(source), limits, std::move(policy));
        impl->codecs = asset::createAssetCodecs();
        for (auto& codec : additional_codecs)
        {
            impl->codecs.push_back(std::move(codec));
        }
        for (auto& codec : impl->codecs)
        {
            if (!codec || !impl->by_magic.emplace(codec->magic(), codec.get()).second)
            {
                return cxx::unexpected(error::Error{kAssetTypeMismatch});
            }
        }
        return std::unique_ptr<AssetRepository>{new AssetRepository(std::move(impl))};
    }

    AssetResult<detail::AssetRecord*>
    AssetRepository::acquireRecord(asset::AssetId id, asset::AssetTypeId type, cxx::type_info data_type) noexcept
    {
        if (!impl_->execution.isMainThread())
        {
            return cxx::unexpected(execution::toError(execution::EExecutionError::WRONG_THREAD));
        }
        if (impl_->tasks_.stopToken().stop_requested())
        {
            return cxx::unexpected(error::Error{kAssetRepositoryClosed});
        }
        auto entry = impl_->source.describe(id);
        if (!entry)
        {
            return cxx::unexpected(error::Error{kAssetReadFailure, {static_cast<std::uint64_t>(entry.error())}});
        }
        auto codec = impl_->by_magic.find(entry->magic_number);
        const bool missing_codec = codec == impl_->by_magic.end();
        const bool mismatched =
            !missing_codec && (codec->second->type() != type || codec->second->dataType() != data_type);
        if (missing_codec || mismatched)
        {
            return cxx::unexpected(error::Error{kAssetTypeMismatch});
        }
        auto [position, inserted] = impl_->records.try_emplace(id);
        if (inserted)
        {
            position->second = std::make_unique<Impl::Record>(*impl_, id, *codec->second);
        }
        if (&position->second->codec != codec->second)
        {
            return cxx::unexpected(error::Error{kAssetTypeMismatch});
        }
        return position->second.get();
    }

    exec::task<AssetResult<AssetDataRef<asset::Asset>>> AssetRepository::readData(AssetRef<asset::Asset> asset) noexcept
    {
        co_return co_await readRecord(*asset.record_);
    }

    exec::task<AssetResult<AssetDataRef<asset::Asset>>> AssetRepository::readRecord(detail::AssetRecord& asset) noexcept
    {
        if (impl_->tasks_.stopToken().stop_requested())
        {
            co_return cxx::unexpected(error::Error{kAssetRepositoryClosed});
        }
        auto& record = static_cast<Impl::Record&>(asset);
        if (record.current)
        {
            co_return AssetDataRef<asset::Asset>{*record.current};
        }
        const bool first = record.waiters == 0 && !record.loading;
        Impl::Waiting waiting{record};
        if (first)
        {
            impl_->startRead(record);
        }
        if (!record.reading)
        {
            co_return cxx::unexpected(error::Error{kAssetRepositoryClosed});
        }
        auto read = co_await execution::asResult(stdexec::then(*record.reading, &copyOutcome));
        if (!read)
        {
            co_return cxx::unexpected(assetError(read.error()));
        }
        if (!record.current)
        {
            co_return cxx::unexpected(error::Error{kAssetChanged});
        }
        co_return AssetDataRef<asset::Asset>{*record.current};
    }

    exec::task<AssetResult<cxx::SharedBytes<>>>
    AssetRepository::readSourceRange(AssetDataRef<void> source, std::uint64_t offset, std::uint64_t size) noexcept
    {
        if (impl_->tasks_.stopToken().stop_requested())
        {
            co_return cxx::unexpected(error::Error{kAssetRepositoryClosed});
        }
        if (!source.record_->source)
        {
            co_return cxx::unexpected(
                error::Error{kAssetReadFailure, {static_cast<std::uint64_t>(asset::EAssetStorageError::UNSUPPORTED)}}
            );
        }
        auto scheduler = impl_->execution.blockingIoScheduler();
        if (!scheduler)
        {
            co_return cxx::unexpected(execution::toError(scheduler.error()));
        }
        auto read = co_await stdexec::continues_on(
            execution::asResult(stdexec::then(
                stdexec::schedule(*scheduler),
                [this, &physical = *source.record_->source, offset, size]() noexcept -> AssetResult<cxx::SharedBytes<>>
                {
                    auto bytes = physical.readRange(offset, size, impl_->limits.max_image_bytes);
                    if (!bytes)
                    {
                        return cxx::unexpected(
                            error::Error{kAssetReadFailure, {static_cast<std::uint64_t>(bytes.error())}}
                        );
                    }
                    return std::move(bytes->bytes);
                }
            )),
            impl_->execution.mainScheduler()
        );
        if (!read)
        {
            co_return cxx::unexpected(assetError(read.error()));
        }
        co_return std::move(*read);
    }

    AssetResult<void>
    AssetRepository::registerPreparation(cxx::type_info type, const void* provider, Prepare prepare) noexcept
    {
        if (impl_->tasks_.stopToken().stop_requested())
        {
            return cxx::unexpected(error::Error{kAssetRepositoryClosed});
        }
        auto& providers = impl_->preparations[type.name()];
        if (!providers.emplace(provider, std::move(prepare)).second)
        {
            return cxx::unexpected(error::Error{kAssetBusy});
        }
        return {};
    }

    exec::task<AssetResult<AssetDataRef<void>>>
    AssetRepository::useData(AssetRef<asset::Asset> asset, cxx::type_info type, const void* provider) noexcept
    {
        if (impl_->tasks_.stopToken().stop_requested())
        {
            co_return cxx::unexpected(error::Error{kAssetRepositoryClosed});
        }
        const auto registration = impl_->preparations.find(type.name());
        if (registration == impl_->preparations.end() || !registration->second.contains(provider))
        {
            co_return cxx::unexpected(error::Error{kAssetTypeMismatch});
        }
        auto& record = static_cast<Impl::Record&>(*asset.record_);
        for (;;)
        {
            auto& providers = record.uses[type.name()];
            auto found = providers.find(provider);
            detail::AssetDataRecord* data = found == providers.end() ? nullptr : found->second;
            if (data && data->release_failure)
            {
                co_return cxx::unexpected(*data->release_failure);
            }
            if (data && data->releasing)
            {
                ++data->waiters;
                cxx::scope_exit finished(
                    [data]() noexcept
                    {
                        --data->waiters;
                        data->asset.changed.request();
                    }
                );
                auto released = co_await execution::asResult(stdexec::then(*data->operation, &copyOutcome));
                if (!released)
                {
                    co_return cxx::unexpected(assetError(released.error()));
                }
                continue;
            }
            const bool expired = data && !data->preparing && data->data && !impl_->isCurrent(*data);
            if (!data || data->source_revision != record.source_revision || expired)
            {
                auto created = std::make_unique<detail::AssetDataRecord>(record);
                data = created.get();
                data->use_type = type;
                data->provider = provider;
                data->source_revision = record.source_revision;
                record.snapshots.push_back(std::move(created));
                providers[provider] = data;
                impl_->startUseOperation(*data, registration->second.at(provider)(record, *data), true);
            }
            ++data->waiters;
            cxx::scope_exit finished(
                [data]() noexcept
                {
                    --data->waiters;
                    data->asset.changed.request();
                }
            );
            if (data->preparing || !data->data)
            {
                auto prepared = co_await execution::asResult(stdexec::then(*data->operation, &copyOutcome));
                if (!prepared)
                {
                    co_return cxx::unexpected(assetError(prepared.error()));
                }
            }
            if (!impl_->isCurrent(*data))
            {
                co_return cxx::unexpected(error::Error{kAssetChanged});
            }
            co_return AssetDataRef<void>{*data};
        }
    }

    void AssetRepository::forEachUse(
        asset::AssetId asset,
        const std::function<void(cxx::type_info, const void*, std::size_t, bool, bool)>& visitor
    ) const noexcept
    {
        const auto found = impl_->records.find(asset);
        if (found == impl_->records.end())
        {
            return;
        }
        for (const auto& data : found->second->snapshots)
        {
            if (data->use_type.isValid())
            {
                visitor(
                    data->use_type,
                    data->provider,
                    data->readers.load(std::memory_order_acquire),
                    data->preparing,
                    data->data != nullptr
                );
            }
        }
    }

    exec::task<AssetResult<void>> AssetRepository::writeData(AssetRef<asset::Asset> asset, const void* data) noexcept
    {
        if (impl_->tasks_.stopToken().stop_requested())
        {
            co_return cxx::unexpected(error::Error{kAssetRepositoryClosed});
        }
        auto& record = static_cast<Impl::Record&>(*asset.record_);
        if (record.writing)
        {
            co_return cxx::unexpected(error::Error{kAssetBusy});
        }
        record.writing = true;
        // The scope guard changes only Main-owned state; all continuations below restore Main affinity.
        cxx::scope_exit complete(
            [&record]() noexcept
            {
                record.writing = false;
                record.changed.request();
            }
        );
        auto original = co_await execution::asResult(readData(asset));
        if (!original)
        {
            co_return cxx::unexpected(assetError(original.error()));
        }
        const auto source_revision = record.source_revision;
        auto encoded = co_await stdexec::continues_on(
            execution::asResult(stdexec::then(
                stdexec::schedule(impl_->execution.cpuScheduler()),
                [this, &record, data, original = std::move(*original)]() noexcept -> AssetResult<std::vector<std::byte>>
                {
                    auto result = record.codec.encode(
                        original->info(),
                        data,
                        original->auxiliaryPayloads(),
                        asset::AssetEncodeLimits{impl_->limits.max_image_bytes}
                    );
                    if (!result)
                    {
                        return cxx::unexpected(error::Error{
                            kAssetEncodeFailure,
                            {static_cast<std::uint64_t>(result.error().code), result.error().offset}
                        });
                    }
                    return std::move(*result);
                }
            )),
            impl_->execution.mainScheduler()
        );
        if (!encoded)
        {
            co_return cxx::unexpected(assetError(encoded.error()));
        }
        if (record.source_revision != source_revision)
        {
            co_return cxx::unexpected(error::Error{kAssetChanged});
        }
        auto scheduler = impl_->execution.blockingIoScheduler();
        if (!scheduler)
        {
            co_return cxx::unexpected(execution::toError(scheduler.error()));
        }
        auto written = co_await stdexec::continues_on(
            execution::asResult(stdexec::then(
                stdexec::schedule(*scheduler),
                [this, id = record.id, bytes = std::move(*encoded)]() noexcept -> AssetResult<void>
                {
                    auto result = impl_->source.replace(id, bytes);
                    if (!result)
                    {
                        return cxx::unexpected(
                            error::Error{kAssetWriteFailure, {static_cast<std::uint64_t>(result.error())}}
                        );
                    }
                    return {};
                }
            )),
            impl_->execution.mainScheduler()
        );
        if (!written)
        {
            const auto failure = assetError(written.error());
            const bool publication_unknown =
                failure.type == kAssetWriteFailure &&
                failure.args[0] == static_cast<std::uint64_t>(asset::EAssetStorageError::PUBLICATION_UNKNOWN);
            if (publication_unknown)
            {
                impl_->invalidate(record);
            }
            // A failed atomic candidate did not replace the old file or its last-good CPU snapshot.
            co_return cxx::unexpected(failure);
        }
        impl_->invalidate(record);
        co_return AssetResult<void>{};
    }

    AssetResult<void> AssetRepository::finishContentWrite(
        detail::AssetRecord& asset,
        asset::EAssetPublication publication,
        AssetResult<void> outcome
    ) noexcept
    {
        auto& record = static_cast<Impl::Record&>(asset);
        if (publication == asset::EAssetPublication::PUBLISHED)
        {
            impl_->invalidate(record);
            return {}; // Publication, not cancellation or the callback's later result, is authoritative.
        }
        if (publication == asset::EAssetPublication::UNKNOWN)
        {
            impl_->invalidate(record);
            return cxx::unexpected(error::Error{
                kAssetWriteFailure,
                {static_cast<std::uint64_t>(asset::EAssetStorageError::PUBLICATION_UNKNOWN)}
            });
        }
        if (outcome)
        {
            return cxx::unexpected(
                error::Error{kAssetWriteFailure, {static_cast<std::uint64_t>(asset::EAssetStorageError::CORRUPT_IMAGE)}}
            ); // Returning success without publishing violates the operation contract.
        }
        return outcome;
    }

    exec::task<AssetResult<void>> AssetRepository::runContentWrite(
        AssetRef<asset::Asset> asset,
        AssetDataRef<void> base,
        asset::EAssetContentWrite mode,
        std::string path,
        std::function<exec::task<AssetResult<void>>(asset::AssetContentWriter&)> operation
    ) noexcept
    {
        if (impl_->tasks_.stopToken().stop_requested())
        {
            co_return cxx::unexpected(error::Error{kAssetRepositoryClosed});
        }
        auto& record = static_cast<Impl::Record&>(*asset.record_);
        if (record.writing)
        {
            co_return cxx::unexpected(error::Error{kAssetBusy});
        }
        const bool create = mode == asset::EAssetContentWrite::CREATE;
        const bool invalid_base =
            create ? bool(base) : (!base || &base.record_->asset != &record || !base.record_->source);
        if (invalid_base || !operation)
        {
            co_return cxx::unexpected(error::Error{kAssetChanged});
        }
        auto scheduler = impl_->execution.blockingIoScheduler();
        if (!scheduler)
        {
            co_return cxx::unexpected(execution::toError(scheduler.error()));
        }
        record.writing = true;
        cxx::scope_exit complete(
            [&record]() noexcept
            {
                record.writing = false;
                record.changed.request();
            }
        );
        const auto* physical = base ? &*base.record_->source : nullptr;
        auto writer = co_await stdexec::continues_on(
            execution::asResult(stdexec::then(
                stdexec::schedule(*scheduler),
                [this, id = record.id, physical, mode, &path]() noexcept -> AssetResult<asset::AssetContentWriter>
                {
                    auto opened = impl_->source.beginContentWrite(id, physical, mode, path);
                    if (!opened)
                    {
                        return cxx::unexpected(
                            error::Error{kAssetWriteFailure, {static_cast<std::uint64_t>(opened.error())}}
                        );
                    }
                    return std::move(*opened);
                }
            )),
            impl_->execution.mainScheduler()
        );
        if (!writer)
        {
            co_return cxx::unexpected(assetError(writer.error()));
        }
        auto performed =
            co_await stdexec::continues_on(execution::asResult(operation(*writer)), impl_->execution.mainScheduler());
        auto outcome =
            performed ? AssetResult<void>{} : AssetResult<void>{cxx::unexpected(assetError(performed.error()))};
        const auto publication = writer->publication();
        // Candidate removal and physical handle closure stay on blocking IO, before Main releases borrows.
        auto discarded = co_await stdexec::continues_on(
            execution::asResult(stdexec::then(
                stdexec::schedule(*scheduler),
                [writer = std::move(*writer)]() mutable noexcept -> AssetResult<void>
                {
                    asset::AssetContentWriter owned(std::move(writer));
                    return {};
                }
            )),
            impl_->execution.mainScheduler()
        );
        if (!discarded && outcome)
        {
            outcome = cxx::unexpected(assetError(discarded.error()));
        }
        co_return finishContentWrite(record, publication, std::move(outcome));
    }

    exec::task<AssetResult<AssetRef<asset::Asset>>> AssetRepository::createContentRecord(
        std::string path,
        asset::AssetInfo info,
        asset::AssetTypeId type,
        cxx::type_info data_type,
        std::function<exec::task<AssetResult<void>>(asset::AssetContentWriter&)> operation
    ) noexcept
    {
        if (impl_->tasks_.stopToken().stop_requested())
        {
            co_return cxx::unexpected(error::Error{kAssetRepositoryClosed});
        }
        const bool invalid = info.id.isNull() || info.type != type;
        if (invalid)
        {
            co_return cxx::unexpected(error::Error{kAssetTypeMismatch});
        }
        const auto codec = std::ranges::find_if(
            impl_->codecs,
            [&](const auto& value) noexcept { return value->type() == type && value->dataType() == data_type; }
        );
        if (codec == impl_->codecs.end())
        {
            co_return cxx::unexpected(error::Error{kAssetTypeMismatch});
        }
        if (impl_->records.contains(info.id) || impl_->source.describe(info.id))
        {
            co_return cxx::unexpected(
                error::Error{kAssetWriteFailure, {static_cast<std::uint64_t>(asset::EAssetStorageError::ID_CONFLICT)}}
            );
        }
        auto record = std::make_unique<Impl::Record>(*impl_, info.id, **codec);
        AssetRef<asset::Asset> reference{*record};
        impl_->records.emplace(info.id, std::move(record));
        auto created = co_await runContentWrite(
            reference,
            {},
            asset::EAssetContentWrite::CREATE,
            std::move(path),
            std::move(operation)
        );
        if (!created)
        {
            co_return cxx::unexpected(created.error());
        }
        co_return reference;
    }

    asset::AssetId AssetRepository::resolve(std::string_view path) const noexcept
    {
        return impl_->source.resolve(path);
    }

    const asset::AssetDecodeLimits& AssetRepository::decodeLimits() const noexcept
    {
        return impl_->limits;
    }

    std::optional<AssetUsage> AssetRepository::usage(asset::AssetId id) const noexcept
    {
        auto found = impl_->records.find(id);
        if (found == impl_->records.end())
        {
            return std::nullopt;
        }
        const auto& record = *found->second;
        return AssetUsage{
            record.references.load(std::memory_order_acquire),
            record.data_readers.load(std::memory_order_acquire),
            std::ranges::any_of(record.snapshots, [](const auto& data) noexcept { return bool(data->backing); }),
            record.use_readers.load(std::memory_order_acquire)
        };
    }

    void AssetRepository::collect() noexcept
    {
        if (!impl_->policy || impl_->tasks_.stopToken().stop_requested())
        {
            return;
        }
        impl_->collectUses();
        impl_->selected.clear();
        impl_->policy->selectReleases({}, nullptr, impl_->candidates, impl_->selected);
        for (auto id : impl_->selected)
        {
            const auto found = impl_->records.find(id);
            if (found != impl_->records.end())
            {
                auto& record = *found->second;
                const bool unused = record.current && !record.writing && !record.loading && record.waiters == 0 &&
                                    record.current->readers.load(std::memory_order_acquire) == 0;
                if (unused)
                {
                    record.current = nullptr;
                    record.changed.request();
                }
            }
        }
    }

#if defined(LUX_ASSET_FILEWATCH)
    exec::task<AssetResult<void>> AssetRepository::refresh(std::span<const platform::FileEvent> events) noexcept
    {
        co_await stdexec::schedule(impl_->execution.mainScheduler());
        if (impl_->tasks_.stopToken().stop_requested())
        {
            co_return cxx::unexpected(error::Error{kAssetRepositoryClosed});
        }
        ++impl_->pending_refreshes;
        cxx::scope_exit finished(
            [this]() noexcept
            {
                --impl_->pending_refreshes;
                impl_->changed();
            }
        );
        std::vector<std::filesystem::path> paths;
        paths.reserve(events.size() * 2);
        for (const auto& event : events)
        {
            paths.push_back(event.path);
            if (!event.old_path.empty())
            {
                paths.push_back(event.old_path);
            }
        }
        auto scheduler = impl_->execution.blockingIoScheduler();
        if (!scheduler)
        {
            co_return cxx::unexpected(execution::toError(scheduler.error()));
        }
        auto refreshed = co_await stdexec::continues_on(
            execution::asResult(stdexec::then(
                stdexec::schedule(*scheduler),
                [this, paths = std::move(paths)]() noexcept -> AssetResult<std::vector<asset::AssetId>>
                {
                    auto result = impl_->source.refresh(paths);
                    if (!result)
                    {
                        return cxx::unexpected(
                            error::Error{kAssetReadFailure, {static_cast<std::uint64_t>(result.error())}}
                        );
                    }
                    return std::move(*result);
                }
            )),
            impl_->execution.mainScheduler()
        );
        if (!refreshed)
        {
            // A provider may have published before another provider failed. Invalidate on this exceptional path.
            for (auto& [id, record] : impl_->records)
            {
                impl_->invalidate(*record);
            }
            co_return cxx::unexpected(assetError(refreshed.error()));
        }
        for (auto id : *refreshed)
        {
            const auto found = impl_->records.find(id);
            if (found != impl_->records.end())
            {
                impl_->invalidate(*found->second);
            }
        }
        co_return AssetResult<void>{};
    }

    AssetResult<void> AssetRepository::watch(platform::FileWatcher& watcher, std::chrono::milliseconds period) noexcept
    {
        if (impl_->tasks_.stopToken().stop_requested())
        {
            return cxx::unexpected(error::Error{kAssetRepositoryClosed});
        }
        if (impl_->watching || period.count() <= 0)
        {
            return cxx::unexpected(error::Error{kAssetBusy});
        }
        impl_->watching = true;
        auto submitted = impl_->tasks_.spawnOnMain(
            impl_->observe(*this, watcher, period),
            [impl = impl_.get()](auto outcome) noexcept
            {
                // Clear admission only at terminal delivery, not earlier when observe returns.
                impl->watching = false;
                if (!outcome && !outcome.error().isCancelled())
                {
                    const auto failure = assetError(outcome.error());
                    const bool normal_stop =
                        impl->tasks_.stopToken().stop_requested() &&
                        (failure.type == kAssetCancelled || failure.type == kAssetRepositoryClosed);
                    if (!normal_stop)
                    {
                        log::error("Assets", "Directory observation failed: {}", error::format(failure));
                    }
                }
            }
        );
        if (!submitted)
        {
            impl_->watching = false;
            return cxx::unexpected(execution::toError(submitted.error()));
        }
        return {};
    }
#else
    exec::task<AssetResult<void>> AssetRepository::refresh(std::span<const platform::FileEvent>) noexcept
    {
        co_return cxx::unexpected(execution::toError(execution::EExecutionError::CAPABILITY_UNAVAILABLE));
    }

    AssetResult<void> AssetRepository::watch(platform::FileWatcher&, std::chrono::milliseconds) noexcept
    {
        return cxx::unexpected(execution::toError(execution::EExecutionError::CAPABILITY_UNAVAILABLE));
    }
#endif

    exec::task<AssetResult<void>> AssetRepository::settleAndRelease() noexcept
    {
        impl_->tasks_.requestStop();
        co_await stdexec::continues_on(impl_->tasks_.whenSettled(), impl_->execution.mainScheduler());
        // Directly awaited writes/refreshes remain visible until their terminal adoption is complete.
        while (impl_->hasOperations())
        {
            co_await stdexec::continues_on(impl_->whenChanged(), impl_->execution.mainScheduler());
        }
        co_return co_await impl_->releaseResources();
    }

    bool AssetRepository::empty() const noexcept
    {
        return impl_->records.empty();
    }
} // namespace lux::engine::assets
