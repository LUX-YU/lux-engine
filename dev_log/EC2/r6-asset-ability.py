from pathlib import Path
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec2')
p=s/'modules/function/script/lua/include/lux/engine/function/script/lua/LuaValue.hpp'
t=p.read_text(); pos=t.index('    template <class T, class Policy>\n    LuaValueResult<T> LuaValueReader::field')
t=t[:pos]+'''    // Opt-in representation for bounded trivial values. Lua owns a copy, never a native owner.
    // Semantic identity and codec representation are both checked before reconstructing T.
    template <class T> requires(lux::semantic::TypeDeclared<T> && std::is_trivially_copyable_v<T>)
    struct TLuaOpaqueValue
    {
        inline static constexpr std::string_view name = "lux.lua.opaque-value";
        inline static constexpr std::uint32_t version = 1;
        inline static constexpr std::size_t storage = sizeof(T);
        inline static constexpr std::size_t depth = 0;
        static bool prepare(lua_State* state) noexcept { return detail::LuaValueAccess::prepareOpaque(state); }
        template <class Policy> static LuaValueResult<T> read(LuaValueReader& input) noexcept
        {
            return input.template opaque<T>(
                lux::semantic::typeId(lux::semantic::TTypeTraits<T>::CanonicalName),
                TLuaValueCodec<T, Policy>::representation()
            );
        }
        template <class Policy> static LuaValueResult<void> push(LuaValueWriter& output, const T& value) noexcept
        {
            return output.opaque(value,
                lux::semantic::typeId(lux::semantic::TTypeTraits<T>::CanonicalName),
                TLuaValueCodec<T, Policy>::representation()
            );
        }
    };

'''+t[pos:];p.write_text(t)
p=s/'modules/function/script/lua/src/LuaValue.cpp';t=p.read_text().replace('            // Lua55 C errors', '            static_assert(std::is_trivially_destructible_v<Operation>);\n            // Lua55 C errors');t=t.replace('            bool validOpaque(lux::semantic::TypeId type, std::uint64_t representation,\n                std::size_t alignment, std::size_t size) noexcept', '''            bool validOpaque(
                lux::semantic::TypeId type,
                std::uint64_t representation,
                std::size_t alignment,
                std::size_t size
            ) noexcept''');p.write_text(t)
# No second result owner. These query values report the native query status without exceptional control flow.
p=s/'engine/scene/scripting/assets/include/lux/engine/scene/scripting/ScriptAssetResult.hpp';t=p.read_text();i=t.index('\n}\n\nnamespace lux::semantic');t=t[:i]+'''
    struct ScriptAssetInspection final
    {
        ScriptAssetDescription value;
        std::uint32_t error{};
    };

    struct ScriptAssetBytes final
    {
        AssetByteChunk value;
        std::uint32_t error{};
    };
'''+t[i:]
for typ,name in [('lux::asset::AssetId','lux.resource.AssetId.v1'),('lux::scene::script::ScriptAssetHandle','lux.scene.script.AssetHandle.v1'),('lux::scene::script::ScriptAssetInspection','lux.scene.script.AssetInspection.v1'),('lux::scene::script::ScriptAssetBytes','lux.scene.script.AssetBytes.v1')]:
 t=t[:-2]+f'''    template <> struct TTypeTraits<{typ}> final
    {{
        inline static constexpr std::string_view CanonicalName = "{name}";
        inline static constexpr std::uint8_t AbiKind = static_cast<std::uint8_t>(EAbiKind::STRUCT_REF);
        inline static constexpr std::uint32_t Size = sizeof({typ});
        inline static constexpr std::uint32_t Alignment = alignof({typ});
    }};
}}
'''
p.write_text(t)
methods=[
 ('QUERY','lux::asset::AssetId','assetId','std::uint32_t first, std::uint32_t second, std::uint32_t third, std::uint32_t fourth'),
 ('QUERY','std::uint32_t','assetWord','lux::asset::AssetId id, std::uint32_t word'),
 ('ASYNC','ScriptAssetReadOutcome','readAsset','lux::asset::AssetId id'),
 ('QUERY','bool','succeeded','ScriptAssetReadOutcome result'),
 ('QUERY','ScriptAssetHandle','handle','ScriptAssetReadOutcome result'),
 ('QUERY','std::uint32_t','errorDomain','ScriptAssetReadOutcome result'),
 ('QUERY','std::uint32_t','errorCode','ScriptAssetReadOutcome result'),
 ('QUERY','ScriptAssetInspection','describeAsset','ScriptAssetHandle handle'),
 ('QUERY','std::uint32_t','inspectionError','ScriptAssetInspection result'),
 ('QUERY','lux::asset::AssetId','inspectedId','ScriptAssetInspection result'),
 ('QUERY','std::uint32_t','imageSizeWord','ScriptAssetInspection result, std::uint32_t word'),
 ('QUERY','ScriptAssetBytes','copyAssetBytes','ScriptAssetHandle handle, std::uint32_t offset_high, std::uint32_t offset_low, std::uint32_t count'),
 ('QUERY','std::uint32_t','bytesError','ScriptAssetBytes result'),
 ('QUERY','std::uint32_t','bytesCount','ScriptAssetBytes result'),
 ('QUERY','std::int32_t','byteAt','ScriptAssetBytes result, std::uint32_t index'),
 ('COMMAND','std::int32_t','releaseAsset','ScriptAssetHandle handle')]
