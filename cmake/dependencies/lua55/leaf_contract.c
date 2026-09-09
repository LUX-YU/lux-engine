#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#define CHECK(test) do { if (!(test)) { fprintf(stderr,"FAIL line=%d %s\n",__LINE__,#test); exit(1); } } while (0)
static int finish(lua_State* L,int status,lua_KContext base) { (void)status; return lua_gettop(L)-(int)base; }
static int leaf(lua_State* L) { return luxlua_yieldleaf(L,lua_gettop(L),finish); }
static int afterCall(lua_State* L,int status,lua_KContext ctx) { (void)status;(void)ctx;return lua_gettop(L); }
static int reenter(lua_State* L) { lua_getglobal(L,"wait");lua_callk(L,0,LUA_MULTRET,0,afterCall);return lua_gettop(L); }
static void run(lua_State* L,const char* name,const char* code,int count,int fast,int fail,int close_early) {
    lua_State* T; int ref,n,status,i; unsigned long long a,b;
    CHECK(luaL_loadstring(L,code)==LUA_OK);CHECK(lua_pcall(L,0,1,0)==LUA_OK);
    T=lua_newthread(L);ref=luaL_ref(L,LUA_REGISTRYINDEX);lua_xmove(L,T,1);
    status=lua_resume(T,L,0,&n);
    for(i=0;i<count;i++) {
        CHECK(status==LUA_YIELD && n==0 && lua_status(T)==LUA_YIELD);
        if(close_early) break;
        lua_pushinteger(T,42);status=lua_resume(T,L,1,&n);
    }
    luxlua_leafstats(T,&a,&b);
    CHECK(fast ? a==(unsigned long long)(close_early?1:count) && b==0 : a==0 && b>0);
    if(close_early) CHECK(lua_closethread(T,L)==LUA_OK);
    else if(fail) CHECK(status==LUA_ERRRUN && strstr(lua_tostring(T,-1),"after")!=NULL);
    else CHECK(status==LUA_OK && n==1 && lua_tointeger(T,-1)==42);
    printf("LEAF_CASE,%s,fast=%llu,fallback=%llu,resumes=%d,status=%d,close=%d\n",name,a,b,i,status,close_early);
    luaL_unref(L,LUA_REGISTRYINDEX,ref);lua_settop(L,0);
}
int main(void) {
    lua_State* L=luaL_newstate();CHECK(L);luaL_openlibs(L);
    lua_pushcfunction(L,leaf);lua_setglobal(L,"wait");lua_pushcfunction(L,reenter);lua_setglobal(L,"reenter");
    run(L,"direct","return function() local v=wait(); return v end",1,1,0,0);
    run(L,"nested-vararg","local function f(...) local v=wait(); return v end return function() local x=f(1,2);return x end",1,1,0,0);
    run(L,"repeated","return function() local x=0 for i=1,1000 do x=wait() end return x end",1000,1,0,0);
    run(L,"pcall","return function() local ok,x=pcall(function() local x=wait(); return x end);assert(ok);return x end",1,0,0,0);
    run(L,"xpcall","return function() local ok,x=xpcall(function() local x=wait();return x end,debug.traceback);assert(ok);return x end",1,0,0,0);
    run(L,"tail","return function() return wait() end",1,0,0,0);
    run(L,"metamethod","local t=setmetatable({},{__call=function() local x=wait();return x end});return function() local x=t();return x end",1,0,0,0);
    run(L,"iterator","return function() for x in function() local v=wait();return v end do return x end end",1,0,0,0);
    run(L,"hook","return function() debug.sethook(function() end,'',1);local x=wait();debug.sethook();return x end",1,0,0,0);
    run(L,"C-reentry","return function() local x=reenter();return x end",1,0,0,0);
    run(L,"close-normal","return function() local c <close> = setmetatable({},{__close=function() closed=(closed or 0)+1 end});local x=wait();return x end",1,0,0,0);
    run(L,"close-cancel","return function() local c <close> = setmetatable({},{__close=function() closed=(closed or 0)+1 end});local x=wait();return x end",1,0,0,1);
    run(L,"fast-cancel","return function() local x=wait();return x end",1,1,0,1);
    run(L,"error-after","return function() local x=wait();error('after resume') end",1,1,1,0);
    CHECK(luaL_dostring(L,"assert(closed==2);local c=coroutine.create(function() coroutine.yield(7);return 8 end);local ok,x=coroutine.resume(c);assert(ok and x==7);ok,x=coroutine.resume(c);assert(ok and x==8)")==LUA_OK);
    lua_close(L);puts("LEAF_CONTRACT_PASS,cases=14,standard_coroutine=1,close_count=2");return 0;
}
