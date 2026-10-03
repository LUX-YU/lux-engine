#include <lux/engine/scene/scripting/ScriptAssetAccess.hpp>

#include <algorithm>
#include <limits>
#include <thread>
#include <vector>

namespace lux::scene::script
{
    namespace
    {
        using namespace lux::process::asset_loading;
        using lux::cxx::unexpected;
        using lux::process::EExecutionError;

        struct AccessState final
        {
            lux::process::TaskScope* tasks;
            AssetReadPort read;
            ScriptAssetLimits limits;
            const std::thread::id owner{std::this_thread::get_id()};
            bool stopping{};
        };

        [[nodiscard]] ScriptAssetReadOutcome mapFailure(const AssetLoadFailure& failure) noexcept
        {
            using Domain = EScriptAssetFailureDomain;
            switch (failure.code)
            {
            case EAssetLoadError::STORAGE_FAILURE:
                return ScriptAssetReadOutcome::failure(
                    Domain::STORAGE, static_cast<std::uint32_t>(failure.storage_error)
                );
            case EAssetLoadError::DECODE_FAILURE:
                return ScriptAssetReadOutcome::failure(Domain::DECODE, static_cast<std::uint32_t>(failure.decode.code));
            case EAssetLoadError::SUBMIT_FAILURE:
                return ScriptAssetReadOutcome::failure(
                    Domain::SUBMIT, static_cast<std::uint32_t>(failure.submit_error)
                );
            default:
                return ScriptAssetReadOutcome::failure(
                    Domain::EXECUTION, static_cast<std::uint32_t>(failure.execution_error)
                );
            }
        }
    }

    struct ScriptAssetScope::State final
    {
        struct Pending final
        {
            Completion completion;
            ScriptAssetReadOutcome outcome;
        };
        struct Record final
        {
            ScriptAssetHandle handle;
            lux::asset::AssetId id;
            lux::process::TaskId task;
            std::size_t reserved{};
            lux::asset::AssetBlob image;
            std::shared_ptr<const HeldAsset> asset;
            std::optional<Pending> pending;
            bool ready{};
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

        [[nodiscard]] bool onOwner() const noexcept { return access->owner == std::this_thread::get_id(); }
        [[nodiscard]] Record* find(ScriptAssetHandle handle) noexcept
        {
            return handle.domain == domain ? records.find(handle.slot) : nullptr;
        }
        void remove(ScriptAssetHandle handle) noexcept
        {
            auto* record = find(handle);
            if (record == nullptr)
                return;
            // Erase only empty owners. Their destructors may enter native capabilities again.
            auto image = std::move(record->image);
            auto asset = std::move(record->asset);
            auto pending = std::move(record->pending);
            bytes -= record->reserved;
            records.erase(handle.slot);
        }
        void deliver(ScriptAssetHandle handle) noexcept
        {
            auto* record = find(handle);
            if (record == nullptr || !record->pending)
                return;
            auto pending = std::move(*record->pending);
            record->pending.reset();
            const auto result = pending.completion.success(pending.outcome);
            // Completion callbacks may revoke this scope or release a delivered result.
            record = find(handle);
            if (record == nullptr)
                return;
            if (!result && result.error() == lux::script::EScriptAbilityCompletionError::BACKPRESSURE)
                record->pending.emplace(std::move(pending));
            else if (!result || !pending.outcome.succeeded())
                remove(handle);
        }
        void deliverCompletions() noexcept
        {
            if (delivering || stopping)
                return;
            delivering = true;
            delivery_keys.clear();
            for (const auto& record : records)
                if (record.pending)
                    delivery_keys.push_back(record.handle);
            for (const auto handle : delivery_keys)
                deliver(handle);
            delivery_keys.clear();
            delivering = false;
        }
        void revoke() noexcept
        {
            if (!onOwner())
                std::terminate();
            if (stopping)
                return;
            stopping = true;
            while (!records.empty())
            {
                const auto handle = records.values().front().handle;
                const auto task = records.values().front().task;
                if (task && !access->stopping)
                    (void)access->tasks->execution().requestStop(task);
                remove(handle);
            }
        }
        [[nodiscard]] lux::cxx::expected<Record*, EScriptAssetError> ready(ScriptAssetHandle handle) noexcept
        {
            if (!onOwner())
                return unexpected(EScriptAssetError::WRONG_THREAD);
            auto* record = find(handle);
            if (record == nullptr)
                return unexpected(EScriptAssetError::INVALID_HANDLE);
            if (!record->ready)
                return unexpected(EScriptAssetError::NOT_READY);
            return record;
        }

