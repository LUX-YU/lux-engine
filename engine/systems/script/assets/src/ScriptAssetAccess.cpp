#include <lux/engine/scene/scripting/ScriptAssetAccess.hpp>

#include <algorithm>
#include <limits>
#include <thread>
#include <variant>
#include <vector>

namespace lux::scene::script
{
    namespace
    {
        using namespace lux::engine::assets;
        using lux::cxx::unexpected;
        using lux::execution::EExecutionError;

        struct AccessState final
        {
            lux::execution::TaskScope* tasks;
            AssetRepository& assets;
            lux::asset::AssetVfsView source;
            ScriptAssetLimits limits;
            const std::thread::id owner{std::this_thread::get_id()};
        };

        [[nodiscard]] ScriptAssetReadOutcome mapFailure(const error::Error& failure) noexcept
        {
            using Domain = EScriptAssetFailureDomain;
            if (failure.type == kAssetReadFailure)
            {
                return ScriptAssetReadOutcome::failure(Domain::STORAGE, static_cast<std::uint32_t>(failure.args[0]));
            }
            if (failure.type == kAssetDecodeFailure)
            {
                return ScriptAssetReadOutcome::failure(Domain::DECODE, static_cast<std::uint32_t>(failure.args[0]));
            }
            if (failure.type == kAssetCancelled)
            {
                return ScriptAssetReadOutcome::failure(Domain::CANCELLED, 0);
            }
            return ScriptAssetReadOutcome::failure(Domain::EXECUTION, 0);
        }
    } // namespace

    struct ScriptAssetScope::State final
    {
        struct Pending final
        {
            Completion completion;
            ScriptAssetReadOutcome outcome;
        };

        // Completion delivery can remain pending after the owned result is ready.
        using VResult = std::variant<std::monostate, lux::asset::AssetBlob, AssetDataRef<lux::asset::Asset>>;

        struct Record final
        {
            ScriptAssetHandle handle;
            lux::asset::AssetId id;
            lux::execution::TaskId task;
            std::size_t reserved{};
            AssetRef<lux::asset::Asset> asset;
            VResult result;
            std::optional<Pending> pending;
        };

        State(std::shared_ptr<AccessState> access, lux::simulation::script::ScriptInstanceId instance)
            : access(std::move(access)), instance(instance)
        {
            // One compiled source, not one inline counter per plugin/consumer DSO.
            static lux::cxx::ScopeIdSource<ScriptAssetScopeTag> domains;
            domain = domains.acquire();
            records.reserve(this->access->limits.results_per_scope);
            delivery_keys.reserve(this->access->limits.results_per_scope);
        }

        [[nodiscard]] bool onOwner() const noexcept
        {
            return access->owner == std::this_thread::get_id();
        }

        [[nodiscard]] Record* find(ScriptAssetHandle handle) noexcept
        {
            return handle.domain == domain ? records.find(handle.slot) : nullptr;
        }

        void remove(ScriptAssetHandle handle) noexcept
        {
            auto* record = find(handle);
            if (record == nullptr)
            {
                return;
            }
            // Erase only empty owners. Their destructors may enter native capabilities again.
            auto result = std::move(record->result);
            auto pending = std::move(record->pending);
            bytes -= record->reserved;
            records.erase(handle.slot);
        }

        void deliver(ScriptAssetHandle handle) noexcept
        {
            auto* record = find(handle);
            const bool has_pending_result = record != nullptr && record->pending.has_value();
            if (!has_pending_result)
            {
                return;
            }
            auto pending = std::move(*record->pending);
            record->pending.reset();
            const auto result = pending.completion.success(pending.outcome);
            // Completion callbacks may revoke this scope or release a delivered result.
            record = find(handle);
            if (record == nullptr)
            {
                return;
            }
            const bool is_backpressure =
                !result && result.error() == lux::script::EScriptAbilityCompletionError::BACKPRESSURE;
            const bool must_remove = !result || !pending.outcome.succeeded();
            if (is_backpressure)
            {
                record->pending.emplace(std::move(pending));
            }
            else if (must_remove)
            {
                remove(handle);
            }
        }

