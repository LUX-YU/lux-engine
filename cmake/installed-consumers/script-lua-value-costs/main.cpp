#include <lux/engine/simulation/scripting/lua/LuaScriptBackend.hpp>
#if SR5_VALUES
#include "CollisionValue.lua.value.generated.hpp"
#else
#include "CollisionValue.hpp"
#endif
#include <lua.hpp>
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string_view>
struct Allocation
{
    bool diagnostics{};
    std::size_t allocations{};
    std::size_t bytes{};
    static void* allocate(void* context, void* pointer, std::size_t old_size, std::size_t size)
    {
        auto& self = *static_cast<Allocation*>(context);
        if (!size) { std::free(pointer); return nullptr; }
        if (self.diagnostics && (!pointer || size > old_size))
        { ++self.allocations; self.bytes += pointer ? size - old_size : size; }
        return std::realloc(pointer, size);
    }
};
#if !SR5_VALUES
// Exact operation from the entry collision_event_chain_test.cpp, including its old error boundary.
static bool pushCollision(void*, void* opaque_state, const void* opaque_value) noexcept
{
    auto* state = static_cast<lua_State*>(opaque_state);
    const auto& collision = *static_cast<const CollisionEvent*>(opaque_value);
    lua_createtable(state, 0, 2);
    lua_pushinteger(state, collision.body);
    lua_setfield(state, -2, "body");
    lua_pushnumber(state, collision.impulse);
    lua_setfield(state, -2, "impulse");
    return true;
}
#endif
int main(int argc, char** argv)
{
    const bool diagnostics = argc > 1 && std::string_view{argv[1]} == "diagnostics";
    Allocation allocation;
    auto* state = lua_newstate(Allocation::allocate, &allocation);
    assert(state);
#if SR5_VALUES
    assert(lux::script::lua::detail::LuaValueAccess::initialize(state));
    const auto operation = lux::script::lua::makeLuaValueOperation<CollisionEvent>();
#else
    const lux::simulation::script::LuaRecordMarshaller operation{
        lux::semantic::typeId("lux.physics.CollisionEvent"), "lux.physics.CollisionEvent",
        sizeof(CollisionEvent), alignof(CollisionEvent), nullptr, &pushCollision};
#endif
    const CollisionEvent value{7, 2.5f};
    const auto push = [&]() noexcept {
        lua_settop(state, 0);
#if SR5_VALUES
        assert(operation.push(state, &value));
#else
        assert(operation.push(operation.context, state, &value));
#endif
        assert(lua_gettop(state) == 1);
    };
    for (unsigned i{}; i < 1000; ++i) push();
    allocation.diagnostics = diagnostics;
    constexpr std::size_t count = 100000;
    const auto start = std::chrono::steady_clock::now();
    for (std::size_t i{}; i < count; ++i) push();
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        std::chrono::steady_clock::now() - start).count();
    allocation.diagnostics = false;
    lua_getfield(state, 1, "body"); lua_getfield(state, 1, "impulse");
    assert(lua_tointeger(state, -2) == 7 && lua_tonumber(state, -1) == 2.5);
    lua_close(state);
    std::printf("VALUE_PUSH count=%zu warmup=1000 ns=%lld fields=2 errors=0 backlog=0 diagnostics=%d allocations=%zu bytes=%zu\n",
        count, elapsed, diagnostics, allocation.allocations, allocation.bytes);
}
