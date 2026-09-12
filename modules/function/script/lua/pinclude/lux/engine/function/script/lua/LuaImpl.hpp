#pragma once
#include <string_view>
#include <lux/engine/function/script/lua/Lua.hpp>

#include <lua.hpp>
#include <lux/engine/function/script/lua/LuaPageAllocator.hpp>
#include <lux/engine/function/script/lua/LuaBoundary.h>

namespace lux::script::lua
{
    class ScriptEngineImpl
    {
    public:
        explicit ScriptEngineImpl(LuaVmConfiguration config) : allocator_(config)
        {
            const bool invalid_mode = config.gc_mode != ELuaGcMode::INCREMENTAL &&
                config.gc_mode != ELuaGcMode::GENERATIONAL;
            if (invalid_mode || std::ranges::any_of(config.gc_parameters, [](int value) { return value < -1; }))
                return;
            L_ = lua_newstate(allocator_.callback(), &allocator_, config.seed);
            if (!L_) return;
            lua_pushcfunction(L_, &luxLuaBootstrap);
            if (lua_pcall(L_, 0, 0, 0) != LUA_OK)
            {
                lua_close(L_);
                L_ = nullptr;
                return;
            }
            lua_gc(L_, config.gc_mode == ELuaGcMode::GENERATIONAL ? LUA_GCGEN : LUA_GCINC);
            for (int index{}; index < LUA_GCPN; ++index)
                if (config.gc_parameters[index] != -1) lua_gc(L_, LUA_GCPARAM, index, config.gc_parameters[index]);
        }
        [[nodiscard]] LuaAllocationStats allocationStats() const noexcept
        {
            auto result = allocator_.stats();
            if (result.enabled && L_)
                for (int i{}; i < LUA_GCPN; ++i) result.gc_parameters[i] = lua_gc(L_, LUA_GCPARAM, i, -1);
            return result;
        }

        ~ScriptEngineImpl()
        {
            if (L_)
            {
                lua_close(L_);
            }
        }

        lua_State* state() const
        {
            return L_;
        }

        void setErrorCallback(ScriptEngine::ErrorHandler cb)
        {
            on_error_ = std::move(cb);
        }

        /// Parses the source code and returns a registry ref
        std::optional<int> parseScript(std::string_view code)
        {
            if (!L_)
                return std::nullopt;
            if (luaL_loadbufferx(L_, code.data(), code.size(), "blueprint", "t") != LUA_OK)
            {
                reportAndPopError();
                return std::nullopt;
            }
            return luaL_ref(L_, LUA_REGISTRYINDEX);
        }

        /// Runs a registry ref
        bool runScript(const ScriptRef& program)
        {
            if (!L_)
                return false;
            // push traceback(err) closure
            lua_pushlightuserdata(L_, this);
            lua_pushcclosure(L_, &luaTraceback, 1);
            int errfunc = lua_gettop(L_);

            lua_rawgeti(L_, LUA_REGISTRYINDEX, program.ref()); // push func

            const bool succeeded = lua_pcall(L_, 0, 0, errfunc) == LUA_OK;
            if (!succeeded)
                reportAndPopError(); // the error message is already on top of the stack
            lua_remove(L_, errfunc); // pop the traceback closure
            return succeeded;
        }

        ScriptEngineImpl(const ScriptEngineImpl&) = delete;
        ScriptEngineImpl& operator=(const ScriptEngineImpl&) = delete;

    private:
        static int luaTraceback(lua_State* L)
        {
            const char* msg = lua_tostring(L, 1);
            auto* self = static_cast<ScriptEngineImpl*>(lua_touserdata(L, lua_upvalueindex(1)));
            if (self && self->on_error_)
                self->on_error_(msg ? msg : "(no msg)");
            luaL_traceback(L, L, msg, 1);
            return 1;
        }

        void reportAndPopError()
        {
            if (on_error_)
                on_error_(lua_tostring(L_, -1));
            lua_pop(L_, 1);
        }

        LuaPageAllocator allocator_; // Construct before VM; destroy after lua_close.
        lua_State* L_ = nullptr;
        ScriptEngine::ErrorHandler on_error_;
    };
}
