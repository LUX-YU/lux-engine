#include <lux/engine/simulation/script/lua/LuaScriptBackend.hpp>

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
#include <vector>

namespace lux::simulation::script
{
    struct LuaScriptBackend::State final
    {
        struct Prototype final
        {
            lux::asset::AssetId script;
            int table_ref{LUA_NOREF};
        };

        struct HostHandle final
        {
            State* owner{};
            ScriptBehavior* host{};
            bool alive{};
        };

        struct Instance final
        {
            State* owner{};
            int table_ref{LUA_NOREF};
            bool entity_scope{};
            HostHandle* host_handle{};
        };

        struct Call final
        {
            State* owner{};
            Instance* instance{};
            int function_ref{LUA_NOREF};
        };

        State(
            std::size_t capacity,
            std::span<const LuaComponentBinding> source_components
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
            lua_pushcfunction(state, &State::traceback);
            traceback_ref = luaL_ref(state, LUA_REGISTRYINDEX);
        }

        ~State()
        {
            for (const auto& prototype : prototypes)
            {
                if (prototype.table_ref != LUA_NOREF)
                    luaL_unref(state, LUA_REGISTRYINDEX, prototype.table_ref);
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
            const lux::asset::ScriptAssetContent& asset
        ) noexcept
        {
            for (const auto& prototype : prototypes)
            {
                if (prototype.script == context.asset)
                    return prototype.table_ref;
            }
            if (asset.payload.empty())
                return LUA_NOREF;
            const auto* body = std::get_if<lux::rdesc::LuaSourceScript>(
                std::addressof(asset.description.body));
            if (!body)
                return LUA_NOREF;
            lua_rawgeti(state, LUA_REGISTRYINDEX, traceback_ref);
            const auto error_index = lua_gettop(state);
            const auto* source = reinterpret_cast<const char*>(
                asset.payload.data());
            if (luaL_loadbufferx(
                    state,
                    source,
                    asset.payload.size(),
                    asset.description.module_name.c_str(),
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
                prototypes.push_back(Prototype{context.asset, reference});
                ++chunk_loads;
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
            const auto* layout = lux::script::scriptBuiltinLayout(type.type_id);
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

        [[nodiscard]] const LuaComponentBinding* component(
            std::string_view name
        ) const noexcept
        {
            const auto found = std::find_if(
                components.begin(),
                components.end(),
                [name](const LuaComponentBinding& value) noexcept
                {
                    return value.name == name;
                }
            );
            return found == components.end() ? nullptr : std::addressof(*found);
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
            const lux::asset::ScriptAssetContent& asset,
            ScriptBackendInstance& result
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            if (std::this_thread::get_id() != self.affinity)
                return EScriptBackendResult::CONSTRUCTION_FAILURE;
            if (self.live_instances >= self.instance_capacity)
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
            const auto prototype = self.prototypeFor(context, asset);
            if (prototype == LUA_NOREF)
                return EScriptBackendResult::CONSTRUCTION_FAILURE;

            auto* instance = new (std::nothrow) Instance{
                std::addressof(self),
                LUA_NOREF,
                std::holds_alternative<EntityScriptScope>(context.scope),
                nullptr};
            if (!instance)
                return EScriptBackendResult::ALLOCATION_FAILURE;

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
            ++self.live_instances;
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
            for (const auto& argument : function.args)
            {
                if (!self.supportedType(argument))
                    return EScriptBackendResult::UNSUPPORTED_MARSHAL_TYPE;
            }
            for (const auto& return_type : function.returns)
            {
                if (!self.supportedType(return_type) ||
                    return_type.pass != lux::script::EScriptPassMode::VALUE)
                {
                    return EScriptBackendResult::UNSUPPORTED_MARSHAL_TYPE;
                }
            }
            lua_rawgeti(self.state, LUA_REGISTRYINDEX, instance->table_ref);
            lua_getfield(self.state, -1, function.name.c_str());
            lua_remove(self.state, -2);
            if (!lua_isfunction(self.state, -1))
            {
                lua_pop(self.state, 1);
                return EScriptBackendResult::CONSTRUCTION_FAILURE;
            }
            const auto function_ref = luaL_ref(
                self.state,
                LUA_REGISTRYINDEX);
            auto* call = new (std::nothrow) Call{
                std::addressof(self),
                instance,
                function_ref};
            if (!call)
            {
                luaL_unref(self.state, LUA_REGISTRYINDEX, function_ref);
                return EScriptBackendResult::ALLOCATION_FAILURE;
            }
            ++self.prepared_references;
            result = lux::script::BoundScriptCall{&State::invoke, call};
            return EScriptBackendResult::SUCCESS;
        }

        static bool pushArgument(
            lua_State* state,
            const lux_script_value_slot& value
        ) noexcept
        {
            if (!value.data)
                return false;
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
            auto& call = *static_cast<Call*>(frame->user_context);
            auto& self = *call.owner;
            if (std::this_thread::get_id() != self.affinity)
                return -2;
            lua_rawgeti(self.state, LUA_REGISTRYINDEX, self.traceback_ref);
            const auto error_index = lua_gettop(self.state);
            lua_rawgeti(self.state, LUA_REGISTRYINDEX, call.function_ref);
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
                if (!pushArgument(self.state, frame->args[index]))
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
            auto* call = static_cast<Call*>(call_value.context);
            if (!call)
                return;
            if (call->function_ref != LUA_NOREF)
            {
                luaL_unref(
                    self.state,
                    LUA_REGISTRYINDEX,
                    call->function_ref);
                if (self.prepared_references != 0U)
                    --self.prepared_references;
            }
            delete call;
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
            delete instance;
            if (self.live_instances != 0U)
                --self.live_instances;
        }

        lux::script::lua::ScriptEngine engine;
        lua_State* state{};
        std::thread::id affinity;
        int traceback_ref{LUA_NOREF};
        std::size_t instance_capacity{};
        std::size_t live_instances{};
        std::vector<Prototype> prototypes;
        std::vector<LuaComponentBinding> components;
        std::size_t chunk_loads{};
        std::size_t prepared_references{};
    };

    lux::cxx::expected<
        LuaScriptBackend,
        ELuaScriptBindingBackendError> LuaScriptBackend::create(
            std::size_t instance_capacity,
            std::span<const LuaComponentBinding> components
        ) noexcept
    {
        for (std::size_t index{}; index < components.size(); ++index)
        {
            const auto& component = components[index];
            const auto* layout = lux::script::scriptBuiltinLayout(
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
                    lux::script::scriptSemanticTypeId(
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
        try
        {
            return LuaScriptBackend{
                std::make_unique<State>(instance_capacity, components)};
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
            nullptr,
            nullptr,
            &State::releaseMethod,
            &State::destroyInstance};
    }

    std::size_t LuaScriptBackend::loadedInstanceCount() const noexcept
    {
        return state_ ? state_->live_instances : 0U;
    }

    std::size_t LuaScriptBackend::chunkLoadCount() const noexcept
    {
        return state_ ? state_->chunk_loads : 0U;
    }

    std::size_t LuaScriptBackend::preparedReferenceCount() const noexcept
    {
        return state_ ? state_->prepared_references : 0U;
    }

    std::size_t LuaScriptBackend::cachedTracebackCount() const noexcept
    {
        return state_ && state_->traceback_ref != LUA_NOREF ? 1U : 0U;
    }
}