        std::shared_ptr<AccessState> access;
        const lux::simulation::script::ScriptInstanceId instance;
        lux::cxx::ScopeId<ScriptAssetScopeTag> domain;
        lux::cxx::SlotMap<Record, ScriptAssetResultTag> records;
        std::vector<ScriptAssetHandle> delivery_keys;
        std::optional<AssetLoadFailure> failure;
        std::size_t bytes{};
        bool stopping{};
        bool delivering{};
    };

    struct ScriptAssetAccess::Impl final
    {
        Impl(lux::process::ExecutionRuntime& execution, AssetReadPort read, ScriptAssetLimits limits)
            : tasks(execution), access(std::make_shared<AccessState>(&tasks, std::move(read), limits))
        {
            scopes.reserve(limits.scopes);
            delivery_scopes.reserve(limits.scopes);
        }
        ~Impl() noexcept
        {
            if (access->owner != std::this_thread::get_id())
                std::terminate();
            access->stopping = true;
            for (const auto& weak : scopes)
                if (auto state = weak.lock())
                    state->revoke();
            tasks.requestStop();
            if (!tasks.join())
                std::terminate();
            access->tasks = nullptr;
        }
        lux::process::TaskScope tasks;
        std::shared_ptr<AccessState> access;
        std::vector<std::weak_ptr<ScriptAssetScope::State>> scopes;
        std::vector<std::weak_ptr<ScriptAssetScope::State>> delivery_scopes;
        bool delivering{};
    };

    ScriptAssetAccess::CreateResult ScriptAssetAccess::create(
        lux::process::ExecutionRuntime& execution,
        AssetReadPort read,
        ScriptAssetLimits limits
    ) noexcept
    {
        const bool has_invalid_capacity = limits.scopes == 0 || limits.results_per_scope == 0 ||
            limits.results_per_scope >= UINT32_MAX;
        const bool has_invalid_limits = limits.decode.max_image_bytes == 0 ||
            limits.decode.max_image_bytes > limits.retained_bytes_per_scope ||
            limits.decode.max_decoded_bytes > SIZE_MAX - limits.decode.max_image_bytes;
        if (!read || has_invalid_capacity || has_invalid_limits)
            return unexpected(EScriptAssetError::INVALID_INPUT);
        return std::unique_ptr<ScriptAssetAccess>{new ScriptAssetAccess{
            std::make_unique<Impl>(execution, std::move(read), limits)
        }};
    }

    ScriptAssetAccess::ScriptAssetAccess(std::unique_ptr<Impl> impl) noexcept : impl_(std::move(impl)) {}
    ScriptAssetAccess::~ScriptAssetAccess() noexcept = default;

    void ScriptAssetAccess::deliverCompletions() noexcept
    {
        if (impl_->access->owner != std::this_thread::get_id())
            std::terminate();
        if (impl_->delivering || impl_->access->stopping)
            return;
        impl_->delivering = true;
        impl_->delivery_scopes = impl_->scopes;
        for (const auto& weak : impl_->delivery_scopes)
            if (auto state = weak.lock())
                state->deliverCompletions();
        impl_->delivery_scopes.clear();
        impl_->delivering = false;
    }

    lux::cxx::expected<std::shared_ptr<ScriptAssetScope>, EScriptAssetError>
    ScriptAssetAccess::prepare(lux::simulation::script::ScriptInstanceId instance) noexcept
    {
        auto& access = *impl_->access;
        if (access.owner != std::this_thread::get_id())
            return unexpected(EScriptAssetError::WRONG_THREAD);
        if (access.stopping)
            return unexpected(EScriptAssetError::STOPPING);
        if (!instance.valid())
            return unexpected(EScriptAssetError::INVALID_INPUT);
        std::erase_if(impl_->scopes, [](const auto& weak) noexcept { return weak.expired(); });
        if (impl_->scopes.size() >= access.limits.scopes)
            return unexpected(EScriptAssetError::CAPACITY_EXCEEDED);
        auto state = std::make_shared<ScriptAssetScope::State>(impl_->access, instance);
        impl_->scopes.push_back(state);
        return std::shared_ptr<ScriptAssetScope>{new ScriptAssetScope{std::move(state)}};
    }

