#include <lux/engine/function/script/lua/LuaValue.hpp>
#include <lua.hpp>
#include <algorithm>
#include <cstring>

namespace lux::script::lua
{
    void LuaValueFailure::prepend(std::string_view field) noexcept
    {
        const auto previous = path;
        std::size_t size{};
        for (char c : field)
        {
            if (size == path.size() - 1)
            {
                truncated = true;
                break;
            }
            path[size++] = c;
        }
        if (previous[0] != 0 && size < path.size() - 1)
            path[size++] = '.';
        for (char c : previous)
        {
            if (!c)
                break;
            if (size == path.size() - 1)
            {
                truncated = true;
                break;
            }
            path[size++] = c;
        }
        path[size] = 0;
    }
    namespace detail
    {
        namespace
        {
            char operation_key;
            struct Operation final
            {
                int (*execute)(lua_State *, Operation &);
                std::string_view key;
                std::span<const std::string_view> keys;
                LuaValueFailure failure;
                bool success{true};
                int count{};
            };
            // These C trampolines have no owning C++ locals and deliberately are not noexcept:
            // the selected VM may use either longjmp or C++ unwinding to reach its own pcall.
            int trampoline(lua_State *state)
            {
                auto *operation = static_cast<Operation *>(lua_touserdata(state, 1));
                return operation->execute(state, *operation);
            }
            int initializeTrampoline(lua_State *state)
            {
                lua_pushlightuserdata(state, &operation_key);
                lua_pushcfunction(state, trampoline);
                lua_rawset(state, LUA_REGISTRYINDEX);
                return 0;
            }
            bool run(lua_State *state, Operation &operation, int input, int second, int results) noexcept
            {
                const int base = lua_gettop(state);
                // checkstack is the API's non-throwing, fallible stack growth entry.
                if (!lua_checkstack(state, 5))
                    return false;
                lua_pushlightuserdata(state, &operation_key);
                lua_rawget(state, LUA_REGISTRYINDEX);
                if (!lua_isfunction(state, -1))
                {
                    lua_pop(state, 1);
                    return false;
                }
                lua_pushlightuserdata(state, &operation);
                int count = 1;
                if (input)
                {
                    lua_pushvalue(state, input);
                    ++count;
                }
                if (second)
                {
                    lua_pushvalue(state, second);
                    ++count;
                }
                const int status = lua_pcall(state, count, results, 0);
                if (status != 0 || !operation.success)
                {
                    // The only slots above base are our pcall outputs/error; no to-be-closed slots.
                    lua_settop(state, base);
                    return false;
                }
                return true;
            }
            int makeTable(lua_State *state, Operation &operation)
            {
                lua_createtable(state, 0, operation.count);
                return 1;
            }
            int failureValues(lua_State *state, Operation &operation)
            {
                lua_pushboolean(state, false);
                lua_pushlstring(state, operation.key.data(), operation.key.size());
                return 2;
            }
            int readField(lua_State *state, Operation &operation)
            {
                lua_pushlstring(state, operation.key.data(), operation.key.size());
                lua_rawget(state, 2);
                return 1;
            }
            int writeField(lua_State *state, Operation &operation)
            {
                lua_pushlstring(state, operation.key.data(), operation.key.size());
                lua_pushvalue(state, 3);
                lua_rawset(state, 2);
                return 0;
            }
            int checkShape(lua_State *state, Operation &operation)
            {
                if (lua_type(state, 2) != LUA_TTABLE)
                {
                    operation.success = false;
                    return 0;
                }
                std::size_t count{};
                lua_pushnil(state);
                while (lua_next(state, 2))
                {
                    if (++count > operation.keys.size() || lua_type(state, -2) != LUA_TSTRING)
                    {
                        operation.failure.code = ELuaValueError::UNKNOWN_FIELD;
                        operation.success = false;
                        return 0;
                    }
                    std::size_t length{};
                    const char *key = lua_tolstring(state, -2, &length);
                    bool found{};
                    for (const auto expected : operation.keys)
                        if (expected.size() == length && std::memcmp(expected.data(), key, length) == 0)
                            found = true;
                    if (!found)
                    {
                        operation.failure.code = ELuaValueError::UNKNOWN_FIELD;
                        operation.success = false;
                        return 0;
                    }
                    lua_pop(state, 1);
                }
                // Missing fields are identified in declaration order, independently of table iteration.
                for (const auto key : operation.keys)
                {
                    lua_pushlstring(state, key.data(), key.size());
                    lua_rawget(state, 2);
                    if (lua_isnil(state, -1))
                    {
                        operation.failure.code = ELuaValueError::MISSING_FIELD;
                        operation.failure.prepend(key);
                        operation.success = false;
                        return 0;
                    }
                    lua_pop(state, 1);
                }
                return 0;
            }
        } // namespace
        bool LuaValueAccess::initialize(lua_State *state) noexcept
        {
            if (!state || !lua_checkstack(state, 2))
                return false;
            const auto base = lua_gettop(state);
#if defined(LUX_SCRIPT_LUA_VM_LUAJIT)
            const int status = lua_cpcall(state, initializeTrampoline, nullptr);
#else
            // Lua 5.4's zero-upvalue C function is a light C function (no allocation).
            lua_pushcfunction(state, initializeTrampoline);
            const int status = lua_pcall(state, 0, 0, 0);
#endif
            lua_settop(state, base);
            return status == 0;
        }
        int LuaValueAccess::top(lua_State *state) noexcept
        {
            return lua_gettop(state);
        }
        int LuaValueAccess::absolute(lua_State *state, int index) noexcept
        {
            return index > 0 || index <= LUA_REGISTRYINDEX ? index : lua_gettop(state) + index + 1;
        }
        bool LuaValueAccess::boolean(lua_State *state, int index, bool &result) noexcept
        {
            if (lua_type(state, index) != LUA_TBOOLEAN)
                return false;
            result = lua_toboolean(state, index) != 0;
            return true;
        }
        bool LuaValueAccess::number(lua_State *state, int index, double &result) noexcept
        {
            if (lua_type(state, index) != LUA_TNUMBER)
                return false;
            result = lua_tonumber(state, index);
            return true;
        }
        bool LuaValueAccess::pushBoolean(lua_State *state, bool value) noexcept
        {
            if (!lua_checkstack(state, 1))
                return false;
            lua_pushboolean(state, value);
            return true;
        }
        bool LuaValueAccess::pushNumber(lua_State *state, double value) noexcept
        {
            if (!lua_checkstack(state, 1))
                return false;
            lua_pushnumber(state, value);
            return true;
        }
        bool LuaValueAccess::table(lua_State *state, int fields) noexcept
        {
            Operation operation{makeTable};
            operation.count = fields;
            return run(state, operation, 0, 0, 1);
        }
        int LuaValueAccess::failure(lua_State *state, const char *message) noexcept
        {
            Operation operation{failureValues};
            operation.key = message;
            if (run(state, operation, 0, 0, 2))
                return 2;
            // No allocation is needed for the fallback. The Lua wrapper reports this after the
            // generated C++ conversion frame has been destroyed.
            if (!lua_checkstack(state, 2))
                return 0;
            lua_pushboolean(state, false);
            lua_pushnil(state);
            return 2;
        }
        LuaValueResult<void> LuaValueAccess::shape(lua_State *state, int index,
                                                   std::span<const std::string_view> keys) noexcept
        {
            Operation operation{checkShape};
            operation.keys = keys;
            if (run(state, operation, index, 0, 0))
                return {};
            if (operation.success)
                operation.failure.code = ELuaValueError::VM_FAILURE;
            return lux::cxx::unexpected(operation.failure);
        }
        bool LuaValueAccess::field(lua_State *state, int index, std::string_view key) noexcept
        {
            Operation operation{readField};
            operation.key = key;
            return run(state, operation, index, 0, 1);
        }
        bool LuaValueAccess::setField(lua_State *state, int index, std::string_view key) noexcept
        {
            Operation operation{writeField};
            operation.key = key;
            return run(state, operation, index, lua_gettop(state), 0);
        }
        void LuaValueAccess::restoreScratch(lua_State *state, int top) noexcept
        {
            lua_settop(state, top);
        }
    } // namespace detail
} // namespace lux::script::lua
