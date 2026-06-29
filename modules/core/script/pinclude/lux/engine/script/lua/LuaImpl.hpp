#pragma once
#include <string_view>
#include <lux/engine/script/lua/Lua.hpp>

#include <lua.hpp>

#include <lux/engine/meta/Meta.hpp>

namespace lux::script::lua
{
    class ScriptEngineImpl {
    public:
        ScriptEngineImpl() {
            L_ = luaL_newstate();
            luaL_openlibs(L_);
        }

        ~ScriptEngineImpl()
        {
            if (L_)
            {
                lua_close(L_);
            }
        }
    
        lua_State* state() const { return L_; }
    
        void setErrorCallback(std::function<void(std::string_view)> cb) {
            on_error_ = std::move(cb);
        }
    
        /// 解析源码，返回 registry ref
        std::optional<int> parseScript(std::string_view code) {
            if (luaL_loadbufferx(L_, code.data(), code.size(), "blueprint", "t") != LUA_OK)
            {
                reportAndPopError();
                return std::nullopt;
            }
            return luaL_ref(L_, LUA_REGISTRYINDEX);
        }
    
        /// 执行 registry ref
        void runScript(const ScriptRef& program)
        {
            // push traceback(err) closure
            lua_pushlightuserdata(L_, this);
            lua_pushcclosure(L_, &luaTraceback, 1);
            int errfunc = lua_gettop(L_);

            lua_rawgeti(L_, LUA_REGISTRYINDEX, program.ref());   // push func
    
            if (lua_pcall(L_, 0, 0, errfunc) != LUA_OK)
                reportAndPopError(); // error msg 已经在栈顶
            lua_remove(L_, errfunc); // 清掉 traceback 闭包
        }
    
        ScriptEngineImpl(const ScriptEngineImpl&)            = delete;
        ScriptEngineImpl& operator=(const ScriptEngineImpl&) = delete;
    
    private:
        static int luaTraceback(lua_State* L)
        {
            const char* msg = lua_tostring(L, 1);
            auto* self = static_cast<ScriptEngineImpl*>(lua_touserdata(L, lua_upvalueindex(1)));
            if (self && self->on_error_) self->on_error_(msg ? msg : "(no msg)");
            luaL_traceback(L, L, msg, 1);
            return 1;
        }
    
        void reportAndPopError() {
            if(on_error_) on_error_(lua_tostring(L_, -1));
            lua_pop(L_, 1);
        }
    
        lua_State* L_ = nullptr;
        std::function<void(std::string_view)> on_error_;
    };
}