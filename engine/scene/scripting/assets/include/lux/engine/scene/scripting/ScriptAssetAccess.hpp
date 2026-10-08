#pragma once

#include <lux/engine/function/script/ScriptAbilityAsync.hpp>
#include <lux/engine/process/TaskScope.hpp>
#include <lux/engine/process/asset_loading/AssetLoadSender.hpp>
#include <lux/engine/scene/script_assets/visibility.h>
#include <lux/engine/scene/scripting/ScriptAssetResult.hpp>
#include <lux/engine/simulation/scripting/ScriptApiCapability.hpp>

#include <memory>
#include <optional>

namespace lux::scene::script
{
    class ScriptAssetAccess;

    // Binding preparation substitutes the original provider with the instance-owned scope.
    // The generated descriptor/erased adapters are shared by native and Lua consumers.
    [[nodiscard]] LUX_SCENE_SCRIPT_ASSETS_PUBLIC lux::simulation::script::ScriptApiCapabilityPublication
    publishAssetAbility(ScriptAssetAccess& access) noexcept;

    struct ScriptAssetLimits final
    {
        std::size_t scopes{64};
        std::size_t results_per_scope{16};
        std::size_t retained_bytes_per_scope{64U * 1024U * 1024U};
        lux::asset::AssetDecodeLimits decode{8U * 1024U * 1024U, 32U * 1024U * 1024U, 32};
    };

    // Owner-thread capability. A token is an alias into this scope, never an asset owner.
    // Revoke is nonblocking. Accepted TaskScope work retains only native records and settles normally.
    class LUX_SCENE_SCRIPT_ASSETS_PUBLIC ScriptAssetScope final
    {
    public:
        using Completion = lux::script::TScriptAbilityCompletion<ScriptAssetReadOutcome>;
        using StartResult = lux::cxx::expected<void, EScriptAssetError>;
        ~ScriptAssetScope() noexcept;
        ScriptAssetScope(const ScriptAssetScope&) = delete;
        ScriptAssetScope& operator=(const ScriptAssetScope&) = delete;
        ScriptAssetScope(ScriptAssetScope&&) = delete;
        ScriptAssetScope& operator=(ScriptAssetScope&&) = delete;

        [[nodiscard]] StartResult readAsset(lux::asset::AssetId id, Completion completion) noexcept;

        template <class Asset>
        [[nodiscard]] StartResult readTyped(
            lux::asset::AssetId id,
            Completion completion,
            std::shared_ptr<const void> code = {}
        ) noexcept
        {
            const auto state = state_;
            auto reserved = reserve(state, id, true, completion.active());
            if (!reserved)
            {
                return lux::cxx::unexpected(reserved.error());
            }
            const auto handle = reserved->handle;
            auto submitted = reserved->tasks.submit(
                {"Read script CPU asset", "script", {}, code},
                [&reserved, id](lux::process::TaskReporter reporter) noexcept
                {
                    auto sender = lux::process::asset_loading::loadAsset<Asset>(
                        reserved->read,
                        reserved->cpu,
                        id,
                        reserved->limits,
                        reporter.stopToken()
                    );
                    auto values = stdexec::then(
                        std::move(sender),
                        [](std::shared_ptr<const Asset> value) noexcept
                        { return lux::cxx::expected<std::shared_ptr<const Asset>, LoadFailure>{std::move(value)}; }
                    );
                    return stdexec::upon_error(
                        std::move(values),
                        [](LoadFailure failure) noexcept
                        {
                            return lux::cxx::expected<std::shared_ptr<const Asset>, LoadFailure>{
                                lux::cxx::unexpected(std::move(failure))
                            };
                        }
                    );
                },
                [state, handle, completion, code](
                    lux::process::TTaskResult<std::shared_ptr<const Asset>, LoadFailure>&& result
                ) noexcept
                {
                    if (!result)
                    {
                        completeFailure(state, handle, completion, std::move(result.error()));
                        return;
                    }
                    auto held = holdAsset(std::move(code), std::move(*result));
                    completeTyped(state, handle, completion, std::move(held));
                }
            );
            return finishStart(state, handle, std::move(submitted));
        }

        [[nodiscard]] lux::cxx::expected<ScriptAssetDescription, EScriptAssetError>
        describeAsset(ScriptAssetHandle handle) const noexcept;
        [[nodiscard]] lux::cxx::expected<AssetByteChunk, EScriptAssetError> copyAssetBytes(
            ScriptAssetHandle handle,
            std::uint64_t offset,
            std::uint32_t count
        ) const noexcept;
        [[nodiscard]] StartResult releaseAsset(ScriptAssetHandle handle) noexcept;

