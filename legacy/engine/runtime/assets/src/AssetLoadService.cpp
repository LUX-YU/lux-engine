#include <lux/engine/runtime/assets/AssetLoadService.hpp>

#include <lux/engine/resource/asset/AssetSerDeser.hpp>
#include <lux/engine/runtime/execution/AsyncRuntimeSenders.hpp>
#include <lux/engine/runtime/execution/AsyncScopeSenders.hpp>
#include <lux/engine/log/Log.hpp>

#include <stdexec/execution.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <unordered_map>
#include <utility>
#include <vector>

namespace lux::asset_runtime
{
    namespace ex = stdexec;

    namespace
    {
        [[nodiscard]] bool isTransient(
            lux::asset::EAssetError error) noexcept
        {
            switch (error)
            {
            case lux::asset::EAssetError::FILE_OPEN_FAIL:
            case lux::asset::EAssetError::READ_FILE_FAIL:
            case lux::asset::EAssetError::OUT_OF_MEMORY:
            case lux::asset::EAssetError::UNKNOWN_FILESYSTEM_ERROR:
                return true;
            default:
                return false;
            }
        }

        struct AttemptResult final
        {
            lux::cxx::expected<AssetLoadResult, lux::asset::EAssetError> result;
            std::uint32_t observed_revision{0};
        };
    }

