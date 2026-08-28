#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>

#include <lux/engine/function/script/lua/Lua.hpp>

#include <lua.hpp>

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <thread>
#include <unordered_map>
#include <vector>

namespace lux::simulation::script
{
    struct LuaScriptBackend::State final
    {
        struct HostHandle final
        {
            State* owner{};
            ScriptBehavior* host{};
            bool alive{};
        };

        struct Instance final
        {
            State* owner{};
            lux::asset::AssetId asset;
            int table_ref{LUA_NOREF};
            bool entity_scope{};
            HostHandle* host_handle{};
            bool active{};
        };

        struct FunctionKey final
        {
            lux::asset::AssetId asset;
            lux::script::ScriptSymbolId symbol{};

            [[nodiscard]] bool operator==(const FunctionKey&) const noexcept = default;
        };

        struct FunctionKeyHash final
        {
            [[nodiscard]] std::size_t operator()(const FunctionKey& key) const noexcept
            {
                const auto asset_hash = std::hash<lux::asset::AssetId>{}(key.asset);
                const auto symbol_hash = std::hash<lux::script::ScriptSymbolId>{}(key.symbol);
                return asset_hash ^ (symbol_hash + 0x9E3779B9U + (asset_hash << 6U) + (asset_hash >> 2U));
            }
        };

        struct LuaFunctionBinding final
        {
            lux::rdesc::ScriptFunction signature;
            int function_ref{LUA_NOREF};
            std::vector<const LuaRecordMarshaller*> argument_marshallers;
        };

        struct PreparedCall final
        {
            Instance* instance{};
            const LuaFunctionBinding* function{};
            bool active{};
        };

        State(
            std::size_t capacity,
            std::size_t prepared_capacity,
            std::span<const LuaComponentBinding> source_components,
            std::span<const LuaRecordMarshaller> source_record_marshallers
        )
            : state(engine.state()),
              affinity(std::this_thread::get_id()),
              instance_capacity(capacity)
        {
            prototypes.reserve(capacity);
            components.assign(
                source_components.begin(),
                source_components.end()
            );
            component_index.reserve(components.size());
            for (std::size_t index{}; index < components.size(); ++index)
                component_index.emplace(components[index].name, index);
            record_marshallers.assign(
                source_record_marshallers.begin(),
                source_record_marshallers.end()
            );
            record_marshaller_index.reserve(record_marshallers.size());
            for (std::size_t index{}; index < record_marshallers.size(); ++index)
            {
                record_marshaller_index.emplace(
                    record_marshallers[index].semantic_type,
                    index
                );
            }
            instances.resize(instance_capacity);
            free_instances.reserve(instance_capacity);
            for (std::size_t index = instance_capacity; index > 0U; --index)
                free_instances.push_back(index - 1U);
            function_bindings.reserve(prepared_capacity);
            function_index.reserve(prepared_capacity);
            prepared_calls.resize(prepared_capacity);
            free_prepared_calls.reserve(prepared_capacity);
            for (std::size_t index = prepared_capacity; index > 0U; --index)
                free_prepared_calls.push_back(index - 1U);
            lua_pushcfunction(state, &State::traceback);
            traceback_ref = luaL_ref(state, LUA_REGISTRYINDEX);
        }

        ~State()
        {
            for (const auto& [asset, prototype] : prototypes)
            {
                static_cast<void>(asset);
                if (prototype != LUA_NOREF)
                    luaL_unref(state, LUA_REGISTRYINDEX, prototype);
            }
            for (const auto& function : function_bindings)
            {
                if (function.function_ref != LUA_NOREF)
                    luaL_unref(state, LUA_REGISTRYINDEX, function.function_ref);
            }
            if (state && traceback_ref != LUA_NOREF)
                luaL_unref(state, LUA_REGISTRYINDEX, traceback_ref);
        }

