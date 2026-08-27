#include <lux/engine/simulation/LuaScriptBindingBackend.hpp>

#include <lux/engine/function/script/lua/Lua.hpp>

#include <lua.hpp>

#include <algorithm>
#include <memory>
#include <new>
#include <thread>
#include <vector>

namespace lux::simulation
{
    struct LuaScriptBindingBackend::State final
    {
        struct Instance final
        {
            lua_State* state{};
            int table_ref{LUA_NOREF};

            ~Instance()
            {
                if (state && table_ref != LUA_NOREF)
                    luaL_unref(state, LUA_REGISTRYINDEX, table_ref);
            }
        };

        struct InstanceEntry final
        {
            lux::asset::AssetId script;
            ecs::Entity entity{ecs::NullEntity};
            std::uint32_t mount_ordinal{};
            std::weak_ptr<Instance> instance;
        };

        struct Call final
        {
            State* owner{};
            std::shared_ptr<Instance> instance;
            int function_ref{LUA_NOREF};

            ~Call()
            {
                if (owner && function_ref != LUA_NOREF)
                {
                    luaL_unref(
                        owner->state,
                        LUA_REGISTRYINDEX,
                        function_ref
                    );
                    --owner->prepared_references;
                }
            }
        };

        explicit State(std::size_t capacity)
            : state(engine.state()), affinity(std::this_thread::get_id())
        {
            instances.reserve(capacity);
            lua_pushcfunction(state, &State::traceback);
            traceback_ref = luaL_ref(state, LUA_REGISTRYINDEX);
        }

        ~State()
        {
            instances.clear();
            if (state && traceback_ref != LUA_NOREF)
                luaL_unref(state, LUA_REGISTRYINDEX, traceback_ref);
        }

        static int traceback(lua_State* state)
        {
            const char* message = lua_tostring(state, 1);
            luaL_traceback(state, state, message ? message : "script error", 1);
            return 1;
        }

        [[nodiscard]] std::shared_ptr<Instance> loadInstance(
            const ScriptPrepareContext& context,
            const lux::asset::ScriptAssetContent& asset
        ) noexcept
        {
            for (auto& entry : instances)
            {
                if (entry.script == context.script &&
                    entry.entity == context.entity &&
                    entry.mount_ordinal == context.mount_ordinal)
                {
                    if (auto existing = entry.instance.lock())
                        return existing;
                }
            }
            if (instances.size() >= instances.capacity() || asset.payload.empty())
                return {};
            const auto* body = std::get_if<lux::rdesc::LuaSourceScript>(
                std::addressof(asset.description.body)
            );
            if (!body)
                return {};

            lua_rawgeti(state, LUA_REGISTRYINDEX, traceback_ref);
            const auto error_index = lua_gettop(state);
            const auto* bytes = reinterpret_cast<const char*>(
                asset.payload.data()
            );
            if (luaL_loadbufferx(
                    state,
                    bytes,
                    asset.payload.size(),
                    asset.description.module_name.c_str(),
                    "t"
                ) != LUA_OK ||
                lua_pcall(state, 0, 1, error_index) != LUA_OK)
            {
                lua_settop(state, error_index - 1);
                return {};
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
                return {};
            }
            try
            {
                auto instance = std::make_shared<Instance>();
                instance->state = state;
                instance->table_ref = luaL_ref(state, LUA_REGISTRYINDEX);
                instances.push_back(InstanceEntry{
                    context.script,
                    context.entity,
                    context.mount_ordinal,
                    instance});
                ++chunk_loads;
                return instance;
            }
            catch (const std::bad_alloc&)
            {
                return {};
            }
        }