    struct AssetLoadService::State final :
        std::enable_shared_from_this<AssetLoadService::State>
    {
        enum class ERowStatus : std::uint8_t
        {
            IDLE,
            RUNNING,
            BACKOFF,
            TERMINAL
        };

        struct Row final
        {
            std::uint32_t revision{0};
            std::uint32_t attempts{0};
            ERowStatus status{ERowStatus::IDLE};
            lux::asset::EAssetError terminal_error{
                lux::asset::EAssetError::UNKNOWN_ERROR};
            std::shared_ptr<const lux::asset::AssetVfs> vfs;
            std::vector<lux::exec::AsyncOperationCompletion<LoadAsset>> waiters;
        };

        struct BatchJoin final
        {
            explicit BatchJoin(
                std::size_t count,
                lux::exec::AsyncOperationCompletion<LoadAssetBatch> terminal)
                : results(count)
                , remaining(count)
                , completion(std::move(terminal))
            {}

            void settle(
                std::size_t index,
                lux::async::OperationOutcome<LoadAsset> outcome) noexcept
            {
                if (settled)
                    return;
                if (!outcome)
                {
                    settled = true;
                    if (outcome.error().isRuntime())
                        completion.failRuntime(outcome.error().runtimeError());
                    else
                    {
                        completion.complete(lux::cxx::unexpected(
                            lux::async::OperationFailure<
                                lux::asset::EAssetError>::domain(
                                    outcome.error().domainError())));
                    }
                    return;
                }

                results[index] = std::move(*outcome);
                if (--remaining != 0u)
                    return;
                settled = true;
                completion.complete(AssetLoadBatchResult{
                    std::move(results)});
            }

            std::vector<AssetLoadResult> results;
            std::size_t remaining{0};
            bool settled{false};
            lux::exec::AsyncOperationCompletion<LoadAssetBatch> completion;
        };

        explicit State(lux::asset::AssetManager& manager_arg) noexcept
            : manager(&manager_arg)
        {}

        void bind(lux::exec::AsyncOperationContext& context) noexcept
        {
            if (!runtime)
            {
                runtime = &context.runtime();
                scope = &context.scope();
            }
        }

        void ensure(
            const EnsureAssetLoaded& operation,
            lux::exec::AsyncOperationContext context) noexcept
        {
            if (operation.already_ready)
                return;
            auto& row = prepare(
                operation.id,
                operation.revision,
                operation.vfs);
            if (row.status == ERowStatus::IDLE)
                start(operation.id, context);
        }

        void observe(
            LoadAsset&& operation,
            lux::exec::AsyncOperationContext context,
            lux::exec::AsyncOperationCompletion<LoadAsset> completion) noexcept
        {
            if (operation.already_ready)
            {
                completion.complete(AssetLoadResult{
                    operation.id,
                    operation.revision});
                return;
            }
            if (operation.id.is_nil() || !operation.vfs)
            {
                completion.complete(lux::cxx::unexpected(
                    lux::async::OperationFailure<lux::asset::EAssetError>::domain(
                        lux::asset::EAssetError::ASSET_NOT_EXIST)));
                return;
            }

            auto& row = prepare(
                operation.id,
                operation.revision,
                std::move(operation.vfs));
            if (row.status == ERowStatus::TERMINAL)
            {
                completion.complete(lux::cxx::unexpected(
                    lux::async::OperationFailure<lux::asset::EAssetError>::domain(
                        row.terminal_error)));
                return;
            }
            row.waiters.push_back(std::move(completion));
            if (row.status == ERowStatus::IDLE)
                start(operation.id, context);
        }

        void observeBatch(
            LoadAssetBatch&& operation,
            lux::exec::AsyncOperationContext context,
            lux::exec::AsyncOperationCompletion<LoadAssetBatch> completion)
            noexcept
        {
            if (!operation.assets)
            {
                completion.complete(lux::cxx::unexpected(
                    lux::async::OperationFailure<
                        lux::asset::EAssetError>::domain(
                            lux::asset::EAssetError::ASSET_NOT_EXIST)));
                return;
            }
            if (operation.assets->empty())
            {
                completion.complete(AssetLoadBatchResult{});
                return;
            }

            auto join = std::make_shared<BatchJoin>(
                operation.assets->size(),
                std::move(completion));
            for (std::size_t index = 0; index < operation.assets->size(); ++index)
            {
                auto child = lux::exec::AsyncOperationCompletion<LoadAsset>{
                    [join, index](void* opaque) noexcept
                    {
                        auto& outcome = *static_cast<
                            lux::async::OperationOutcome<LoadAsset>*>(opaque);
                        join->settle(index, std::move(outcome));
                    }};
                observe(
                    LoadAsset{(*operation.assets)[index]},
                    context,
                    std::move(child));
            }
        }

        Row& prepare(
            const lux::asset::asset_id_t& id,
            std::uint32_t revision,
            std::shared_ptr<const lux::asset::AssetVfs> vfs)
        {
            auto& row = rows[id];
            if (row.revision != revision)
            {
                row.revision = revision;
                row.attempts = 0;
                row.status = ERowStatus::IDLE;
                row.terminal_error = lux::asset::EAssetError::UNKNOWN_ERROR;
            }
            if (vfs)
                row.vfs = std::move(vfs);
            return row;
        }

        void invalidate(
            const InvalidateAssetLoad& operation,
            lux::exec::AsyncOperationContext context) noexcept
        {
            auto found = rows.find(operation.id);
            if (found == rows.end())
                return;
            Row& row = found->second;
            row.revision = operation.revision;
            row.attempts = 0;
            row.terminal_error = lux::asset::EAssetError::UNKNOWN_ERROR;
            if (row.status != ERowStatus::RUNNING)
            {
                row.status = ERowStatus::IDLE;
                if (!row.waiters.empty())
                    start(operation.id, context);
            }
        }

        void start(
            const lux::asset::asset_id_t& id,
            lux::exec::AsyncOperationContext context) noexcept
        {
            auto found = rows.find(id);
            if (found == rows.end())
                return;
            Row& row = found->second;
            if (row.status == ERowStatus::RUNNING || !row.vfs ||
                closing.load(std::memory_order_acquire))
                return;

            row.status = ERowStatus::RUNNING;
            const auto revision = row.revision;
            auto vfs = row.vfs;
            auto self = shared_from_this();
            auto codecs = manager->codecCatalogOwner();

            auto attempt = ex::schedule(
                    lux::exec::blockingIoScheduler(context.runtime()))
                | ex::then(
                      [id, vfs]() noexcept
                          -> lux::cxx::expected<
                              lux::asset::AssetBlob,
                              lux::asset::EAssetError>
                      {
                          return vfs->open(id);
                      })
                | ex::continues_on(
                      lux::exec::backgroundCpuScheduler(context.runtime()))
                | ex::then(
                      [codecs](lux::cxx::expected<
                             lux::asset::AssetBlob,
                             lux::asset::EAssetError> blob) noexcept
                          -> lux::cxx::expected<
                              std::unique_ptr<lux::asset::LuxAsset>,
                              lux::asset::EAssetError>
                      {
                          if (!blob)
                              return lux::cxx::unexpected(blob.error());
                          return codecs->decodeAsset(blob->bytes);
                      })
                | ex::continues_on(
                      lux::exec::mainThreadScheduler(context.runtime()))
                | ex::then(
                      [self, id, revision](
                          lux::cxx::expected<
                              std::unique_ptr<lux::asset::LuxAsset>,
                              lux::asset::EAssetError> decoded) mutable noexcept
                      {
                          return self->installMain(
                              id,
                              revision,
                              std::move(decoded));
                      })
                | ex::continues_on(
                      lux::exec::coordinatorScheduler(context.runtime()))
                | ex::then(
                      [self, id, revision, context](
                          AttemptResult result) mutable noexcept
                      {
                          self->finish(
                              id,
                              revision,
                              std::move(result),
                              context);
                      })
                | ex::upon_stopped(
                      [self, id]() noexcept
                      {
                          self->stopRow(id);
                      });

            if (!lux::exec::spawn(context.scope(), std::move(attempt)))
                stopRow(id);
        }

        AttemptResult installMain(
            const lux::asset::asset_id_t& id,
            std::uint32_t revision,
            lux::cxx::expected<
                std::unique_ptr<lux::asset::LuxAsset>,
                lux::asset::EAssetError> decoded) noexcept
        {
            const auto observed = manager->contentRevision(id);
            if (observed != revision)
            {
                return AttemptResult{
                    lux::cxx::unexpected(
                        lux::asset::EAssetError::RELEASED),
                    observed};
            }
            if (!decoded)
                return AttemptResult{
                    lux::cxx::unexpected(decoded.error()),
                    observed};
            auto installed = manager->installLoadedAsset(
                id,
                std::move(*decoded)
            );
            if (!installed)
                return AttemptResult{
                    lux::cxx::unexpected(installed.error()),
                    observed};
            return AttemptResult{AssetLoadResult{id, revision}, observed};
        }

        void finish(
            const lux::asset::asset_id_t& id,
            std::uint32_t attempt_revision,
            AttemptResult result,
            lux::exec::AsyncOperationContext context) noexcept
        {
            auto found = rows.find(id);
            if (found == rows.end())
                return;
            Row& row = found->second;
            if (row.revision != attempt_revision ||
                result.observed_revision != attempt_revision)
            {
                row.revision = result.observed_revision;
                row.attempts = 0;
                row.status = ERowStatus::IDLE;
                if (!closing.load(std::memory_order_acquire))
                    start(id, context);
                return;
            }

            if (result.result)
            {
                complete(row, std::move(*result.result));
                rows.erase(found);
                return;
            }

            const auto error = result.result.error();
            if (isTransient(error) &&
                !closing.load(std::memory_order_acquire))
            {
                row.status = ERowStatus::BACKOFF;
                const auto shift = (std::min)(row.attempts, 6u);
                const auto delay = (std::min<std::chrono::steady_clock::duration>)(
                    std::chrono::milliseconds(500) * (1u << shift),
                    std::chrono::seconds(30));
                ++row.attempts;
                if (row.attempts == 1u)
                {
                    lux::log::warn(
                        "asset",
                        "{}: transient load failure (err={}); retry is runtime-timed",
                        uuids::to_string(id),
                        static_cast<int>(error));
                }
                auto self = shared_from_this();
                auto retry_sender = lux::exec::scheduleAfter(
                        context.runtime(), delay)
                    | ex::then(
                        [self, id, context]() mutable noexcept
                        {
                            auto retry = self->rows.find(id);
                            if (retry == self->rows.end() ||
                                self->closing.load(std::memory_order_acquire))
                                return;
                            retry->second.status = ERowStatus::IDLE;
                            self->start(id, context);
                        })
                    | ex::upon_stopped(
                        [self, id]() noexcept
                        {
                            self->stopRow(id);
                        });
                if (!lux::exec::spawn(
                        context.scope(), std::move(retry_sender)))
                {
                    failRuntime(row, lux::async::ESubmitError::STOPPING);
                }
                return;
            }

            row.status = ERowStatus::TERMINAL;
            row.terminal_error = error;
            lux::log::error(
                "asset",
                "{}: terminal load failure (err={})",
                uuids::to_string(id),
                static_cast<int>(error));
            failDomain(row, error);
        }

        static void complete(Row& row, AssetLoadResult result) noexcept
        {
            auto waiters = std::move(row.waiters);
            row.waiters.clear();
            for (auto& waiter : waiters)
                waiter.complete(result);
        }

        static void failDomain(
            Row& row,
            lux::asset::EAssetError error) noexcept
        {
            auto waiters = std::move(row.waiters);
            row.waiters.clear();
            for (auto& waiter : waiters)
            {
                waiter.complete(lux::cxx::unexpected(
                    lux::async::OperationFailure<lux::asset::EAssetError>::domain(
                        error)));
            }
        }

        static void failRuntime(
            Row& row,
            lux::async::ESubmitError error) noexcept
        {
            auto waiters = std::move(row.waiters);
            row.waiters.clear();
            for (auto& waiter : waiters)
                waiter.failRuntime(error);
        }

        void stopRow(const lux::asset::asset_id_t& id) noexcept
        {
            auto found = rows.find(id);
            if (found == rows.end())
                return;
            failRuntime(
                found->second,
                lux::async::ESubmitError::STOPPING);
            rows.erase(found);
        }

        lux::asset::AssetManager* manager{nullptr};
        lux::exec::AsyncRuntime* runtime{nullptr};
        lux::exec::AsyncScope* scope{nullptr};
        std::unordered_map<lux::asset::asset_id_t, Row> rows;
        std::atomic<bool> closing{false};
    };

