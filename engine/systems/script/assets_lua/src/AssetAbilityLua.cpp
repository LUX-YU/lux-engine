#include <lux/engine/scene/scripting/AssetAbilityLua.hpp>
#include <AssetAbility.ability.generated.hpp>
#include <AssetAbility.ability.lua.generated.hpp>
#include <SkeletonAbility.ability.generated.hpp>
#include <SkeletonAbility.ability.lua.generated.hpp>

namespace lux::scene::script
{
    lux::script::lua::ScriptAbilityLuaContribution skeletonAbilityLua() noexcept
    {
        return lux::script::lua::makeScriptAbilityLuaContribution<SkeletonAbility>();
    }

    lux::script::lua::ScriptAbilityLuaContribution assetAbilityLua() noexcept
    {
        return lux::script::lua::makeScriptAbilityLuaContribution<AssetAbility>();
    }
}