        void deliverCompletions() noexcept
        {
            const bool cannot_deliver = delivering || revoked;
            if (cannot_deliver)
            {
                return;
            }
            delivering = true;
            delivery_keys.clear();
            for (const auto& record : records)
            {
                if (record.pending)
                {
                    delivery_keys.push_back(record.handle);
                }
            }
            for (const auto handle : delivery_keys)
            {
                deliver(handle);
            }
            delivery_keys.clear();
            delivering = false;
        }

        void revoke() noexcept
        {
            if (!onOwner())
            {
                std::terminate();
            }
            if (revoked)
            {
                return;
            }
            revoked = true;
            while (!records.empty())
            {
                const auto handle = records.values().front().handle;
                const auto task = records.values().front().task;
                const bool can_cancel = task && access->tasks != nullptr;
                if (can_cancel)
                {
                    (void)access->tasks->execution().requestStop(task);
                }
                remove(handle);
            }
        }

        [[nodiscard]] lux::cxx::expected<Record*, EScriptAssetError> ready(ScriptAssetHandle handle) noexcept
        {
            if (!onOwner())
            {
                return unexpected(EScriptAssetError::WRONG_THREAD);
            }
            auto* record = find(handle);
            if (record == nullptr)
            {
                return unexpected(EScriptAssetError::INVALID_HANDLE);
            }
            if (std::holds_alternative<std::monostate>(record->result))
            {
                return unexpected(EScriptAssetError::NOT_READY);
            }
            return record;
        }

        std::shared_ptr<AccessState> access;
        const lux::simulation::script::ScriptInstanceId instance;
        lux::cxx::ScopeId<ScriptAssetScopeTag> domain;
        lux::cxx::SlotMap<Record, ScriptAssetResultTag> records;
        std::vector<ScriptAssetHandle> delivery_keys;
        std::optional<error::Error> failure;
        std::size_t bytes{};
        bool revoked{};
        bool delivering{};
        std::weak_ptr<ScriptAssetScope> scope;
    };

    struct ScriptAssetAccess::Impl final
    {
        Impl(
            lux::execution::ExecutionRuntime& execution,
            AssetRepository& assets,
            lux::asset::AssetVfsView source,
            ScriptAssetLimits limits
        )
            : tasks(execution), access(std::make_shared<AccessState>(&tasks, assets, source.capture(), limits))
        {
            scopes.reserve(limits.scopes);
            delivery_scopes.reserve(limits.scopes);
        }

        lux::execution::TaskScope tasks;
        std::shared_ptr<AccessState> access;
        std::vector<std::weak_ptr<ScriptAssetScope::State>> scopes;
        std::vector<std::weak_ptr<ScriptAssetScope::State>> delivery_scopes;
        bool delivering{};
    };

    ScriptAssetAccess::CreateResult ScriptAssetAccess::create(
        lux::execution::ExecutionRuntime& execution,
        AssetRepository& assets,
        lux::asset::AssetVfsView source,
        ScriptAssetLimits limits
    ) noexcept
    {
        const bool has_invalid_capacity =
            limits.scopes == 0 || limits.results_per_scope == 0 || limits.results_per_scope >= UINT32_MAX;
        const bool has_invalid_limits = limits.decode.max_image_bytes == 0 ||
                                        limits.decode.max_image_bytes > limits.retained_bytes_per_scope ||
                                        limits.decode.max_decoded_bytes > SIZE_MAX - limits.decode.max_image_bytes;
        const bool is_invalid_input = !source || has_invalid_capacity || has_invalid_limits;
        if (is_invalid_input)
        {
            return unexpected(EScriptAssetError::INVALID_INPUT);
        }
        if (!execution.blockingIoScheduler())
        {
            return unexpected(EScriptAssetError::EXECUTION_REJECTED);
        }
        return std::unique_ptr<ScriptAssetAccess>{
            new ScriptAssetAccess{std::make_shared<Impl>(execution, assets, std::move(source), limits)}
        };
    }

    ScriptAssetAccess::ScriptAssetAccess(std::shared_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}

    ScriptAssetAccess::~ScriptAssetAccess() noexcept
    {
        const auto impl = impl_;
        if (impl->access->owner != std::this_thread::get_id())
        {
            std::terminate();
        }
        // Revoke admission before cancellation, result cleanup or provider destruction can re-enter.
        impl->access->tasks = nullptr;
        impl->access->source = {};
        impl->tasks.requestStop();
        for (const auto& weak : impl->scopes)
        {
            if (auto state = weak.lock())
            {
                state->revoke();
            }
        }
    }