        static int traceback(lua_State* state)
        {
            const char* message = lua_tostring(state, 1);
            luaL_traceback(state, state, message ? message : "script error", 1);
            return 1;
        }

        [[nodiscard]] int prototypeFor(
            const ScriptInstanceCreateContext& context,
            const lux::script::ScriptArtifact& artifact
        ) noexcept
        {
            const auto found = prototypes.find(context.asset);
            if (found != prototypes.end())
                return found->second;
            if (artifact.payload().empty())
                return LUA_NOREF;
            const auto* body = std::get_if<lux::rdesc::LuaSourceScript>(
                std::addressof(artifact.description().body));
            if (!body)
                return LUA_NOREF;
            lua_rawgeti(state, LUA_REGISTRYINDEX, traceback_ref);
            const auto error_index = lua_gettop(state);
            const auto* source = reinterpret_cast<const char*>(
                artifact.payload().data());
            if (luaL_loadbufferx(
                    state,
                    source,
                    artifact.payload().size(),
                    artifact.description().module_name.c_str(),
                    "t") != LUA_OK ||
                lua_pcall(state, 0, 1, error_index) != LUA_OK)
            {
                lua_settop(state, error_index - 1);
                return LUA_NOREF;
            }
            lua_remove(state, error_index);
            if (!lua_istable(state, -1))
            {
                lua_pop(state, 1);
                lua_getglobal(state, body->entry.c_str());
            }
            if (!lua_istable(state, -1))
            {
                lua_pop(state, 1);
                return LUA_NOREF;
            }
            const auto reference = luaL_ref(state, LUA_REGISTRYINDEX);
            try
            {
                prototypes.emplace(context.asset, reference);
                return reference;
            }
            catch (const std::bad_alloc&)
            {
                luaL_unref(state, LUA_REGISTRYINDEX, reference);
                return LUA_NOREF;
            }
        }

        [[nodiscard]] bool supportedType(
            const lux::rdesc::ScriptValueType& type
        ) const noexcept
        {
            const auto* layout = lux::semantic::builtinLayout(type.type_id);
            if (!layout || layout->canonical_name != type.canonical_name)
                return false;
            switch (layout->abi_kind)
            {
            case LUX_SCRIPT_VK_BOOL:
            case LUX_SCRIPT_VK_INT32:
            case LUX_SCRIPT_VK_UINT32:
            case LUX_SCRIPT_VK_INT64:
            case LUX_SCRIPT_VK_FLOAT:
            case LUX_SCRIPT_VK_DOUBLE:
                return true;
            default:
                return false;
            }
        }

        [[nodiscard]] const LuaRecordMarshaller* recordMarshaller(
            const lux::rdesc::ScriptValueType& type
        ) const noexcept
        {
            if (type.pass != lux::semantic::EValuePass::CONST_REF)
                return nullptr;
            const auto found = record_marshaller_index.find(type.type_id);
            if (found == record_marshaller_index.end())
                return nullptr;
            const auto& marshaller = record_marshallers[found->second];
            return marshaller.canonical_name == type.canonical_name
                ? std::addressof(marshaller)
                : nullptr;
        }

        [[nodiscard]] const LuaComponentBinding* component(
            std::string_view name
        ) const noexcept
        {
            const auto found = component_index.find(name);
            return found == component_index.end()
                ? nullptr
                : std::addressof(components[found->second]);
        }

        [[nodiscard]] static HostHandle* hostHandle(lua_State* state) noexcept
        {
            return static_cast<HostHandle*>(lua_touserdata(
                state,
                lua_upvalueindex(1)
            ));
        }

        static int hasComponent(lua_State* state) noexcept
        {
            auto* handle = hostHandle(state);
            const char* name = lua_tostring(state, 2);
            const auto* binding = handle && handle->alive && handle->owner &&
                name
                ? handle->owner->component(name)
                : nullptr;
            lua_pushboolean(
                state,
                binding && handle->host &&
                    handle->host->read(binding->component_type));
            return 1;
        }