        // The callback may inspect this fixed result only for the duration of the call.
        template <class Asset, class Function>
            requires std::is_nothrow_invocable_r_v<void, Function&, const Asset&>
        [[nodiscard]] StartResult withAsset(ScriptAssetHandle handle, Function&& function) const noexcept
        {
            auto held = borrowTyped(handle);
            if (!held)
            {
                return lux::cxx::unexpected(held.error());
            }
            const auto* value = (*held)->asset->template as<Asset>();
            if (value == nullptr)
            {
                return lux::cxx::unexpected(EScriptAssetError::TYPE_MISMATCH);
            }
            std::invoke(function, *value);
            return {};
        }

        void revoke() noexcept;
        [[nodiscard]] std::size_t retainedResults() const noexcept;
        [[nodiscard]] std::size_t reservedBytes() const noexcept;
        [[nodiscard]] std::optional<lux::process::asset_loading::AssetLoadFailure> lastFailure() const noexcept;

    private:
        friend class ScriptAssetAccess;
        using LoadFailure = lux::process::asset_loading::AssetLoadFailure;
        struct State;

        struct HeldAsset final
        {
            HeldAsset(std::shared_ptr<const void> code, std::shared_ptr<const lux::asset::Asset> asset) noexcept
                : code(std::move(code)), asset(std::move(asset))
            {
            }

            // Declaration order is the destruction contract: asset deleter before its code owner.
            const std::shared_ptr<const void> code;
            const std::shared_ptr<const lux::asset::Asset> asset;
        };

        struct Reservation final
        {
            ScriptAssetHandle handle;
            lux::process::TaskScope& tasks;
            lux::process::asset_loading::AssetReadPort read;
            lux::process::CpuScheduler cpu;
            lux::asset::AssetDecodeLimits limits;
        };

        // The control block must live in this native library. Instantiating make_shared in a
        // plugin can unload that plugin before its control-block destruction returns.
        [[nodiscard]] static std::shared_ptr<const HeldAsset> holdAsset(
            std::shared_ptr<const void> code,
            std::shared_ptr<const lux::asset::Asset> asset
        ) noexcept;

        explicit ScriptAssetScope(std::shared_ptr<State> state) noexcept : state_(std::move(state)) {}

        [[nodiscard]] static lux::cxx::expected<Reservation, EScriptAssetError> reserve(
            const std::shared_ptr<State>& state,
            lux::asset::AssetId id,
            bool typed,
            bool completion_active
        ) noexcept;
        [[nodiscard]] static StartResult finishStart(
            const std::shared_ptr<State>& state,
            ScriptAssetHandle handle,
            lux::cxx::expected<lux::process::TaskId, lux::process::EExecutionError> submitted
        ) noexcept;
        static void completeFailure(
            const std::shared_ptr<State>& state,
            ScriptAssetHandle handle,
            Completion completion,
            lux::process::TTaskError<LoadFailure> failure
        ) noexcept;
        static void completeTyped(
            const std::shared_ptr<State>& state,
            ScriptAssetHandle handle,
            Completion completion,
            std::shared_ptr<const HeldAsset> asset
        ) noexcept;
        [[nodiscard]] lux::cxx::expected<std::shared_ptr<const HeldAsset>, EScriptAssetError>
        borrowTyped(ScriptAssetHandle handle) const noexcept;
        std::shared_ptr<State> state_;
    };

    class LUX_SCENE_SCRIPT_ASSETS_PUBLIC ScriptAssetAccess final
    {
    public:
        using CreateResult = lux::cxx::expected<std::unique_ptr<ScriptAssetAccess>, EScriptAssetError>;
        [[nodiscard]] static CreateResult create(
            lux::process::ExecutionRuntime& execution,
            lux::process::asset_loading::AssetReadPort read,
            ScriptAssetLimits limits
        ) noexcept;
        // Closes admission and revokes instance results without waiting or collecting Runtime completions.
        ~ScriptAssetAccess() noexcept;
        ScriptAssetAccess(const ScriptAssetAccess&) = delete;
        ScriptAssetAccess& operator=(const ScriptAssetAccess&) = delete;
        ScriptAssetAccess(ScriptAssetAccess&&) = delete;
        ScriptAssetAccess& operator=(ScriptAssetAccess&&) = delete;

        [[nodiscard]] lux::cxx::expected<std::shared_ptr<ScriptAssetScope>, EScriptAssetError>
        prepare(lux::simulation::script::ScriptInstanceId instance) noexcept;
        [[nodiscard]] static lux::simulation::script::ScriptApiInstanceResult prepareInstance(
            void* context,
            lux::simulation::script::ScriptInstanceId instance
        ) noexcept;
        // Retry only already accepted native completions at the host's maintenance boundary.
        // A full ScriptSystem ingress retains the original result/slot; no IO or decode is restarted.
        void deliverCompletions() noexcept;

    private:
        struct Impl;
        explicit ScriptAssetAccess(std::shared_ptr<Impl> impl) noexcept;
        // Synchronous delivery protects its working storage across callbacks. Tasks never retain Impl.
        std::shared_ptr<Impl> impl_;
    };
} // namespace lux::scene::script