    lux::simulation::script::ScriptApiInstanceResult ScriptAssetAccess::prepareInstance(
        void* context,
        lux::simulation::script::ScriptInstanceId instance
    ) noexcept
    {
        using Error = lux::simulation::script::EScriptApiPrepareError;
        if (context == nullptr)
            return unexpected(Error::INVALID_INSTANCE);
        auto scope = static_cast<ScriptAssetAccess*>(context)->prepare(instance);
        if (!scope)
        {
            if (scope.error() == EScriptAssetError::CAPACITY_EXCEEDED)
                return unexpected(Error::CAPACITY_EXCEEDED);
            if (scope.error() == EScriptAssetError::STOPPING)
                return unexpected(Error::STOPPING);
            return unexpected(Error::INVALID_INSTANCE);
        }
        auto* target = scope->get();
        return lux::simulation::script::ScriptApiInstanceBinding::create(
            std::move(*scope), target, [](void* scope) noexcept { static_cast<ScriptAssetScope*>(scope)->revoke(); }
        );
    }

    ScriptAssetScope::~ScriptAssetScope() noexcept { revoke(); }
    void ScriptAssetScope::revoke() noexcept { state_->revoke(); }

    lux::cxx::expected<ScriptAssetScope::Reservation, EScriptAssetError>
    ScriptAssetScope::reserve(lux::asset::AssetId id, bool typed, bool completion_active) noexcept
    {
        auto& state = *state_;
        if (!state.onOwner())
            return unexpected(EScriptAssetError::WRONG_THREAD);
        if (state.stopping || state.access->stopping)
            return unexpected(EScriptAssetError::STOPPING);
        const bool is_invalid_input = id.isNull() || !completion_active;
        if (is_invalid_input)
            return unexpected(EScriptAssetError::INVALID_INPUT);
        const auto& limits = state.access->limits;
        const auto reservation = limits.decode.max_image_bytes + (typed ? limits.decode.max_decoded_bytes : 0);
        const bool is_full = state.records.size() >= limits.results_per_scope ||
            reservation > limits.retained_bytes_per_scope - state.bytes;
        if (is_full)
            return unexpected(EScriptAssetError::CAPACITY_EXCEEDED);
        const auto key = state.records.emplace();
        const ScriptAssetHandle handle{state.domain, key};
        auto* record = state.records.find(key);
        record->handle = handle;
        record->id = id;
        record->reserved = reservation;
        state.bytes += reservation;
        return Reservation{
            state_, handle, *state.access->tasks, state.access->read,
            state.access->tasks->execution().cpu(), limits.decode
        };
    }

    ScriptAssetScope::StartResult ScriptAssetScope::finishStart(
        const std::shared_ptr<State>& state,
        ScriptAssetHandle handle,
        lux::cxx::expected<lux::process::TaskId, EExecutionError> submitted
    ) noexcept
    {
        if (!submitted)
        {
            state->failure = AssetLoadFailure{
                .code = EAssetLoadError::EXECUTION_FAILURE, .execution_error = submitted.error()
            };
            state->remove(handle);
            return unexpected(EScriptAssetError::EXECUTION_REJECTED);
        }
        if (auto* record = state->find(handle))
            record->task = *submitted;
        return {};
    }

    ScriptAssetScope::StartResult ScriptAssetScope::readAsset(lux::asset::AssetId id, Completion completion) noexcept
    {
        auto reserved = reserve(id, false, completion.active());
        if (!reserved)
            return unexpected(reserved.error());
        const auto handle = reserved->handle;
        auto state = reserved->state;
        using Result = lux::cxx::expected<lux::asset::AssetBlob, AssetLoadFailure>;
        auto submitted = reserved->tasks.submit(
            {"Read script asset image", "script"},
            [&reserved, id](lux::process::TaskReporter) noexcept {
                auto sender = stdexec::continues_on(lux::process::portSender(
                    reserved->read, ReadAssetImage{id, reserved->limits.max_image_bytes}
                ), reserved->cpu);
                auto values = stdexec::then(std::move(sender), [](lux::asset::AssetBlob image) noexcept -> Result {
                    return std::move(image);
                });
                return stdexec::upon_error(std::move(values), [](auto error) noexcept -> Result {
                    AssetLoadFailure failure;
                    if constexpr (std::is_same_v<decltype(error), EExecutionError>)
                    {
                        failure.code = EAssetLoadError::EXECUTION_FAILURE;
                        failure.execution_error = error;
                    }
                    else if (error.isRuntime())
                    {
                        failure.code = EAssetLoadError::SUBMIT_FAILURE;
                        failure.submit_error = error.runtimeError();
                    }
                    else
                    {
                        failure.code = EAssetLoadError::STORAGE_FAILURE;
                        failure.storage_error = error.domainError();
                    }
                    return unexpected(failure);
                });
            },
            [state, handle, completion](
                lux::process::TTaskResult<lux::asset::AssetBlob, AssetLoadFailure>&& result
            ) noexcept {
                if (!result)
                {
                    completeFailure(state, handle, completion, std::move(result.error()));
                    return;
                }
                auto* record = state->find(handle);
                if (record == nullptr)
                    return;
                record->image = std::move(*result);
                record->ready = true;
                record->pending.emplace(completion, ScriptAssetReadOutcome::success(handle));
                state->deliver(handle);
            }
        );
        return finishStart(state, handle, std::move(submitted));
    }