        static bool pushComponentValue(
            lua_State* state,
            std::uint8_t kind,
            const void* value
        ) noexcept
        {
            if (!value)
                return false;
            switch (kind)
            {
            case LUX_SCRIPT_VK_BOOL:
                lua_pushboolean(state, *static_cast<const bool*>(value));
                return true;
            case LUX_SCRIPT_VK_INT32:
                lua_pushinteger(state, *static_cast<const std::int32_t*>(value));
                return true;
            case LUX_SCRIPT_VK_UINT32:
                lua_pushinteger(state, *static_cast<const std::uint32_t*>(value));
                return true;
            case LUX_SCRIPT_VK_INT64:
                lua_pushinteger(state, *static_cast<const std::int64_t*>(value));
                return true;
            case LUX_SCRIPT_VK_FLOAT:
                lua_pushnumber(state, *static_cast<const float*>(value));
                return true;
            case LUX_SCRIPT_VK_DOUBLE:
                lua_pushnumber(state, *static_cast<const double*>(value));
                return true;
            default:
                return false;
            }
        }

        static int getComponent(lua_State* state) noexcept
        {
            auto* handle = hostHandle(state);
            const char* name = lua_tostring(state, 2);
            const auto* binding = handle && handle->alive && handle->owner &&
                name
                ? handle->owner->component(name)
                : nullptr;
            const auto* value = binding && handle->host
                ? handle->host->read(binding->component_type)
                : nullptr;
            if (!binding || !pushComponentValue(state, binding->abi_kind, value))
                lua_pushnil(state);
            return 1;
        }

        template <class Type>
        [[nodiscard]] static bool readStrictNumber(
            lua_State* state,
            int index,
            Type& result
        ) noexcept
        {
            if (lua_type(state, index) != LUA_TNUMBER)
                return false;
            const auto value = lua_tonumber(state, index);
            if constexpr (std::is_integral_v<Type>)
            {
                if (!std::isfinite(value) || std::trunc(value) != value ||
                    value < static_cast<lua_Number>(
                        std::numeric_limits<Type>::lowest()) ||
                    value > static_cast<lua_Number>(
                        std::numeric_limits<Type>::max()))
                {
                    return false;
                }
            }
            else if (!std::isfinite(value) ||
                     value < -static_cast<lua_Number>(
                         std::numeric_limits<Type>::max()) ||
                     value > static_cast<lua_Number>(
                         std::numeric_limits<Type>::max()))
            {
                return false;
            }
            result = static_cast<Type>(value);
            return true;
        }

        static int patchComponent(lua_State* state) noexcept
        {
            auto* handle = hostHandle(state);
            const char* name = lua_tostring(state, 2);
            const auto* binding = handle && handle->alive && handle->owner &&
                name
                ? handle->owner->component(name)
                : nullptr;
            if (!binding || !handle->host)
            {
                lua_pushboolean(state, false);
                return 1;
            }
            alignas(std::uint64_t) std::byte storage[sizeof(std::uint64_t)]{};
            bool valid{};
            switch (binding->abi_kind)
            {
            case LUX_SCRIPT_VK_BOOL:
                if (lua_type(state, 3) == LUA_TBOOLEAN)
                {
                    *reinterpret_cast<bool*>(storage) =
                        lua_toboolean(state, 3) != 0;
                    valid = true;
                }
                break;
            case LUX_SCRIPT_VK_INT32:
                valid = readStrictNumber(
                    state,
                    3,
                    *reinterpret_cast<std::int32_t*>(storage));
                break;
            case LUX_SCRIPT_VK_UINT32:
                valid = readStrictNumber(
                    state,
                    3,
                    *reinterpret_cast<std::uint32_t*>(storage));
                break;
            case LUX_SCRIPT_VK_INT64:
                valid = readStrictNumber(
                    state,
                    3,
                    *reinterpret_cast<std::int64_t*>(storage));
                break;
            case LUX_SCRIPT_VK_FLOAT:
                valid = readStrictNumber(
                    state,
                    3,
                    *reinterpret_cast<float*>(storage));
                break;
            case LUX_SCRIPT_VK_DOUBLE:
                valid = readStrictNumber(
                    state,
                    3,
                    *reinterpret_cast<double*>(storage));
                break;
            default:
                break;
            }
            lua_pushboolean(
                state,
                valid && handle->host->patch(
                    binding->component_type,
                    storage
                ));
            return 1;
        }