    lux::cxx::expected<
        AssetLoadService,
        lux::exec::AsyncAssemblyFailure>
    AssetLoadService::addTo(
        lux::exec::AsyncRuntimeBuilder& builder,
        lux::asset::AssetManager& manager)
    {
        auto state = std::make_shared<State>(manager);
        auto ensure = builder.addOperation<EnsureAssetLoaded>(
            [state](
                EnsureAssetLoaded&& operation,
                lux::exec::AsyncOperationContext& context,
                lux::exec::AsyncOperationCompletion<
                    EnsureAssetLoaded>&& completion) noexcept
            {
                state->bind(context);
                if (operation.id.is_nil() ||
                    (!operation.already_ready && !operation.vfs))
                {
                    completion.complete(lux::cxx::unexpected(
                        lux::async::OperationFailure<
                            lux::asset::EAssetError>::domain(
                                lux::asset::EAssetError::ASSET_NOT_EXIST)));
                    return;
                }
                state->ensure(operation, context);
                completion.complete({});
            }, {}, lux::exec::AsyncOperationQueueConfig{
                .capacity = 1024,
                .byte_budget = 8u * 1024u * 1024u,
                .drain_batch = 64});
        if (!ensure)
            return lux::cxx::unexpected(ensure.error());

        auto load = builder.addOperation<LoadAsset>(
            [state](
                LoadAsset&& operation,
                lux::exec::AsyncOperationContext& context,
                lux::exec::AsyncOperationCompletion<LoadAsset>&& completion) noexcept
            {
                state->bind(context);
                state->observe(
                    std::move(operation),
                    context,
                    std::move(completion));
            }, {}, lux::exec::AsyncOperationQueueConfig{
                .capacity = 1024,
                .byte_budget = 8u * 1024u * 1024u,
                .drain_batch = 64});
        if (!load)
            return lux::cxx::unexpected(load.error());

        auto load_batch = builder.addOperation<LoadAssetBatch>(
            [state](
                LoadAssetBatch&& operation,
                lux::exec::AsyncOperationContext& context,
                lux::exec::AsyncOperationCompletion<
                    LoadAssetBatch>&& completion) noexcept
            {
                state->bind(context);
                state->observeBatch(
                    std::move(operation),
                    context,
                    std::move(completion));
            }, {}, lux::exec::AsyncOperationQueueConfig{
                .capacity = 256,
                .byte_budget = 64u * 1024u * 1024u,
                .drain_batch = 32});
        if (!load_batch)
            return lux::cxx::unexpected(load_batch.error());

        auto invalidate = builder.addOperation<InvalidateAssetLoad>(
            [state](
                InvalidateAssetLoad&& operation,
                lux::exec::AsyncOperationContext& context,
                lux::exec::AsyncOperationCompletion<
                    InvalidateAssetLoad>&& completion) noexcept
            {
                state->bind(context);
                state->invalidate(operation, context);
                completion.complete({});
            }, {}, lux::exec::AsyncOperationQueueConfig{
                .capacity = 256,
                .byte_budget = 1024u * 1024u,
                .drain_batch = 32});
        if (!invalidate)
            return lux::cxx::unexpected(invalidate.error());

        return AssetLoadService{
            std::move(state),
            manager,
            std::move(*ensure),
            std::move(*load),
            std::move(*load_batch),
            std::move(*invalidate)};
    }

