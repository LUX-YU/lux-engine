#include <lux/engine/function/script/lua/LuaVm.hpp>
#include <lux/engine/function/script/lua/detail/LuaVmCompatibility.hpp>

#include <lua.hpp>

#include <cassert>
#include <cstdio>
#include <string_view>

namespace
{
    int yieldForResume(lua_State* state)
    {
        assert(lua_gettop(state) == 0);
        return lux::script::lua::detail::yieldLuaInvocation(state, 0);
    }

    int yieldPair(lua_State* state)
    {
        assert(lua_gettop(state) == 2 && lua_tointeger(state, 1) == 11 && lua_tointeger(state, 2) == 12);
        return lux::script::lua::detail::yieldLuaInvocation(state, 0);
    }

    void testMultipleResults(lua_State* state)
    {
        const auto base = lua_gettop(state);
        lua_pushcfunction(state, &yieldPair);
        lua_setglobal(state, "engine_pair");
        assert(luaL_loadstring(state,
            "return function() local sum=0; for i=1,3 do local a,b=engine_pair(11,12); "
            "assert(a==8 and b==9); sum=sum+a+b end; return sum,22,33 end") == LUA_OK);
        assert(lua_pcall(state, 0, 1, 0) == LUA_OK);
        auto* thread = lua_newthread(state);
        const auto reference = luaL_ref(state, LUA_REGISTRYINDEX);
        lua_xmove(state, thread, 1);
        auto result = lux::script::lua::detail::resumeLuaVm(thread, state, 0);
        int suspended_top{};
        for (int suspension{}; suspension != 3; ++suspension)
        {
            assert(result.status == LUA_YIELD && result.result_count == 0);
            if (suspension == 0) suspended_top = lua_gettop(thread);
            assert(lua_gettop(thread) == suspended_top);
            lua_pushinteger(thread, 8);
            lua_pushinteger(thread, 9);
            result = lux::script::lua::detail::resumeLuaVm(thread, state, 2);
        }
        assert(result.status == LUA_OK && result.result_count == 3 && lua_gettop(thread) == 3);
        assert(lua_tointeger(thread, 1) == 51 && lua_tointeger(thread, 2) == 22 && lua_tointeger(thread, 3) == 33);
        luaL_unref(state, LUA_REGISTRYINDEX, reference);
        assert(lua_gettop(state) == base);
        std::puts("LUA_RESUME_BASE,yields=3,input_args=2,resume_args=2,result_count=3,values=51/22/33,stack_growth=0");
    }

    struct BootstrapAllocator final
    {
        lua_Alloc original{};
        void* context{};
        std::size_t failures{};
        static void* denyGrowth(void* opaque, void* pointer, std::size_t old_size, std::size_t size) noexcept
        {
            auto& self = *static_cast<BootstrapAllocator*>(opaque);
            if (size != 0U && (pointer == nullptr || size > old_size))
            {
                ++self.failures;
                return nullptr;
            }
            return self.original(self.context, pointer, old_size, size);
        }
    };

    void testProtectedBootstrap(lua_State* state)
    {
        const auto top = lua_gettop(state);
        BootstrapAllocator allocation;
        allocation.original = lua_getallocf(state, &allocation.context);
        lua_setallocf(state, &BootstrapAllocator::denyGrowth, &allocation);
        const auto status = lux::script::lua::detail::bootstrapLuaOperation(state, [](lua_State* inner) {
            lua_createtable(inner, 0, 8);
            return 0;
        }, nullptr);
        lua_setallocf(state, allocation.original, allocation.context);
        assert(status == LUA_ERRMEM && allocation.failures != 0U && lua_gettop(state) == top);
        bool recovered{};
        assert(lux::script::lua::detail::bootstrapLuaOperation(state, [](lua_State* inner) {
            auto* result = static_cast<bool*>(lua_touserdata(inner, 1));
            lua_createtable(inner, 0, 8);
            *result = true;
            return 0;
        }, &recovered) == LUA_OK);
        assert(recovered && lua_gettop(state) == top);
        std::printf("BOOTSTRAP_OOM,failures=%zu,stack_delta=0,recovery=1\n", allocation.failures);
    }
}