        static EScriptBackendResult createInstance(
            void* opaque,
            const ScriptInstanceCreateContext& context,
            const lux::script::ScriptArtifact& artifact,
            ScriptBackendInstance& result
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            if (std::this_thread::get_id() != self.affinity)
                return EScriptBackendResult::CONSTRUCTION_FAILURE;
            if (self.free_instances.empty())
                return EScriptBackendResult::CAPACITY_EXCEEDED;
            for (const auto& binding : self.components)
            {
                ScriptHostComponentContract contract;
                if (!context.behavior || !context.behavior->componentContract(
                        binding.component_type,
                        contract) ||
                    contract.component_type != binding.component_type ||
                    contract.semantic_type != binding.semantic_type ||
                    contract.canonical_name != binding.canonical_name ||
                    contract.abi_kind != binding.abi_kind ||
                    contract.size != binding.size ||
                    contract.alignment != binding.alignment)
                {
                    return EScriptBackendResult::
                        HOST_COMPONENT_CONTRACT_MISMATCH;
                }
            }
            const auto prototype = self.prototypeFor(context, artifact);
            if (prototype == LUA_NOREF)
                return EScriptBackendResult::CONSTRUCTION_FAILURE;

            const auto instance_slot = self.free_instances.back();
            self.free_instances.pop_back();
            auto* instance = std::addressof(self.instances[instance_slot]);
            *instance = Instance{
                std::addressof(self),
                context.asset,
                LUA_NOREF,
                std::holds_alternative<EntityScriptScope>(context.scope),
                nullptr,
                true};

            lua_newtable(self.state);
            const auto instance_index = lua_gettop(self.state);
            lua_rawgeti(self.state, LUA_REGISTRYINDEX, prototype);
            const auto prototype_index = lua_gettop(self.state);
            lua_pushnil(self.state);
            while (lua_next(self.state, prototype_index) != 0)
            {
                lua_pushvalue(self.state, -2);
                lua_pushvalue(self.state, -2);
                lua_settable(self.state, instance_index);
                lua_pop(self.state, 1);
            }
            lua_pop(self.state, 1);
            if (instance->entity_scope)
            {
                auto* handle = static_cast<HostHandle*>(
                    lua_newuserdata(self.state, sizeof(HostHandle)));
                *handle = HostHandle{
                    std::addressof(self),
                    context.behavior,
                    true};
                instance->host_handle = handle;
                const auto handle_index = lua_gettop(self.state);
                lua_pushvalue(self.state, handle_index);
                lua_pushcclosure(self.state, &State::hasComponent, 1);
                lua_setfield(self.state, instance_index, "has_component");
                lua_pushvalue(self.state, handle_index);
                lua_pushcclosure(self.state, &State::getComponent, 1);
                lua_setfield(self.state, instance_index, "get_component");
                lua_pushvalue(self.state, handle_index);
                lua_pushcclosure(self.state, &State::patchComponent, 1);
                lua_setfield(self.state, instance_index, "patch_component");
                lua_pop(self.state, 1);
            }
            const auto table_ref = luaL_ref(self.state, LUA_REGISTRYINDEX);
            instance->table_ref = table_ref;
            result.value = instance;
            return EScriptBackendResult::SUCCESS;
        }

