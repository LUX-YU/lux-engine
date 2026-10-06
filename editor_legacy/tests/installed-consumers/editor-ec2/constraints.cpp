#include <lux/engine/scene/scripting/ScriptAssetAccess.hpp>
#include <lux/engine/simulation/scripting/ScriptAbilityInvocation.hpp>
#include <lux/engine/simulation/scripting/ScriptLocalAsync.hpp>
#include <string>

using namespace lux::simulation::script;
static_assert(!std::is_copy_constructible_v<lux::scene::script::ScriptAssetAccess>);
static_assert(!std::is_move_constructible_v<lux::scene::script::ScriptAssetAccess>);
static_assert(!std::is_copy_constructible_v<lux::scene::script::ScriptAssetScope>);
static_assert(!std::is_move_constructible_v<lux::scene::script::ScriptAssetScope>);
ScriptStepResult invocation(ScriptStepContext& context) noexcept
{
#ifdef REJECT_NONTRIVIAL
    return invokeScriptAbilityAsync<std::string>(context, [](auto) noexcept -> lux::script::ScriptAbilityStartResult {
        return {};
    });
#else
    return invokeScriptAbilityAsync<lux::scene::script::ScriptAssetReadOutcome>(context,
        [](auto) noexcept -> lux::script::ScriptAbilityStartResult { return {}; });
#endif
}
PreparedLocalAsyncStart authority() noexcept
{
#ifdef REJECT_LOCAL_AUTHORITY
    return PreparedLocalAsyncStart{nullptr, nullptr, 0};
#else
    return {};
#endif
}
