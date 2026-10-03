#include <lux/engine/scene/scripting/SkeletonAbility.hpp>
#include <lux/engine/scene/scripting/ScriptAssetAccess.hpp>
#include <lux/engine/resource/asset/animation/SkeletonAsset.hpp>
#include <SkeletonAbility.ability.generated.hpp>
#include <limits>

namespace lux::scene::script
{
    namespace
    {
        using Traits = lux::script::TScriptAbilityTraits<SkeletonAbility>;
        std::int32_t failure(EScriptAssetError error) noexcept { return -2 - static_cast<std::int32_t>(error); }
        const Traits::Dispatch Dispatch{
            [](void* context, lux::asset::AssetId id, ScriptAssetScope::Completion completion) noexcept
                -> lux::script::ScriptAbilityStartResult {
                auto result = static_cast<ScriptAssetScope*>(context)->readTyped<lux::asset::SkeletonAsset>(
                    id, std::move(completion)
                );
                if (!result)
                    return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                        static_cast<std::int32_t>(result.error())
                    });
                return {};
            },
            [](void* context, ScriptAssetHandle handle) noexcept {
                std::int32_t count = failure(EScriptAssetError::INVALID_RANGE);
                auto result = static_cast<ScriptAssetScope*>(context)->withAsset<lux::asset::SkeletonAsset>(
                    handle, [&count](const auto& asset) noexcept {
                        const auto size = asset.data().bones.size();
                        if (size <= static_cast<std::size_t>(INT32_MAX)) count = static_cast<std::int32_t>(size);
                    }
                );
                return result ? count : failure(result.error());
            },
            [](void* context, ScriptAssetHandle handle, std::uint32_t index) noexcept {
                std::int32_t parent = failure(EScriptAssetError::INVALID_RANGE);
                auto result = static_cast<ScriptAssetScope*>(context)->withAsset<lux::asset::SkeletonAsset>(
                    handle, [&parent, index](const auto& asset) noexcept {
                        if (index < asset.data().bones.size()) parent = asset.data().bones[index].parent_index;
                    }
                );
                return result ? parent : failure(result.error());
            }
        };
    }
    lux::simulation::script::ScriptApiCapabilityPublication publishSkeletonAbility(ScriptAssetAccess& access) noexcept
    {
        return lux::simulation::script::publishScriptAbility(
            lux::script::ScriptAbilityBinding{&Traits::Description, &access, &Dispatch, Traits::ErasedMethods},
            &ScriptAssetAccess::prepareInstance
        );
    }
}