        static EScriptBackendResult prepareMethod(
            void* opaque,
            ScriptBackendInstance instance_value,
            const lux::rdesc::ScriptFunction& function,
            lux::script::BoundScriptCall& result
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            auto* instance = static_cast<Instance*>(instance_value.value);
            if (!instance || std::this_thread::get_id() != self.affinity)
                return EScriptBackendResult::CONSTRUCTION_FAILURE;
            if (self.free_prepared_calls.empty())
                return EScriptBackendResult::CAPACITY_EXCEEDED;

            const FunctionKey key{instance->asset, function.symbol_id};
            const LuaFunctionBinding* function_binding{};
            const auto cached = self.function_index.find(key);
            if (cached != self.function_index.end())
            {
                function_binding = std::addressof(self.function_bindings[cached->second]);
                if (function_binding->signature != function)
                    return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
            }
            else
            {
                if (self.function_bindings.size() >= self.prepared_calls.size())
                    return EScriptBackendResult::CAPACITY_EXCEEDED;
                std::vector<const LuaRecordMarshaller*> argument_marshallers;
                try
                {
                    argument_marshallers.reserve(function.args.size());
                    for (const auto& argument : function.args)
                    {
                        const auto* record = self.recordMarshaller(argument);
                        if (!self.supportedType(argument) && !record)
                            return EScriptBackendResult::UNSUPPORTED_MARSHAL_TYPE;
                        argument_marshallers.push_back(record);
                    }
                    for (const auto& return_type : function.returns)
                    {
                        const bool unsupported_return = !self.supportedType(return_type) ||
                            return_type.pass != lux::semantic::EValuePass::VALUE;
                        if (unsupported_return)
                            return EScriptBackendResult::UNSUPPORTED_MARSHAL_TYPE;
                    }
                }
                catch (const std::bad_alloc&)
                {
                    return EScriptBackendResult::ALLOCATION_FAILURE;
                }

                const auto prototype = self.prototypes.find(instance->asset);
                if (prototype == self.prototypes.end())
                    return EScriptBackendResult::CONSTRUCTION_FAILURE;
                lua_rawgeti(self.state, LUA_REGISTRYINDEX, prototype->second);
                lua_getfield(self.state, -1, function.name.c_str());
                lua_remove(self.state, -2);
                if (!lua_isfunction(self.state, -1))
                {
                    lua_pop(self.state, 1);
                    return EScriptBackendResult::CONSTRUCTION_FAILURE;
                }
                const auto function_ref = luaL_ref(self.state, LUA_REGISTRYINDEX);
                try
                {
                    const auto binding_index = self.function_bindings.size();
                    self.function_bindings.push_back(LuaFunctionBinding{
                        function,
                        function_ref,
                        std::move(argument_marshallers)
                    });
                    if (!self.function_index.emplace(key, binding_index).second)
                    {
                        self.function_bindings.pop_back();
                        luaL_unref(self.state, LUA_REGISTRYINDEX, function_ref);
                        return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
                    }
                    function_binding = std::addressof(self.function_bindings.back());
                }
                catch (const std::bad_alloc&)
                {
                    if (!self.function_bindings.empty() &&
                        self.function_bindings.back().function_ref == function_ref)
                    {
                        self.function_bindings.pop_back();
                    }
                    luaL_unref(self.state, LUA_REGISTRYINDEX, function_ref);
                    return EScriptBackendResult::ALLOCATION_FAILURE;
                }
            }

            const auto call_slot = self.free_prepared_calls.back();
            self.free_prepared_calls.pop_back();
            auto& call = self.prepared_calls[call_slot];
            call.instance = instance;
            call.function = function_binding;
            call.active = true;
            result = lux::script::BoundScriptCall{&State::invoke, std::addressof(call)};
            return EScriptBackendResult::SUCCESS;
        }

