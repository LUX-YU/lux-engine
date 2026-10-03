#pragma once

#include <lux/engine/function/script/lua/ScriptAbilityLua.hpp>
#include <lux/engine/scene/scripting/ScriptAssetResult.hpp>

namespace lux::script::lua
{
    template <> struct TLuaValueOverride<lux::asset::AssetId, LuaValuePolicy> : TLuaOpaqueValue<lux::asset::AssetId> {};
    template <> struct TLuaValueOverride<lux::scene::script::ScriptAssetHandle, LuaValuePolicy> : TLuaOpaqueValue<lux::scene::script::ScriptAssetHandle> {};
    template <> struct TLuaValueOverride<lux::scene::script::ScriptAssetReadOutcome, LuaValuePolicy> : TLuaOpaqueValue<lux::scene::script::ScriptAssetReadOutcome> {};
    template <> struct TLuaValueOverride<lux::scene::script::ScriptAssetInspection, LuaValuePolicy> : TLuaOpaqueValue<lux::scene::script::ScriptAssetInspection> {};
    template <> struct TLuaValueOverride<lux::scene::script::ScriptAssetBytes, LuaValuePolicy> : TLuaOpaqueValue<lux::scene::script::ScriptAssetBytes> {};
}

namespace lux::scene::script
{
    [[nodiscard]] lux::script::lua::ScriptAbilityLuaContribution assetAbilityLua() noexcept;
    [[nodiscard]] lux::script::lua::ScriptAbilityLuaContribution skeletonAbilityLua() noexcept;
}