    AssetLoadService::~AssetLoadService()
    {
        close();
    }

    AssetLoadService::AssetLoadService(
        AssetLoadService&& other) noexcept
        : state_(std::move(other.state_))
        , manager_(std::exchange(other.manager_, nullptr))
        , ensure_(std::move(other.ensure_))
        , load_(std::move(other.load_))
        , load_batch_(std::move(other.load_batch_))
        , invalidate_(std::move(other.invalidate_))
        , closed_(std::exchange(other.closed_, true))
    {
    }

    AssetLoadService& AssetLoadService::operator=(
        AssetLoadService&& other) noexcept
    {
        if (this == &other)
            return *this;
        close();
        state_ = std::move(other.state_);
        manager_ = std::exchange(other.manager_, nullptr);
        ensure_ = std::move(other.ensure_);
        load_ = std::move(other.load_);
        load_batch_ = std::move(other.load_batch_);
        invalidate_ = std::move(other.invalidate_);
        closed_ = std::exchange(other.closed_, true);
        return *this;
    }

    AssetClient AssetLoadService::client() const noexcept
    {
        if (closed_ || !manager_)
            return {};
        return AssetClient{
            *manager_, ensure_, load_, load_batch_, invalidate_};
    }

    void AssetLoadService::close() noexcept
    {
        if (closed_)
            return;
        if (state_)
            state_->closing.store(true, std::memory_order_release);
        ensure_ = {};
        load_ = {};
        load_batch_ = {};
        invalidate_ = {};
        closed_ = true;
    }

}