        static bool pushArgument(
            lua_State* state,
            const lux_script_value_slot& value,
            const LuaRecordMarshaller* record
        ) noexcept
        {
            if (!value.data)
                return false;
            if (record)
            {
                const auto address = reinterpret_cast<std::uintptr_t>(
                    value.data
                );
                const bool valid_layout = value.kind ==
                        LUX_SCRIPT_VK_STRUCT_REF &&
                    value.type_id == record->semantic_type &&
                    value.size == record->size &&
                    address % record->alignment == 0U;
                return valid_layout && record->push && record->push(
                    record->context,
                    state,
                    value.data
                );
            }
            switch (value.kind)
            {
            case LUX_SCRIPT_VK_BOOL:
                lua_pushboolean(state, *static_cast<const bool*>(value.data));
                return true;
            case LUX_SCRIPT_VK_INT32:
                lua_pushinteger(
                    state,
                    *static_cast<const std::int32_t*>(value.data));
                return true;
            case LUX_SCRIPT_VK_UINT32:
                lua_pushinteger(
                    state,
                    *static_cast<const std::uint32_t*>(value.data));
                return true;
            case LUX_SCRIPT_VK_INT64:
                lua_pushinteger(
                    state,
                    static_cast<lua_Integer>(
                        *static_cast<const std::int64_t*>(value.data)));
                return true;
            case LUX_SCRIPT_VK_FLOAT:
                lua_pushnumber(state, *static_cast<const float*>(value.data));
                return true;
            case LUX_SCRIPT_VK_DOUBLE:
                lua_pushnumber(state, *static_cast<const double*>(value.data));
                return true;
            default:
                return false;
            }
        }

        template <class Type>
        static bool writeNumber(
            lux_script_value_slot& slot,
            Type value
        ) noexcept
        {
            if (!slot.data || slot.size < sizeof(Type))
                return false;
            std::memcpy(slot.data, std::addressof(value), sizeof(Type));
            return true;
        }

        static bool readReturn(
            lua_State* state,
            int index,
            lux_script_value_slot& slot
        ) noexcept
        {
            switch (slot.kind)
            {
            case LUX_SCRIPT_VK_BOOL:
                if (lua_type(state, index) != LUA_TBOOLEAN)
                    return false;
                return writeNumber<bool>(
                    slot,
                    lua_toboolean(state, index) != 0);
            case LUX_SCRIPT_VK_INT32:
            {
                std::int32_t value{};
                return readStrictNumber(state, index, value) &&
                    writeNumber(slot, value);
            }
            case LUX_SCRIPT_VK_UINT32:
            {
                std::uint32_t value{};
                return readStrictNumber(state, index, value) &&
                    writeNumber(slot, value);
            }
            case LUX_SCRIPT_VK_INT64:
            {
                std::int64_t value{};
                return readStrictNumber(state, index, value) &&
                    writeNumber(slot, value);
            }
            case LUX_SCRIPT_VK_FLOAT:
            {
                float value{};
                return readStrictNumber(state, index, value) &&
                    writeNumber(slot, value);
            }
            case LUX_SCRIPT_VK_DOUBLE:
            {
                double value{};
                return readStrictNumber(state, index, value) &&
                    writeNumber(slot, value);
            }
            default:
                return false;
            }
        }