    void ScriptAssetAccess::deliverCompletions() noexcept
    {
        const auto impl = impl_;
        if (impl->access->owner != std::this_thread::get_id())
        {
            std::terminate();
        }
        const bool cannot_deliver = impl->delivering || impl->access->tasks == nullptr;
        if (cannot_deliver)
        {
            return;
        }
        impl->delivering = true;
        impl->delivery_scopes = impl->scopes;
        for (const auto& weak : impl->delivery_scopes)
        {
            if (auto state = weak.lock())
            {
                state->deliverCompletions();
            }
        }
        impl->delivery_scopes.clear();
        impl->delivering = false;
    }

    lux::cxx::expected<std::shared_ptr<ScriptAssetScope>, EScriptAssetError>
    ScriptAssetAccess::prepare(lux::simulation::script::ScriptInstanceId instance) noexcept
    {
        auto& access = *impl_->access;
        if (access.owner != std::this_thread::get_id())
        {
            return unexpected(EScriptAssetError::WRONG_THREAD);
        }
        if (access.tasks == nullptr)
        {
            return unexpected(EScriptAssetError::STOPPING);
        }
        if (!instance.valid())
        {
            return unexpected(EScriptAssetError::INVALID_INPUT);
        }
        std::erase_if(impl_->scopes, [](const auto& weak) noexcept { return weak.expired(); });
        // Several independently declared abilities may project the same instance-owned scope.
        // One Access belongs to one ScriptSystem; it must not merge IDs from different systems.
        for (const auto& weak : impl_->scopes)
        {
            auto state = weak.lock();
            const bool is_other_instance = !state || state->instance != instance;
            if (is_other_instance)
            {
                continue;
            }
            if (state->revoked)
            {
                return unexpected(EScriptAssetError::STOPPING);
            }
            if (auto scope = state->scope.lock())
            {
                return scope;
            }
            return unexpected(EScriptAssetError::STOPPING);
        }
        if (impl_->scopes.size() >= access.limits.scopes)
        {
            return unexpected(EScriptAssetError::CAPACITY_EXCEEDED);
        }
        auto state = std::make_shared<ScriptAssetScope::State>(impl_->access, instance);
        auto scope = std::shared_ptr<ScriptAssetScope>{new ScriptAssetScope{state}};
        state->scope = scope;
        impl_->scopes.push_back(state);
        return scope;
    }

    lux::simulation::script::ScriptApiInstanceResult ScriptAssetAccess::prepareInstance(
        void* context,
        lux::simulation::script::ScriptInstanceId instance
    ) noexcept
    {
        using Error = lux::simulation::script::EScriptApiPrepareError;
        if (context == nullptr)
        {
            return unexpected(Error::INVALID_INSTANCE);
        }
        auto scope = static_cast<ScriptAssetAccess*>(context)->prepare(instance);
        if (!scope)
        {
            if (scope.error() == EScriptAssetError::CAPACITY_EXCEEDED)
            {
                return unexpected(Error::CAPACITY_EXCEEDED);
            }
            if (scope.error() == EScriptAssetError::STOPPING)
            {
                return unexpected(Error::STOPPING);
            }
            return unexpected(Error::INVALID_INSTANCE);
        }
        auto* target = scope->get();
        return lux::simulation::script::ScriptApiInstanceBinding::create(
            std::move(*scope),
            target,
            [](void* scope) noexcept { static_cast<ScriptAssetScope*>(scope)->revoke(); }
        );
    }

    ScriptAssetScope::~ScriptAssetScope() noexcept
    {
        revoke();
    }

    void ScriptAssetScope::revoke() noexcept
    {
        const auto state = state_;
        state->revoke();
    }