int main(int argc, char** argv)
{
    const bool interpreter_only = argc == 2 && std::string_view{argv[1]} == "--interpreter-only";
    lua_State* state = luaL_newstate();
    assert(state != nullptr);
    luaL_openlibs(state);
    testProtectedBootstrap(state);
    testMultipleResults(state);

    lux::script::lua::LuaRuntimeInfo runtime;
    assert(lux::script::lua::detail::configureLuaVm(
        state,
        interpreter_only
            ? lux::script::lua::ELuaExecutionPolicy::INTERPRETER_ONLY
            : lux::script::lua::ELuaExecutionPolicy::DEFAULT,
        runtime
    ));
    assert(!runtime.vm.empty() && !runtime.version.empty());
    assert(runtime.jit_available || !runtime.jit_enabled);
    assert(!interpreter_only || !runtime.jit_enabled);

    lua_pushcfunction(state, &yieldForResume);
    lua_setglobal(state, "engine_wait");
    assert(luaL_loadstring(
        state,
        "return function() local value = engine_wait(); return value + 1 end"
    ) == LUA_OK);
    assert(lua_pcall(state, 0, 1, 0) == LUA_OK);
    assert(lua_isfunction(state, -1));

    lua_State* thread = lua_newthread(state);
    assert(thread != nullptr);
    const int thread_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    assert(thread_ref != LUA_NOREF && thread_ref != LUA_REFNIL);
    lua_xmove(state, thread, 1);

    lua_gc(state, LUA_GCCOLLECT, 0);
    const auto suspended = lux::script::lua::detail::resumeLuaVm(thread, nullptr, 0);
    assert(suspended.status == LUA_YIELD && suspended.result_count == 0);
    assert(lua_status(thread) == LUA_YIELD);

    lua_pushnumber(thread, 41.0);
    const auto completed = lux::script::lua::detail::resumeLuaVm(thread, nullptr, 1);
    assert(completed.status == LUA_OK && completed.result_count == 1);
    assert(lua_tonumber(thread, -1) == 42.0);

    lua_settop(thread, 0);
    luaL_unref(state, LUA_REGISTRYINDEX, thread_ref);
    lua_gc(state, LUA_GCCOLLECT, 0);
    // A script can retain coroutine.running() both directly and through a closure. Retiring the
    // backend registry reference does not make that thread available for another invocation.
    assert(luaL_loadstring(state, R"lua(
        retained = {}
        function remember()
            local self = coroutine.running()
            retained[#retained + 1] = self
            retained_closure = function() return self end
            coroutine.yield()
            return 7
        end
    )lua") == LUA_OK);
    assert(lua_pcall(state, 0, 0, 0) == LUA_OK);
    auto* retained_thread = lua_newthread(state);
    const auto retained_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_getglobal(retained_thread, "remember");
    assert(lux::script::lua::detail::resumeLuaVm(retained_thread, nullptr, 0).status == LUA_YIELD);
    assert(lux::script::lua::detail::resumeLuaVm(retained_thread, nullptr, 0).status == LUA_OK);
    lua_settop(retained_thread, 0);
    luaL_unref(state, LUA_REGISTRYINDEX, retained_ref);
    lua_gc(state, LUA_GCCOLLECT, 0);
    assert(luaL_loadstring(state,
        "assert(coroutine.status(retained[1]) == 'dead'); assert(retained_closure() == retained[1])") == LUA_OK);
    assert(lua_pcall(state, 0, 0, 0) == LUA_OK);

    auto* fresh_thread = lua_newthread(state);
    const auto fresh_ref = luaL_ref(state, LUA_REGISTRYINDEX);
    lua_getglobal(fresh_thread, "remember");
    assert(lux::script::lua::detail::resumeLuaVm(fresh_thread, nullptr, 0).status == LUA_YIELD);
    assert(luaL_loadstring(state,
        "assert(retained[1] ~= retained[2]); assert(coroutine.status(retained[1]) == 'dead'); "
        "assert(coroutine.status(retained[2]) == 'suspended')") == LUA_OK);
    assert(lua_pcall(state, 0, 0, 0) == LUA_OK);
    // Cancellation releases the backend reference, while Lua reachability still retains the thread.
    lua_settop(fresh_thread, 0);
    luaL_unref(state, LUA_REGISTRYINDEX, fresh_ref);
    lua_gc(state, LUA_GCCOLLECT, 0);
    assert(luaL_loadstring(state,
        "assert(type(retained[2]) == 'thread'); assert(retained_closure() == retained[2]); "
        "local t=coroutine.create(function() error('expected') end); "
        "assert(not coroutine.resume(t)); assert(coroutine.status(t)=='dead')") == LUA_OK);
    assert(lua_pcall(state, 0, 0, 0) == LUA_OK);
#if LUA_VERSION_NUM >= 504
    assert(luaL_loadstring(state,
        "closed=0; local t=coroutine.create(function() "
        "local x <close> = setmetatable({}, {__close=function() closed=closed+1 end}); "
        "coroutine.yield() end); assert(coroutine.resume(t)); assert(closed==0); "
        "assert(coroutine.close(t)); assert(closed==1)") == LUA_OK);
    assert(lua_pcall(state, 0, 0, 0) == LUA_OK);
#endif
#if LUA_VERSION_NUM >= 505
    assert(luaL_loadstring(state, "local global = 1") == LUA_ERRSYNTAX);
    lua_pop(state, 1);
    assert(luaL_loadstring(state, "for i=1,2 do i=3 end") == LUA_ERRSYNTAX);
    lua_pop(state, 1);
    assert(luaL_loadstring(state,
        "local t={['global']=7}; assert(t['global']==7); "
        "assert(tostring(1.0)=='1.0'); assert(tonumber(tostring(1.234567890123456))==1.234567890123456)") == LUA_OK);
    assert(lua_pcall(state, 0, 0, 0) == LUA_OK);
    std::puts("LUA55_LANGUAGE,global_reserved=1,for_readonly=1,quoted_key=1,float_roundtrip=1");
#endif
    std::puts("THREAD_REUSE_REJECTED,retained_after_unref=1,closure=1,identity=1,dead=1,cancel=1,error=1");
    lua_close(state);
    return 0;
}