        static bool prepare(
            void* opaque,
            const ScriptPrepareContext& context,
            const lux::asset::ScriptAssetContent& asset,
            const lux::rdesc::ScriptFunction& function,
            lux::script::BoundScriptCall& result
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            if (std::this_thread::get_id() != self.affinity)
                return false;
            auto instance = self.loadInstance(context, asset);
            if (!instance)
                return false;
            lua_rawgeti(self.state, LUA_REGISTRYINDEX, instance->table_ref);
            lua_getfield(self.state, -1, function.name.c_str());
            lua_remove(self.state, -2);
            if (!lua_isfunction(self.state, -1))
            {
                lua_pop(self.state, 1);
                return false;
            }
            try
            {
                auto call = std::make_unique<Call>();
                call->owner = std::addressof(self);
                call->instance = std::move(instance);
                call->function_ref = luaL_ref(
                    self.state,
                    LUA_REGISTRYINDEX
                );
                ++self.prepared_references;
                result = lux::script::BoundScriptCall{
                    &State::invoke,
                    call.release()};
                return true;
            }
            catch (const std::bad_alloc&)
            {
                lua_pop(self.state, 1);
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
            for (std::uint32_t index{}; index < frame->arg_count; ++index)
            {
                const auto& value = frame->args[index];
                switch (value.kind)
                {
                case LUX_SCRIPT_VK_BOOL:
                    lua_pushboolean(
                        self.state,
                        *static_cast<const bool*>(value.data)
                    );
                    break;
                case LUX_SCRIPT_VK_INT32:
                    lua_pushinteger(
                        self.state,
                        *static_cast<const std::int32_t*>(value.data)
                    );
                    break;
                case LUX_SCRIPT_VK_UINT32:
                    lua_pushinteger(
                        self.state,
                        *static_cast<const std::uint32_t*>(value.data)
                    );
                    break;
                case LUX_SCRIPT_VK_INT64:
                    lua_pushinteger(
                        self.state,
                        static_cast<lua_Integer>(
                            *static_cast<const std::int64_t*>(value.data)
                        )
                    );
                    break;
                case LUX_SCRIPT_VK_UINT64:
                    lua_pushinteger(
                        self.state,
                        static_cast<lua_Integer>(
                            *static_cast<const std::uint64_t*>(value.data)
                        )
                    );
                    break;
                case LUX_SCRIPT_VK_FLOAT:
                    lua_pushnumber(
                        self.state,
                        *static_cast<const float*>(value.data)
                    );
                    break;
                case LUX_SCRIPT_VK_DOUBLE:
                    lua_pushnumber(
                        self.state,
                        *static_cast<const double*>(value.data)
                    );
                    break;
                default:
                    lua_settop(self.state, error_index - 1);
                    return -3;
                }
            }
            if (lua_pcall(
                    self.state,
                    static_cast<int>(frame->arg_count),
                    1,
                    error_index
                ) != LUA_OK)
            {
                lua_settop(self.state, error_index - 1);
                return -4;
            }
            const auto status = lua_isnumber(self.state, -1)
                ? static_cast<int>(lua_tointeger(self.state, -1))
                : 0;
            lua_pop(self.state, 1);
            lua_remove(self.state, error_index);
            return status;
        }

        static void release(
            void*,
            lux::script::BoundScriptCall call
        ) noexcept
        {
            delete static_cast<Call*>(call.context);
        }

        lux::script::lua::ScriptEngine engine;
        lua_State* state{};
        std::thread::id affinity;
        int traceback_ref{LUA_NOREF};
        std::vector<InstanceEntry> instances;
        std::size_t chunk_loads{};
        std::size_t prepared_references{};
    };

    LuaScriptBindingBackend::LuaScriptBindingBackend(
        std::size_t instance_capacity
    ) noexcept
    {
        try
        {
            state_ = std::make_unique<State>(instance_capacity);
        }
        catch (const std::bad_alloc&)
        {}
    }

    LuaScriptBindingBackend::~LuaScriptBindingBackend() = default;
    LuaScriptBindingBackend::LuaScriptBindingBackend(
        LuaScriptBindingBackend&&
    ) noexcept = default;
    LuaScriptBindingBackend& LuaScriptBindingBackend::operator=(
        LuaScriptBindingBackend&&
    ) noexcept = default;

    LuaScriptBindingBackend::operator bool() const noexcept
    {
        return state_ && state_->state && state_->traceback_ref != LUA_NOREF;
    }

    ScriptBackendDescriptor LuaScriptBindingBackend::descriptor() noexcept
    {
        return ScriptBackendDescriptor{
            lux::rdesc::Script::Kind::LUA_SOURCE,
            state_.get(),
            &State::prepare,
            &State::release};
    }

    std::size_t LuaScriptBindingBackend::loadedInstanceCount() const noexcept
    {
        return state_ ? state_->instances.size() : 0U;
    }

    std::size_t LuaScriptBindingBackend::chunkLoadCount() const noexcept
    {
        return state_ ? state_->chunk_loads : 0U;
    }

    std::size_t LuaScriptBindingBackend::preparedReferenceCount() const noexcept
    {
        return state_ ? state_->prepared_references : 0U;
    }

    std::size_t LuaScriptBindingBackend::cachedTracebackCount() const noexcept
    {
        return state_ && state_->traceback_ref != LUA_NOREF ? 1U : 0U;
    }
}