    void ScriptAssetScope::completeFailure(
        const std::shared_ptr<State>& state,
        ScriptAssetHandle handle,
        Completion completion,
        lux::process::TTaskError<LoadFailure> failure
    ) noexcept
    {
        if (state->find(handle) == nullptr)
            return;
        auto outcome = ScriptAssetReadOutcome::failure(EScriptAssetFailureDomain::CANCELLED, 0);
        if (auto* native = failure.domainFailure())
        {
            state->failure = *native;
            outcome = mapFailure(*native);
        }
        else if (const auto* execution = failure.executionFailure())
        {
            state->failure = AssetLoadFailure{
                .code = EAssetLoadError::EXECUTION_FAILURE, .execution_error = *execution
            };
            outcome = mapFailure(*state->failure);
        }
        state->find(handle)->pending.emplace(completion, outcome);
        state->deliver(handle);
    }

    void ScriptAssetScope::completeTyped(
        const std::shared_ptr<State>& state,
        ScriptAssetHandle handle,
        Completion completion,
        std::shared_ptr<const HeldAsset> asset
    ) noexcept
    {
        auto* record = state->find(handle);
        if (record == nullptr)
            return;
        record->asset = std::move(asset);
        record->ready = true;
        record->pending.emplace(completion, ScriptAssetReadOutcome::success(handle));
        state->deliver(handle);
    }

    lux::cxx::expected<ScriptAssetDescription, EScriptAssetError>
    ScriptAssetScope::describeAsset(ScriptAssetHandle handle) const noexcept
    {
        auto record = state_->ready(handle);
        if (!record)
            return unexpected(record.error());
        return ScriptAssetDescription{
            (*record)->id, (*record)->asset ? (*record)->asset->asset->type() : lux::asset::AssetTypeId{},
            (*record)->image.bytes.size(), static_cast<bool>((*record)->asset), !(*record)->asset
        };
    }

    lux::cxx::expected<AssetByteChunk, EScriptAssetError> ScriptAssetScope::copyAssetBytes(
        ScriptAssetHandle handle, std::uint64_t offset, std::uint32_t count
    ) const noexcept
    {
        auto record = state_->ready(handle);
        if (!record)
            return unexpected(record.error());
        if ((*record)->asset)
            return unexpected(EScriptAssetError::TYPE_MISMATCH);
        const auto& bytes = (*record)->image.bytes;
        const bool is_invalid_start = offset > bytes.size();
        const bool is_invalid_range = is_invalid_start || count > AssetByteChunk::Capacity ||
            count > bytes.size() - static_cast<std::size_t>(offset);
        if (is_invalid_range)
            return unexpected(EScriptAssetError::INVALID_RANGE);
        AssetByteChunk result;
        result.size = count;
        if (count != 0)
            std::copy_n(bytes.data() + offset, count, result.bytes.data());
        return result;
    }

    ScriptAssetScope::StartResult ScriptAssetScope::releaseAsset(ScriptAssetHandle handle) noexcept
    {
        auto record = state_->ready(handle);
        if (!record)
            return unexpected(record.error());
        state_->remove(handle);
        return {};
    }

    lux::cxx::expected<std::shared_ptr<const ScriptAssetScope::HeldAsset>, EScriptAssetError>
    ScriptAssetScope::borrowTyped(ScriptAssetHandle handle) const noexcept
    {
        auto record = state_->ready(handle);
        if (!record)
            return unexpected(record.error());
        if (!(*record)->asset)
            return unexpected(EScriptAssetError::TYPE_MISMATCH);
        return (*record)->asset;
    }

    std::size_t ScriptAssetScope::retainedResults() const noexcept { return state_->records.size(); }
    std::size_t ScriptAssetScope::reservedBytes() const noexcept { return state_->bytes; }
    std::optional<AssetLoadFailure> ScriptAssetScope::lastFailure() const noexcept { return state_->failure; }
}