        static int invoke(lux_script_call_frame* frame) noexcept
        {
            if (!frame || !frame->user_context)
                return -1;
            auto& call = *static_cast<PreparedCall*>(frame->user_context);
            if (!call.active || !call.instance || !call.function)
                return -1;
            auto& self = *call.instance->owner;
            if (std::this_thread::get_id() != self.affinity)
                return -2;
            lua_rawgeti(self.state, LUA_REGISTRYINDEX, self.traceback_ref);
            const auto error_index = lua_gettop(self.state);
            lua_rawgeti(self.state, LUA_REGISTRYINDEX, call.function->function_ref);
            std::uint32_t argument_count{};
            if (call.instance->entity_scope)
            {
                lua_rawgeti(
                    self.state,
                    LUA_REGISTRYINDEX,
                    call.instance->table_ref);
                ++argument_count;
            }
            for (std::uint32_t index{}; index < frame->arg_count; ++index)
            {
                const auto* record = index < call.function->argument_marshallers.size()
                    ? call.function->argument_marshallers[index]
                    : nullptr;
                if (!pushArgument(self.state, frame->args[index], record))
                {
                    lua_settop(self.state, error_index - 1);
                    return -3;
                }
                ++argument_count;
            }
            if (lua_pcall(
                    self.state,
                    static_cast<int>(argument_count),
                    static_cast<int>(frame->return_count),
                    error_index) != LUA_OK)
            {
                lua_settop(self.state, error_index - 1);
                return -4;
            }
            for (std::uint32_t index{}; index < frame->return_count; ++index)
            {
                const auto stack_index =
                    error_index + 1 + static_cast<int>(index);
                if (!readReturn(
                        self.state,
                        stack_index,
                        frame->returns[index]))
                {
                    lua_settop(self.state, error_index - 1);
                    return -5;
                }
            }
            lua_settop(self.state, error_index - 1);
            return 0;
        }

        static void releaseMethod(
            void* opaque,
            ScriptBackendInstance,
            lux::script::BoundScriptCall call_value
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            auto* call = static_cast<PreparedCall*>(call_value.context);
            if (!call || !call->active)
                return;
            call->instance = nullptr;
            call->function = nullptr;
            call->active = false;
            const auto call_slot = static_cast<std::size_t>(call - self.prepared_calls.data());
            self.free_prepared_calls.push_back(call_slot);
        }

        static void destroyInstance(
            void* opaque,
            ScriptBackendInstance instance_value
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            auto* instance = static_cast<Instance*>(instance_value.value);
            if (!instance)
                return;
            if (instance->host_handle)
            {
                instance->host_handle->alive = false;
                instance->host_handle->host = nullptr;
                instance->host_handle->owner = nullptr;
                instance->host_handle = nullptr;
            }
            if (instance->table_ref != LUA_NOREF)
                luaL_unref(self.state, LUA_REGISTRYINDEX, instance->table_ref);
            const auto instance_slot = static_cast<std::size_t>(
                instance - self.instances.data()
            );
            *instance = {};
            self.free_instances.push_back(instance_slot);
        }

        lux::script::lua::ScriptEngine engine;
        lua_State* state{};
        std::thread::id affinity;
        int traceback_ref{LUA_NOREF};
        std::size_t instance_capacity{};
        std::unordered_map<lux::asset::AssetId, int> prototypes;
        std::vector<LuaComponentBinding> components;
        std::unordered_map<std::string_view, std::size_t> component_index;
        std::vector<LuaRecordMarshaller> record_marshallers;
        std::unordered_map<std::uint64_t, std::size_t>
            record_marshaller_index;
        std::vector<Instance> instances;
        std::vector<std::size_t> free_instances;
        std::vector<LuaFunctionBinding> function_bindings;
        std::unordered_map<FunctionKey, std::size_t, FunctionKeyHash> function_index;
        std::vector<PreparedCall> prepared_calls;
        std::vector<std::size_t> free_prepared_calls;
    };