    lux::cxx::expected<ScriptAssetScope::Reservation, EScriptAssetError> ScriptAssetScope::reserve(
        const std::shared_ptr<State>& owner,
        lux::asset::AssetId id,
        bool typed,
        bool completion_active
    ) noexcept
    {
        auto& state = *owner;
        if (!state.onOwner())
        {
            return unexpected(EScriptAssetError::WRONG_THREAD);
        }
        const bool is_revoked = state.revoked || state.access->tasks == nullptr;
        if (is_revoked)
        {
            return unexpected(EScriptAssetError::STOPPING);
        }
        const bool is_invalid_input = id.isNull() || !completion_active;
        if (is_invalid_input)
        {
            return unexpected(EScriptAssetError::INVALID_INPUT);
        }
        const auto& limits = state.access->limits;
        const auto& decode = state.access->assets.decodeLimits();
        const bool exceeds_script_budget = typed && (
            decode.max_image_bytes > limits.decode.max_image_bytes ||
            decode.max_decoded_bytes > limits.decode.max_decoded_bytes ||
            decode.max_auxiliary_payloads > limits.decode.max_auxiliary_payloads
        );
        if (exceeds_script_budget)
        {
            return unexpected(EScriptAssetError::CAPACITY_EXCEEDED);
        }
        const auto reservation = limits.decode.max_image_bytes + (typed ? limits.decode.max_decoded_bytes : 0);
        const bool is_full = state.records.size() >= limits.results_per_scope ||
                             reservation > limits.retained_bytes_per_scope - state.bytes;
        if (is_full)
        {
            return unexpected(EScriptAssetError::CAPACITY_EXCEEDED);
        }
        const auto key = state.records.emplace();
        const ScriptAssetHandle handle{state.domain, key};
        auto* record = state.records.find(key);
        record->handle = handle;
        record->id = id;
        record->reserved = reservation;
        state.bytes += reservation;
        return Reservation{
            handle,
            *state.access->tasks,
            state.access->assets,
            state.access->source,
            limits.decode
        };
    }

    ScriptAssetScope::StartResult ScriptAssetScope::finishStart(
        const std::shared_ptr<State>& state,
        ScriptAssetHandle handle,
        lux::cxx::expected<lux::execution::TaskId, execution::EExecutionError> submitted
    ) noexcept
    {
        if (!submitted)
        {
            state->failure = execution::toError(submitted.error());
            state->remove(handle);
            return unexpected(EScriptAssetError::EXECUTION_REJECTED);
        }
        if (auto* record = state->find(handle))
        {
            record->task = *submitted;
        }
        return {};
    }

    ScriptAssetScope::StartResult ScriptAssetScope::readAsset(lux::asset::AssetId id, Completion completion) noexcept
    {
        const auto state = state_;
        auto reserved = reserve(state, id, false, completion.active());
        if (!reserved)
        {
            return unexpected(reserved.error());
        }
        const auto handle = reserved->handle;
        using Result = AssetResult<lux::asset::AssetBlob>;
        auto submitted = reserved->tasks.submit(
            {"Read script asset image", "script"},
            [&reserved, id, state, handle, completion](lux::execution::TaskReporter) noexcept
            {
                auto blocking = reserved->tasks.execution().blockingIoScheduler();
                if (!blocking)
                {
                    // Factory validation establishes this resource for the access lifetime.
                    std::terminate();
                }
                auto read = stdexec::then(
                    stdexec::schedule(*blocking),
                    [source = reserved->source, id, limit = reserved->limits.max_image_bytes]() noexcept -> Result
                    {
                        auto image = source.open(id, limit);
                        if (!image)
                        {
                            return unexpected(error::Error{
                                kAssetReadFailure, {static_cast<std::uint64_t>(image.error())}
                            });
                        }
                        return std::move(*image);
                    }
                );
                auto ready = stdexec::continues_on(
                    lux::execution::asResult(std::move(read)),
                    reserved->tasks.execution().mainScheduler()
                );
                return stdexec::then(
                    std::move(ready),
                    [state,
                     handle,
                     completion](
                        lux::execution::ExecutionResult<lux::asset::AssetBlob, error::Error>&& result
                    ) noexcept
                    {
                        const auto outcome = lux::execution::taskState(result);
                        if (!result)
                        {
                            completeFailure(state, handle, completion, std::move(result.error()));
                            return outcome;
                        }
                        auto* record = state->find(handle);
                        if (record == nullptr)
                        {
                            return outcome;
                        }
                        record->result.emplace<lux::asset::AssetBlob>(std::move(*result));
                        record->pending.emplace(completion, ScriptAssetReadOutcome::success(handle));
                        state->deliver(handle);
                        return outcome;
                    }
                );
            }
        );
        return finishStart(state, handle, std::move(submitted));
    }

