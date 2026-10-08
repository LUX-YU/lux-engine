#include <lux/engine/function/script/lua/Lua.hpp>
#include <lux/engine/function/script/lua/LuaBoundary.h>

#include <lua.hpp>

#include <cassert>

namespace
{
    enum class EFailure
    {
        NONE,
        CREATE,
        BOOTSTRAP
    };
    EFailure failure{};
    lua_Alloc original_allocate{};
    void* original_context{};
    unsigned live_blocks{}, created{}, closed{};

    void* allocation(void*, void* pointer, std::size_t old_size, std::size_t size) noexcept
    {
        auto* result = original_allocate(original_context, pointer, old_size, size);
        if (pointer == nullptr && result != nullptr)
        {
            ++live_blocks;
        }
        if (pointer != nullptr && size == 0)
        {
            --live_blocks;
        }
        return result;
    }

    lua_State* createState(lua_Alloc allocate, void* context, unsigned seed)
    {
        if (failure == EFailure::CREATE)
        {
            return nullptr;
        }
        original_allocate = allocate;
        original_context = context;
        auto* state = lua_newstate(&allocation, nullptr, seed);
        if (state)
        {
            ++created;
        }
        return state;
    }

    int bootstrap(lua_State* state)
    {
        if (failure == EFailure::BOOTSTRAP)
        {
            return luaL_error(state, "injected bootstrap rejection");
        }
        return luxLuaBootstrap(state);
    }

    void closeState(lua_State* state)
    {
        lua_close(state);
        ++closed;
        assert(live_blocks == 0);
    }
} // namespace

// Compile the actual private owner, substituting only its native boundary calls.
// clang-format off: declarations and remapping must precede the production header.
#define lua_newstate createState
#define lua_close closeState
#define luxLuaBootstrap bootstrap
#include <lux/engine/function/script/lua/LuaImpl.hpp>
#undef luxLuaBootstrap
#undef lua_close
#undef lua_newstate
// clang-format on

int main()
{
    using namespace lux::script::lua;
    for (unsigned iteration{}; iteration < 32; ++iteration)
    {
        failure = EFailure::CREATE;
        auto absent = ScriptEngineImpl::create({});
        assert(!absent && absent.error() == ELuaEngineError::VM_CREATION_FAILURE);
        assert(created == closed && live_blocks == 0);
        failure = EFailure::BOOTSTRAP;
        auto rejected = ScriptEngineImpl::create({});
        assert(!rejected && rejected.error() == ELuaEngineError::BOOTSTRAP_FAILURE);
        assert(created == closed && live_blocks == 0);
        failure = EFailure::NONE;
        {
            auto complete = ScriptEngineImpl::create({});
            assert(complete && (*complete)->state());
            assert(created == closed + 1 && live_blocks > 0);
        }
        assert(created == closed && live_blocks == 0);
    }
}