    lux::cxx::expected<
        LuaScriptBackend,
        ELuaScriptBindingBackendError> LuaScriptBackend::create(
            std::size_t instance_capacity,
            std::size_t prepared_call_capacity,
            std::span<const LuaComponentBinding> components,
            std::span<const LuaRecordMarshaller> record_marshallers
        ) noexcept
    {
        if (instance_capacity == 0U || prepared_call_capacity == 0U)
            return lux::cxx::unexpected(ELuaScriptBindingBackendError::INVALID_CAPACITY);
        for (std::size_t index{}; index < components.size(); ++index)
        {
            const auto& component = components[index];
            const auto* layout = lux::semantic::builtinLayout(
                component.semantic_type);
            const bool supported_kind = component.abi_kind ==
                    LUX_SCRIPT_VK_BOOL ||
                component.abi_kind == LUX_SCRIPT_VK_INT32 ||
                component.abi_kind == LUX_SCRIPT_VK_UINT32 ||
                component.abi_kind == LUX_SCRIPT_VK_INT64 ||
                component.abi_kind == LUX_SCRIPT_VK_FLOAT ||
                component.abi_kind == LUX_SCRIPT_VK_DOUBLE;
            if (component.name.empty() || component.component_type == 0U ||
                component.canonical_name.empty() ||
                component.semantic_type !=
                    lux::semantic::typeId(
                        component.canonical_name) ||
                !layout ||
                layout->canonical_name != component.canonical_name ||
                layout->abi_kind != component.abi_kind ||
                layout->size != component.size ||
                layout->alignment != component.alignment ||
                !supported_kind)
            {
                return lux::cxx::unexpected(
                    ELuaScriptBindingBackendError::
                        INVALID_COMPONENT_CONTRACT);
            }
            for (std::size_t previous{}; previous < index; ++previous)
            {
                if (components[previous].name == component.name)
                {
                    return lux::cxx::unexpected(
                        ELuaScriptBindingBackendError::
                            DUPLICATE_COMPONENT_NAME);
                }
                if (components[previous].component_type ==
                    component.component_type)
                {
                    return lux::cxx::unexpected(
                        ELuaScriptBindingBackendError::
                            INVALID_COMPONENT_CONTRACT);
                }
            }
        }
        for (std::size_t index{}; index < record_marshallers.size(); ++index)
        {
            const auto& marshaller = record_marshallers[index];
            const bool power_of_two_alignment = marshaller.alignment != 0U &&
                (marshaller.alignment & (marshaller.alignment - 1U)) == 0U;
            const bool valid_identity = marshaller.semantic_type != 0U &&
                !marshaller.canonical_name.empty() &&
                marshaller.semantic_type == lux::semantic::typeId(
                    marshaller.canonical_name
                );
            if (!valid_identity || marshaller.size == 0U ||
                !power_of_two_alignment || !marshaller.push)
            {
                return lux::cxx::unexpected(
                    ELuaScriptBindingBackendError::INVALID_RECORD_MARSHALLER
                );
            }
            for (std::size_t previous{}; previous < index; ++previous)
            {
                const auto& candidate = record_marshallers[previous];
                if (candidate.semantic_type == marshaller.semantic_type ||
                    candidate.canonical_name == marshaller.canonical_name)
                {
                    return lux::cxx::unexpected(
                        ELuaScriptBindingBackendError::
                            DUPLICATE_RECORD_MARSHALLER
                    );
                }
            }
        }
        try
        {
            return LuaScriptBackend{
                std::make_unique<State>(
                    instance_capacity,
                    prepared_call_capacity,
                    components,
                    record_marshallers
                )};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                ELuaScriptBindingBackendError::ALLOCATION_FAILURE);
        }
    }

    LuaScriptBackend::LuaScriptBackend(
        std::unique_ptr<State> state
    ) noexcept
        : state_(std::move(state))
    {
    }

    LuaScriptBackend::~LuaScriptBackend() = default;
    LuaScriptBackend::LuaScriptBackend(
        LuaScriptBackend&&
    ) noexcept = default;
    LuaScriptBackend& LuaScriptBackend::operator=(
        LuaScriptBackend&&
    ) noexcept = default;

    LuaScriptBackend::operator bool() const noexcept
    {
        return state_ && state_->state && state_->traceback_ref != LUA_NOREF;
    }

    ScriptBackendDescriptor LuaScriptBackend::descriptor() noexcept
    {
        return ScriptBackendDescriptor{
            lux::rdesc::Script::Kind::LUA_SOURCE,
            state_.get(),
            &State::createInstance,
            &State::prepareMethod,
            &State::releaseMethod,
            &State::destroyInstance};
    }

}