    void ScriptAssetScope::completeFailure(
        const std::shared_ptr<State>& state,
        ScriptAssetHandle handle,
        Completion completion,
        lux::execution::ExecutionFailure<LoadFailure> failure
    ) noexcept
    {
        if (state->find(handle) == nullptr)
        {
            return;
        }
        auto outcome = ScriptAssetReadOutcome::failure(EScriptAssetFailureDomain::CANCELLED, 0);
        if (auto* native = failure.domainFailure())
        {
            state->failure = *native;
            outcome = mapFailure(*native);
        }
        else if (const auto* execution = failure.executionFailure())
        {
            state->failure = lux::execution::toError(*execution);
            outcome = mapFailure(*state->failure);
        }
        state->find(handle)->pending.emplace(completion, outcome);
        state->deliver(handle);
    }

    void ScriptAssetScope::completeTyped(
        const std::shared_ptr<State>& state,
        ScriptAssetHandle handle,
        Completion completion,
        AssetRef<lux::asset::Asset> asset,
        AssetDataRef<lux::asset::Asset> data
    ) noexcept
    {
        auto* record = state->find(handle);
        if (record == nullptr)
        {
            return;
        }
        record->asset = std::move(asset);
        record->result.emplace<AssetDataRef<lux::asset::Asset>>(std::move(data));
        record->pending.emplace(completion, ScriptAssetReadOutcome::success(handle));
        state->deliver(handle);
    }

    lux::cxx::expected<ScriptAssetDescription, EScriptAssetError> ScriptAssetScope::describeAsset(
        ScriptAssetHandle handle
    ) const noexcept
    {
        auto record = state_->ready(handle);
        if (!record)
        {
            return unexpected(record.error());
        }
        if (const auto* image = std::get_if<lux::asset::AssetBlob>(&(*record)->result))
        {
            return ScriptAssetDescription{(*record)->id, {}, image->bytes.size(), false, true};
        }
        const auto& asset = std::get<AssetDataRef<lux::asset::Asset>>((*record)->result);
        return ScriptAssetDescription{(*record)->id, asset->type(), 0, true, false};
    }

    lux::cxx::expected<AssetByteChunk, EScriptAssetError> ScriptAssetScope::copyAssetBytes(
        ScriptAssetHandle handle,
        std::uint64_t offset,
        std::uint32_t count
    ) const noexcept
    {
        auto record = state_->ready(handle);
        if (!record)
        {
            return unexpected(record.error());
        }
        const auto* image = std::get_if<lux::asset::AssetBlob>(&(*record)->result);
        if (image == nullptr)
        {
            return unexpected(EScriptAssetError::TYPE_MISMATCH);
        }
        const auto& bytes = image->bytes;
        const bool is_invalid_start = offset > bytes.size();
        const bool is_invalid_range = is_invalid_start || count > AssetByteChunk::Capacity ||
                                      count > bytes.size() - static_cast<std::size_t>(offset);
        if (is_invalid_range)
        {
            return unexpected(EScriptAssetError::INVALID_RANGE);
        }
        AssetByteChunk result;
        result.size = count;
        if (count != 0)
        {
            std::copy_n(bytes.data() + offset, count, result.bytes.data());
        }
        return result;
    }

    ScriptAssetScope::StartResult ScriptAssetScope::releaseAsset(ScriptAssetHandle handle) noexcept
    {
        const auto state = state_;
        auto record = state->ready(handle);
        if (!record)
        {
            return unexpected(record.error());
        }
        state->remove(handle);
        return {};
    }

    lux::cxx::expected<AssetDataRef<lux::asset::Asset>, EScriptAssetError>
    ScriptAssetScope::borrowTyped(ScriptAssetHandle handle) const noexcept
    {
        auto record = state_->ready(handle);
        if (!record)
        {
            return unexpected(record.error());
        }
        const auto* asset = std::get_if<AssetDataRef<lux::asset::Asset>>(&(*record)->result);
        if (asset == nullptr)
        {
            return unexpected(EScriptAssetError::TYPE_MISMATCH);
        }
        return *asset;
    }

    std::size_t ScriptAssetScope::retainedResults() const noexcept
    {
        return state_->records.size();
    }

    std::size_t ScriptAssetScope::reservedBytes() const noexcept
    {
        return state_->bytes;
    }

    std::optional<error::Error> ScriptAssetScope::lastFailure() const noexcept
    {
        return state_->failure;
    }
} // namespace lux::scene::script
