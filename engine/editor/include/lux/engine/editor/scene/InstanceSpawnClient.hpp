#pragma once
/**
 * @file InstanceSpawnClient.hpp
 * @brief Scene-scoped, stdexec-free facade for asynchronous model spawning.
 *
 * The implementation composes AssetLoadService and ResidencyAssembly on
 * AsyncRuntime. This header deliberately exposes only owned values and a
 * one-shot completion seam; game/ECS-facing headers do not include stdexec.
 */

#include <lux/cxx/compile_time/expected.hpp>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/resource/asset/AssetId.hpp>
#include <lux/engine/resource/asset/AssetRef.hpp>
#include <lux/engine/runtime/assets/AssetLoadService.hpp>
#include <lux/engine/ecs/render/ResidencyCallbacks.hpp>
#include <lux/engine/meta/LuxObject.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace lux::asset { class AssetManager; }
namespace lux::exec { class AsyncRuntime; class AsyncScopeCloseSender; }

namespace lux::editor
{
    struct InstanceSubmeshPlan final
    {
        lux::asset::asset_id_t mesh{};
        lux::asset::asset_id_t material{};
        std::string name;
    };

    struct InstanceSpawnPlan final
    {
        lux::asset::asset_id_t model{};
        std::uint32_t model_revision{0};
        std::string root_name;
        std::vector<InstanceSubmeshPlan> submeshes;
        lux::asset::asset_id_t skeleton{};
        lux::asset::asset_id_t animation_clip{};
        std::vector<lux::asset_runtime::AssetLoadResult> dependencies;
        /// Main-thread handoff pins. EditorScene releases them only after the
        /// first ResidencySubsystem tick has acquired its own entity tickets.
        std::vector<lux::asset::AssetRef> residency_pins;
    };

    enum class EInstanceSpawnError : std::uint8_t
    {
        MODEL_NOT_FOUND,
        MODEL_EMPTY,
        ASSET_LOAD_FAILED,
        GPU_RESIDENCY_FAILED,
        SUPERSEDED,
        SCENE_CLOSED,
        STOPPING
    };

    struct InstanceSpawnFailure final
    {
        EInstanceSpawnError code{EInstanceSpawnError::STOPPING};
        lux::asset::asset_id_t asset{};
    };

    struct InstanceSpawnResult final
    {
        lux::meta::entity_id root{lux::meta::null_entity};
        lux::asset::asset_id_t model{};
        std::uint32_t model_revision{0};
    };

    using InstanceSpawnOutcome = lux::cxx::expected<
        InstanceSpawnResult,
        InstanceSpawnFailure>;

    class InstanceSpawnClient final
    {
    public:
        using Commit = lux::cxx::move_only_function<
            lux::meta::entity_id(InstanceSpawnPlan&&)>;
        using Completion = lux::cxx::move_only_function<void(
            InstanceSpawnOutcome)>;

        InstanceSpawnClient(
            lux::exec::AsyncRuntime& runtime,
            lux::asset_runtime::AssetClient assets,
            lux::asset::AssetManager& manager,
            lux::ecs::ResidencyCallbacks residency,
            Commit commit);
        ~InstanceSpawnClient();

        InstanceSpawnClient(const InstanceSpawnClient&) = delete;
        InstanceSpawnClient& operator=(const InstanceSpawnClient&) = delete;
        InstanceSpawnClient(InstanceSpawnClient&&) = delete;
        InstanceSpawnClient& operator=(InstanceSpawnClient&&) = delete;

        /// Main-thread submission. A successful return means the request was
        /// admitted to this scene scope; runtime backpressure is still
        /// delivered through the exactly-once structured completion.
        [[nodiscard]] lux::exec::AsyncSubmitResult spawnModel(
            const lux::asset::asset_id_t& model,
            Completion completion);

        /// Main-thread structured close. Stops this scene's requests and
        /// drains their terminal receivers while AsyncRuntime is still alive.
        [[nodiscard]] lux::exec::AsyncScopeCloseSender closeAsync() noexcept;

    private:
        struct State;
        std::shared_ptr<State> state_;
    };
}
