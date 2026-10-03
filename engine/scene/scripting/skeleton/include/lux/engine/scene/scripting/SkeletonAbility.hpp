#pragma once

#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>
#include <lux/engine/scene/scripting/ScriptAssetResult.hpp>
namespace lux::simulation::script { struct ScriptApiCapabilityPublication; }

namespace lux::scene::script
{
    class ScriptAssetAccess;
    struct LUX_SCRIPT_ABILITY(
        id = lux.scene.skeletons, name = Skeletons, display = Skeletons, version = 1, receiver = provider_instance
    ) SkeletonAbility
    {
        LUX_SCRIPT_ASYNC(id = lux.scene.skeletons.read, display = Read, result_lifetime = awaitable)
        ScriptAssetReadOutcome read(LUX_SCRIPT_PARAM(lifetime = owned_value) lux::asset::AssetId id) noexcept;

        LUX_SCRIPT_QUERY(id = lux.scene.skeletons.boneCount, display = BoneCount, result_lifetime = owned_value)
        std::int32_t boneCount(LUX_SCRIPT_PARAM(lifetime = owned_value) ScriptAssetHandle handle) noexcept;

        LUX_SCRIPT_QUERY(id = lux.scene.skeletons.parentIndex, display = ParentIndex, result_lifetime = owned_value)
        std::int32_t parentIndex(
            LUX_SCRIPT_PARAM(lifetime = owned_value) ScriptAssetHandle handle,
            LUX_SCRIPT_PARAM(lifetime = owned_value) std::uint32_t index
        ) noexcept;
    };

    // Read-only queries use nonnegative values (plus -1 for a root parent).
    // Failure is -2 - EScriptAssetError, preserving both domain status and root sentinel.
    [[nodiscard]] lux::simulation::script::ScriptApiCapabilityPublication
    publishSkeletonAbility(ScriptAssetAccess& access) noexcept;
}
