#include <lux_lua55_extensions.h>
#include <lauxlib.h>
#include <lualib.h>
#include "lstate.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define CHECK(x) do { if (!(x)) { fprintf(stderr,"CI_FAIL line=%d %s\n",__LINE__,#x); exit(1); } } while (0)
typedef struct Memory { size_t live, allocations, frees, fail_size, rejected; } Memory;
static void* allocation(void* opaque, void* pointer, size_t old, size_t size) {
    Memory* m=(Memory*)opaque; void* result;
    if(!pointer) old=0;
    if(!size) { if(pointer) { m->live-=old;m->frees++;free(pointer); } return NULL; }
    if(m->fail_size && size==m->fail_size) { m->rejected++;return NULL; }
    result=realloc(pointer,size);
    if(result) { m->live=m->live-old+size;if(!pointer) m->allocations++; }
    return result;
}
static LX* storage(lua_State* L) { return (LX*)((unsigned char*)L - offsetof(LX,l)); }
static int inlineCount(lua_State* L) {
    unsigned mask=storage(L)->inline_ci_linked_mask;
    CHECK((mask&~3u)==0);return (int)((mask&1u)+((mask>>1)&1u));
}
static void checkLinks(lua_State* T) {
    CallInfo* previous=&T->base_ci;CallInfo* ci=previous->next;int count=0,local=0;
    for(;ci;ci=ci->next) {
        CHECK(ci->previous==previous);
        if(ci==&storage(T)->inline_ci[0]) { CHECK(storage(T)->inline_ci_linked_mask&1u);local++; }
        if(ci==&storage(T)->inline_ci[1]) { CHECK(storage(T)->inline_ci_linked_mask&2u);local++; }
        previous=ci;count++;
    }
    CHECK(count==T->nci && local==inlineCount(T));
}
static int finish(lua_State* L,int status,lua_KContext context) {
    (void)status;(void)context;return lua_gettop(L);
}
static int waitLeaf(lua_State* L) { return luxlua_yieldleaf(L,0,finish); }
static int createThread(lua_State* L) { lua_newthread(L);return 1; }
static void workload(lua_State* L,int depth) {
    lua_State* T=lua_newthread(L);int n,status,before;char code[512];
    CHECK(T->nci==0 && inlineCount(T)==0);
    if(depth==0) strcpy(code,"return 42");
    else snprintf(code,sizeof(code),
        "local function f(n) if n==0 then local x=wait();return x end local x=f(n-1);return x end "
        "local v=f(%d);return v",depth-1);
    CHECK(luaL_loadstring(T,code)==LUA_OK);
    status=lua_resume(T,L,0,&n);checkLinks(T);
    if(depth) {
        CHECK(status==LUA_YIELD);CHECK(inlineCount(T)==2);
        if(depth>1) CHECK(T->nci>2);
        lua_pushinteger(T,42);status=lua_resume(T,L,1,&n);
    }
    CHECK(status==LUA_OK && n==1 && lua_tointeger(T,-1)==42);checkLinks(T);
    before=T->nci;
    lua_gc(L,LUA_GCCOLLECT);checkLinks(T);CHECK(T->nci<=before);
    CHECK(lua_closethread(T,L)==LUA_OK);checkLinks(T);
    CHECK(lua_closethread(T,L)==LUA_OK);checkLinks(T);
    CHECK(luaL_loadstring(T,"local x=wait();return x")==LUA_OK);
    CHECK(lua_resume(T,L,0,&n)==LUA_YIELD);lua_pushinteger(T,42);
    CHECK(lua_resume(T,L,1,&n)==LUA_OK && lua_tointeger(T,-1)==42);checkLinks(T);
    printf("CI_DEPTH,depth=%d,initial_ci=%d,after_shrink=%d,inline=%d,reset_reuse=1\n",
        depth,before,T->nci,inlineCount(T));
    lua_pop(L,1);lua_gc(L,LUA_GCCOLLECT);
}
static void allocationFailures(lua_State* L,Memory* memory) {
    size_t sizes[2]={sizeof(LX),(BASIC_STACK_SIZE+EXTRA_STACK)*sizeof(StackValue)};int i,n;
    for(i=0;i<2;i++) {
        size_t before=memory->rejected;
        lua_pushcfunction(L,createThread);memory->fail_size=sizes[i];
        CHECK(lua_pcall(L,0,1,0)==LUA_ERRMEM);memory->fail_size=0;
        CHECK(memory->rejected>before);lua_pop(L,1);lua_gc(L,LUA_GCCOLLECT);
        lua_pushcfunction(L,createThread);CHECK(lua_pcall(L,0,1,0)==LUA_OK);lua_pop(L,1);
        printf("CI_OOM,layer=%s,rejected=%zu,recovered=1\n",i?"stack":"thread",memory->rejected-before);
    }
    {
        lua_State* T=lua_newthread(L);size_t before=memory->rejected;
        CHECK(luaL_loadstring(T,
            "local function f(n) if n==0 then return 42 end local x=f(n-1);return x end return f(16)")==LUA_OK);
        memory->fail_size=sizeof(CallInfo);CHECK(lua_resume(T,L,0,&n)==LUA_ERRMEM);
        memory->fail_size=0;CHECK(memory->rejected>before);checkLinks(T);
        CHECK(lua_closethread(T,L)==LUA_ERRMEM);lua_settop(T,0);
        CHECK(luaL_loadstring(T,"return 42")==LUA_OK);CHECK(lua_resume(T,L,0,&n)==LUA_OK);
        CHECK(lua_tointeger(T,-1)==42);lua_pop(L,1);lua_gc(L,LUA_GCCOLLECT);
        printf("CI_OOM,layer=heap_overflow,rejected=%zu,recovered=1\n",memory->rejected-before);
    }
}
int main(void) {
    Memory memory={0};unsigned long long local,heap;int observed,i;
    lua_State* L=lua_newstate(allocation,&memory,1592598566);CHECK(L);luaL_openlibs(L);
    lua_pushcfunction(L,waitLeaf);lua_setglobal(L,"wait");
    printf("CI_LAYOUT,LX=%zu,state=%zu,CI=%zu,stack=%zu,inline_capacity=2\n",
        sizeof(LX),sizeof(lua_State),sizeof(CallInfo),(BASIC_STACK_SIZE+EXTRA_STACK)*sizeof(StackValue));
    for(i=0;i<=3;i++) workload(L,i);
    workload(L,32);allocationFailures(L,&memory);
    CHECK(luaL_dostring(L,
        "local saved={} for i=1,2 do local c=coroutine.create(function() local me=coroutine.running();"
        "saved[#saved+1]=me;for j=1,3 do local x=wait();assert(x==42) end return me end);"
        "for j=1,4 do assert(coroutine.resume(c,42)) end assert(saved[i]==c) end "
        "assert(saved[1]~=saved[2]);collectgarbage();assert(coroutine.status(saved[1])=='dead')")==LUA_OK);
    lua_gc(L,LUA_GCCOLLECT);checkLinks(L);CHECK((size_t)gettotalbytes(G(L))==memory.live);
    observed=luxlua_vmcallinfostats(L,&local,&heap);
    if(observed) CHECK(local>0 && heap>0);
    printf("CI_COUNTERS,observed=%d,inline=%llu,heap=%llu,accounting_matches=1\n",observed,local,heap);
    lua_close(L);CHECK(memory.live==0 && memory.allocations==memory.frees);
    puts("CI_CONTRACT_PASS,closed_live=0,identity=1,shrink=1,reset=1,oom_layers=3");return 0;
}