h='''#pragma once

#include <lux/engine/function/script/ScriptAbilityAnnotations.hpp>
#include <lux/engine/scene/scripting/ScriptAssetResult.hpp>

namespace lux::scene::script
{
    struct LUX_SCRIPT_ABILITY(
        id = lux.scene.assets, name = Assets, display = Assets, version = 1, receiver = provider_instance
    ) AssetAbility
    {
'''
for kind,ret,name,args in methods:
 life='awaitable' if kind=='ASYNC' else 'owned_value'
 h+=f'        LUX_SCRIPT_{kind}(id = lux.scene.assets.{name}, display = {name}, result_lifetime = {life})\n'
 annotated=', '.join('LUX_SCRIPT_PARAM(lifetime = owned_value) '+a for a in args.split(', '))
 if len(f'        {ret} {name}({annotated}) noexcept;')<=120:h+=f'        {ret} {name}({annotated}) noexcept;\n\n'
 else:h+=f'        {ret} {name}(\n'+',\n'.join('            '+a for a in annotated.split(', '))+'\n        ) noexcept;\n\n'
h+='    };\n}\n'
p=s/'engine/scene/scripting/assets/include/lux/engine/scene/scripting/AssetAbility.hpp';p.write_text(h)
p=s/'engine/scene/scripting/assets/include/lux/engine/scene/scripting/ScriptAssetAccess.hpp';t=p.read_text();i=t.index('    struct ScriptAssetLimits');t=t[:i]+'''    // Binding preparation substitutes the original provider with the instance-owned scope.
    // The generated descriptor/erased adapters are shared by native and Lua consumers.
    [[nodiscard]] LUX_SCENE_SCRIPT_ASSETS_PUBLIC lux::simulation::script::ScriptApiCapabilityPublication
    publishAssetAbility(ScriptAssetAccess& access) noexcept;

'''.replace('    // Binding','    class ScriptAssetAccess;\n\n    // Binding')+t[i:];p.write_text(t)
cpp='''#include <lux/engine/scene/scripting/ScriptAssetAccess.hpp>
#include <AssetAbility.ability.generated.hpp>

namespace lux::scene::script
{
    namespace
    {
        using Traits = lux::script::TScriptAbilityTraits<AssetAbility>;
        using Completion = ScriptAssetScope::Completion;
        ScriptAssetScope& scope(void* context) noexcept { return *static_cast<ScriptAssetScope*>(context); }
        const Traits::Dispatch Dispatch{
            [](void*, std::uint32_t a, std::uint32_t b, std::uint32_t c, std::uint32_t d) noexcept {
                const std::array words{a, b, c, d};
                std::array<std::uint8_t, 16> bytes{};
                for (std::size_t i{}; i < bytes.size(); ++i)
                    bytes[i] = static_cast<std::uint8_t>(words[i / 4] >> (24 - (i % 4) * 8));
                return lux::asset::AssetId{bytes};
            },
            [](void*, lux::asset::AssetId id, std::uint32_t word) noexcept {
                if (word >= 4U) return std::uint32_t{};
                const auto bytes = id.bytes();
                std::uint32_t result{};
                for (std::size_t i{}; i < 4; ++i)
                    result = (result << 8U) | std::to_integer<std::uint8_t>(bytes[word * 4 + i]);
                return result;
            },
            [](void* context, lux::asset::AssetId id, Completion completion) noexcept -> lux::script::ScriptAbilityStartResult {
                auto result = scope(context).readAsset(id, std::move(completion));
                if (!result)
                    return lux::cxx::unexpected(lux::script::ScriptAbilityOperationError{
                        static_cast<std::int32_t>(result.error())
                    });
                return {};
            },
            [](void*, ScriptAssetReadOutcome value) noexcept { return value.succeeded(); },
            [](void*, ScriptAssetReadOutcome value) noexcept { return value.handle(); },
            [](void*, ScriptAssetReadOutcome value) noexcept { return static_cast<std::uint32_t>(value.errorDomain()); },
            [](void*, ScriptAssetReadOutcome value) noexcept { return value.errorCode(); },
            [](void* context, ScriptAssetHandle handle) noexcept {
                auto result = scope(context).describeAsset(handle);
                if (!result) return ScriptAssetInspection{{}, static_cast<std::uint32_t>(result.error())};
                return ScriptAssetInspection{*result, 0};
            },
            [](void*, ScriptAssetInspection value) noexcept { return value.error; },
            [](void*, ScriptAssetInspection value) noexcept { return value.value.id; },
            [](void*, ScriptAssetInspection value, std::uint32_t word) noexcept {
                if (word >= 2U) return std::uint32_t{};
                return static_cast<std::uint32_t>(value.value.image_bytes >> (word == 0U ? 32U : 0U));
            },
            [](void* context, ScriptAssetHandle handle, std::uint32_t high, std::uint32_t low, std::uint32_t count) noexcept {
                auto result = scope(context).copyAssetBytes(handle, (std::uint64_t{high} << 32U) | low, count);
                if (!result) return ScriptAssetBytes{{}, static_cast<std::uint32_t>(result.error())};
                return ScriptAssetBytes{*result, 0};
            },
            [](void*, ScriptAssetBytes value) noexcept { return value.error; },
            [](void*, ScriptAssetBytes value) noexcept { return value.value.size; },
            [](void*, ScriptAssetBytes value, std::uint32_t index) noexcept -> std::int32_t {
                if (index >= value.value.size) return -1;
                return std::to_integer<std::uint8_t>(value.value.bytes[index]);
            },
            [](void* context, ScriptAssetHandle handle) noexcept -> std::int32_t {
                auto result = scope(context).releaseAsset(handle);
                return result ? 0 : static_cast<std::int32_t>(result.error());
            }
        };
    }
    lux::simulation::script::ScriptApiCapabilityPublication publishAssetAbility(ScriptAssetAccess& access) noexcept
    {
        return lux::simulation::script::publishScriptAbility(
            lux::script::ScriptAbilityBinding{&Traits::Description, &access, &Dispatch, Traits::ErasedMethods},
            &ScriptAssetAccess::prepareInstance
        );
    }
}
'''
(s/'engine/scene/scripting/assets/src/AssetAbility.cpp').write_text(cpp)
# Installation keeps Lua optional; values and generated Lua projection are header-only consumers of script_lua.
p=s/'engine/scene/scripting/assets/include/lux/engine/scene/scripting/AssetAbilityLua.hpp'
p.write_text('''#pragma once

#include <lux/engine/function/script/lua/LuaValue.hpp>
#include <lux/engine/scene/scripting/ScriptAssetResult.hpp>

namespace lux::script::lua
{
'''+''.join(f'    template <> struct TLuaValueOverride<{typ}, LuaValuePolicy> : TLuaOpaqueValue<{typ}> {{}};\n' for typ in ['lux::asset::AssetId','lux::scene::script::ScriptAssetHandle','lux::scene::script::ScriptAssetReadOutcome','lux::scene::script::ScriptAssetInspection','lux::scene::script::ScriptAssetBytes'])+'''}

#include <lux/engine/scene/scripting/AssetAbility.ability.lua.generated.hpp>
''')
p=s/'engine/scene/scripting/assets/CMakeLists.txt';t=p.read_text().replace('SOURCE_FILES src/ScriptAssetAccess.cpp','SOURCE_FILES src/ScriptAssetAccess.cpp src/AssetAbility.cpp');i=t.index('lux_engine_install_components(');t=t[:i]+'''include_component_cmake_scripts(script_core)
lux_script_abilities(
    TARGET scene_script_assets
    SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/include/lux/engine/scene/scripting/AssetAbility.hpp
    LOGICAL_PATHS lux/engine/scene/scripting/AssetAbility.hpp
)
get_target_property(_asset_ability_generated_dir scene_script_assets LUX_SCRIPT_ABILITY_GENERATED_DIR)
install(FILES
    ${_asset_ability_generated_dir}/AssetAbility.ability.generated.hpp
    ${_asset_ability_generated_dir}/AssetAbility.ability.lua.generated.hpp
    DESTINATION include/lux/engine/scene/scripting COMPONENT lux_sdk
)
'''+t[i:];p.write_text(t)
