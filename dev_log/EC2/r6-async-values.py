from pathlib import Path
import hashlib,json,shutil
s=Path('E:/SyncForder/CodeRepos/lux-engine-ec2');o=Path('E:/SyncForder/CodeRepos/lux-engine/.internal/editor-redesign/EC2')
p=s/'modules/function/script/lua/include/lux/engine/function/script/lua/ScriptAbilityLua.hpp';t=p.read_text().replace('#include <memory>', '#include <memory>\n#include <cstring>');t=t.replace('    struct LuaValueOperation final','''    using LuaResumeValuePush = bool (*)(lua_State*, std::span<const std::byte>) noexcept;

    namespace detail
    {
        template <class T, class Policy> bool pushValue(lua_State* state, const T& value) noexcept
        {
            const auto top = LuaValueAccess::top(state);
            LuaValueWriter output{state};
            const auto result = TLuaValueCodec<T, Policy>::push(output, value);
            const bool valid = result && LuaValueAccess::top(state) == top + 1;
            if (!valid) LuaValueAccess::restoreScratch(state, top);
            return valid;
        }
    }

    struct LuaValueOperation final''');t=t.replace('        bool (*prepare)(lua_State*) noexcept {};','''        bool (*prepare)(lua_State*) noexcept {};
        // Present only for a bounded trivially-copyable native value. Resume bytes are
        // reconstructed into an aligned T, then passed through the same protected codec.
        LuaResumeValuePush push_resume{};''')
a=t.index('                const auto top = detail::LuaValueAccess::top(state);');b=t.index('            },',a);t=t[:a]+'''                return detail::pushValue<V, Policy>(state, *static_cast<const V*>(value));
'''+t[b:];t=t.replace('            &Codec::prepare\n','''            &Codec::prepare,
            []() consteval -> LuaResumeValuePush {
                if constexpr (std::is_trivially_copyable_v<V> && Codec::can_push && Codec::bounded)
                    return [](lua_State* state, std::span<const std::byte> bytes) noexcept {
                        if (bytes.size() != sizeof(V)) return false;
                        std::array<std::byte, sizeof(V)> owned{};
                        std::memcpy(owned.data(), bytes.data(), owned.size());
                        const auto value = std::bit_cast<V>(owned);
                        return detail::pushValue<V, Policy>(state, value);
                    };
                else
                    return nullptr;
            }()
''');p.write_text(t)
p=s/'engine/domain/simulation/scripting/lua/include/lux/engine/simulation/scripting/lua/LuaScriptAbilityProjection.hpp';t=p.read_text();a=t.index('        // New record/enum async protocols');b=t.index('            return LuaAbilityProjectionAccess::fail',a);t=t[:a]+'''        // Argument values live only until the provider accepts its owned request. They are
        // destroyed before the original C boundary suspends; no Lua stack address is retained.
        if constexpr (!((std::is_trivially_copyable_v<std::remove_cvref_t<Arguments>> &&
                         TLuaValueCodec<std::remove_cvref_t<Arguments>, Policy>::can_read &&
                         TLuaValueCodec<std::remove_cvref_t<Arguments>, Policy>::bounded) && ...))
'''+t[b:];p.write_text(t)
p=s/'engine/domain/simulation/scripting/lua/src/LuaScriptBackend.cpp';t=p.read_text();t=t.replace('''            LuxLuaTypedWorker entry{};
        };

        struct PreparedAbility''','''            LuxLuaTypedWorker entry{};
            lux::script::lua::LuaResumeValuePush push_resume{};
        };

        struct PreparedAbility''',1);t=t.replace('''            PreparedLocalAsyncStart local_async;
        };

        struct PreparedEventSource''','''            PreparedLocalAsyncStart local_async;
            lux::script::lua::LuaResumeValuePush push_resume{};
        };

        struct PreparedEventSource''',1)
t=t.replace('''                         contribution.methods[index].entry}''','''                         contribution.methods[index].entry,
                         contribution.methods[index].results.empty() ? nullptr :
                             contribution.methods[index].results.front().push_resume}''',1)
t=t.replace('''                    capability->local_async.resolve(method->method, capability->context, capability->dispatch)
''','''                    capability->local_async.resolve(method->method, capability->context, capability->dispatch),
                    projected.push_resume
''',1)
t=t.replace('''            if (is_mismatch || !pushAbilityResult(continuation.thread, expected, packet.value->bytes.data()))''','''            if (is_mismatch || prepared->push_resume == nullptr ||
                !prepared->push_resume(continuation.thread, packet.value->bytes))''',1)
t=t.replace('''                                               (!Impl::supportedType(value) || !operation.native_scalar);''','''                                               (operation.push_resume == nullptr ||
                                                value.pass != lux::semantic::EValuePass::VALUE ||
                                                value.lifetime != lux::script::EScriptAbilityValueLifetime::OWNED_VALUE);''')
t=t.replace('''                                               (!Impl::supportedType(value) || !operation.native_scalar ||''','''                                               (operation.push_resume == nullptr ||''')
t=t.replace('''                   left.readable == right.readable && left.writable == right.writable;''','''                   left.readable == right.readable && left.writable == right.writable &&
                   (left.push_resume != nullptr) == (right.push_resume != nullptr);''')
# Delete replaced scalar-only ability helpers; ordinary scalar script function/event paths remain untouched.
for signature in ['        [[nodiscard]] static bool supportedType(const lux::script::ScriptAbilityValueDescription& type) noexcept', '        [[nodiscard]] static bool pushAbilityResult(']:
 a=t.index(signature);start=t.index('{',a);depth=1;i=start+1
 while depth:
  if t[i]=='{':depth+=1
  elif t[i]=='}':depth-=1
  i+=1
 t=t[:a]+t[i+1:]
p.write_text(t)
records=[]
for name in ['LuaValue.hpp','ScriptAbilityLua.hpp']:
 h=s/f'modules/function/script/lua/include/lux/engine/function/script/lua/{name}'
 for pre in ['Debug/include','RelWithDebInfo/include','Android/lux-engine/include']:
  dst=Path('E:/SyncForder/CodeRepos/install')/pre/f'lux/engine/function/script/lua/{name}';shutil.copy2(h,dst);records.append({'source':str(h),'destination':str(dst),'sha256':hashlib.sha256(dst.read_bytes()).hexdigest()})
(o/'r6-async-projection-include-sync.json').write_text(json.dumps(records,indent=2))
