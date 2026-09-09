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
            struct Operation final
            {
                int (*execute)(lua_State *, Operation &);
                std::string_view key;
                std::span<const std::string_view> keys;
                LuaValueFailure *failure{};
                bool success{true};
                int count{};
                const LuaCodecPlan* plan{};
                const LuaCodecShape* shape{};
                const void* object{};
                std::span<double> scratch;
                std::array<std::string_view, 33U> path{};
                std::size_t path_size{};
                std::size_t cursor{};
            };
            // Lua55 C errors may jump only over trivial, non-owning callback frames.
            int trampoline(lua_State *state)
            {
                auto *operation = static_cast<Operation *>(lua_touserdata(state, 1));
                return operation->execute(state, *operation);
            }
            bool run(lua_State *state, Operation &operation, int input, int second, int results) noexcept
            {
                const int base = lua_gettop(state);
                // checkstack is the API's non-throwing, fallible stack growth entry.
                if (!lua_checkstack(state, 5))
                    return false;
                lua_pushcfunction(state, trampoline);
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
                lua_pushlstring(state, operation.key.data(), operation.key.size());
                return 1;
            }
            void pushShapeKeys(lua_State* state, const LuaCodecShape& shape);
            void pushOperationKey(lua_State* state, Operation& operation)
            {
                if (operation.shape == nullptr) lua_pushlstring(state, operation.key.data(), operation.key.size());
                else
                {
                    pushShapeKeys(state, *operation.shape);
                    lua_rawgeti(state, -1, static_cast<lua_Integer>(operation.count) + 1);
                    lua_remove(state, -2);
                }
            }
            int readField(lua_State *state, Operation &operation)
            {
                pushOperationKey(state, operation);
                lua_rawget(state, 2);
                return 1;
            }
            int writeField(lua_State *state, Operation &operation)
            {
                pushOperationKey(state, operation);
                lua_pushvalue(state, 3);
                lua_rawset(state, 2);
                return 0;
            }
            void pushShapeKeys(lua_State* state, const LuaCodecShape& shape)
            {
                lua_rawgetp(state, LUA_REGISTRYINDEX, &shape);
                if (!lua_isnil(state, -1)) return;
                lua_pop(state, 1);
                lua_createtable(state, static_cast<int>(shape.keys.size()), 0);
                for (std::size_t i{}; i < shape.keys.size(); ++i)
                {
                    const auto key = shape.keys[i];
                    lua_pushlstring(state, key.data(), key.size());
                    lua_rawseti(state, -2, static_cast<lua_Integer>(i + 1U));
                }
                lua_pushvalue(state, -1);
                lua_rawsetp(state, LUA_REGISTRYINDEX, &shape);
            }
            std::size_t fieldOrdinal(const LuaCodecShape* shape, std::span<const std::string_view> keys,
                const char* key, std::size_t length) noexcept
            {
                if (keys.size() <= 8U || shape == nullptr)
                {
                    for (std::size_t i{}; i < keys.size(); ++i)
                        if (keys[i].size() == length && std::memcmp(keys[i].data(), key, length) == 0) return i;
                }
                else
                {
                    const auto hash = luaFieldHash({key, length});
                    std::size_t first{}, last = shape->lookup.size();
                    while (first < last)
                    {
                        const auto middle = first + (last - first) / 2U;
                        if (shape->lookup[middle].hash < hash) first = middle + 1U;
                        else last = middle;
                    }
                    for (; first < shape->lookup.size() && shape->lookup[first].hash == hash; ++first)
                    {
                        const auto ordinal = shape->lookup[first].ordinal;
                        if (keys[ordinal].size() == length && std::memcmp(keys[ordinal].data(), key, length) == 0)
                            return ordinal;
                    }
                }
                return keys.size();
            }
            bool matchShape(lua_State* state, int table, std::span<const std::string_view> keys,
                const LuaCodecShape* shape, LuaValueFailure& failure)
            {
                if (lua_type(state, table) != LUA_TTABLE) { failure.code = ELuaValueError::TYPE; return false; }
                std::size_t count{};
                std::uint64_t seen{};
                lua_pushnil(state);
                while (lua_next(state, table))
                {
                    if (++count > keys.size() || lua_type(state, -2) != LUA_TSTRING)
                    { failure.code = ELuaValueError::UNKNOWN_FIELD; return false; }
                    std::size_t length{};
                    const char* key = lua_tolstring(state, -2, &length);
                    const auto ordinal = fieldOrdinal(shape, keys, key, length);
                    if (ordinal == keys.size()) { failure.code = ELuaValueError::UNKNOWN_FIELD; return false; }
                    seen |= std::uint64_t{1U} << ordinal;
                    lua_pop(state, 1);
                }
                for (std::size_t i{}; i < keys.size(); ++i)
                    if ((seen & (std::uint64_t{1U} << i)) == 0U)
                    {
                        failure.code = ELuaValueError::MISSING_FIELD;
                        failure.prepend(keys[i]);
                        return false;
                    }
                return true;
            }
            int checkShape(lua_State* state, Operation& operation)
            {
                if (operation.shape != nullptr) { pushShapeKeys(state, *operation.shape); lua_pop(state, 1); }
                operation.success = matchShape(state, 2, operation.keys, operation.shape, *operation.failure);
                return 0;
            }
            void preparePlan(lua_State* state, const LuaCodecPlan& plan)
            {
                if (plan.shape == nullptr) return;
                pushShapeKeys(state, *plan.shape);
                lua_pop(state, 1);
                for (const auto& field : plan.fields) preparePlan(state, field.plan());
            }
            int prepareOperation(lua_State* state, Operation& operation)
            {
                if (operation.plan != nullptr) preparePlan(state, *operation.plan);
                else { pushShapeKeys(state, *operation.shape); lua_pop(state, 1); }
                return 0;
            }
            bool pushPlain(lua_State* state, Operation& operation, const LuaCodecPlan& plan,
                const void* object, std::size_t depth)
            {
                if (plan.kind != ELuaPlainKind::RECORD)
                {
                    const auto number = plan.read(object);
                    if (!number.valid)
                    { operation.failure->code = ELuaValueError::RANGE; return false; }
                    if (plan.kind == ELuaPlainKind::BOOLEAN) lua_pushboolean(state, number.value != 0.0);
                    else lua_pushnumber(state, number.value);
                    return true;
                }
                pushShapeKeys(state, *plan.shape);
                const int keys = lua_gettop(state);
                lua_createtable(state, 0, static_cast<int>(plan.fields.size()));
                const int table = lua_gettop(state);
                for (std::size_t i{}; i < plan.fields.size(); ++i)
                {
                    const auto& field = plan.fields[i];
                    operation.path[depth] = field.name;
                    operation.path_size = depth + 1U;
                    // Resolve the member now, after preceding allocations and callbacks.
                    if (!pushPlain(state, operation, field.plan(), field.member(object), depth + 1U)) return false;
                    operation.path_size = depth + 1U;
                    lua_rawgeti(state, keys, static_cast<lua_Integer>(i + 1U));
                    lua_insert(state, -2);
                    lua_rawset(state, table);
                }
                lua_remove(state, keys);
                return true;
            }
            bool readPlain(lua_State* state, Operation& operation, const LuaCodecPlan& plan,
                int value, std::size_t depth)
            {
                const auto slot = operation.cursor++;
                if (slot >= operation.scratch.size()) { operation.failure->code = ELuaValueError::CAPACITY; return false; }
                if (plan.kind != ELuaPlainKind::RECORD)
                {
                    const bool boolean = plan.kind == ELuaPlainKind::BOOLEAN;
                    if (lua_type(state, value) != (boolean ? LUA_TBOOLEAN : LUA_TNUMBER))
                    { operation.failure->code = ELuaValueError::TYPE; return false; }
                    const double number = boolean ? static_cast<double>(lua_toboolean(state, value)) : lua_tonumber(state, value);
                    if (!plan.validate(number)) { operation.failure->code = ELuaValueError::RANGE; return false; }
                    operation.scratch[slot] = number;
                    return true;
                }
                if (!matchShape(state, value, plan.shape->keys, plan.shape, *operation.failure)) return false;
                pushShapeKeys(state, *plan.shape);
                const int keys = lua_gettop(state);
                for (std::size_t i{}; i < plan.fields.size(); ++i)
                {
                    const auto& field = plan.fields[i];
                    operation.path[depth] = field.name;
                    operation.path_size = depth + 1U;
                    lua_rawgeti(state, keys, static_cast<lua_Integer>(i + 1U));
                    lua_rawget(state, value);
                    if (!readPlain(state, operation, field.plan(), lua_gettop(state), depth + 1U)) return false;
                    lua_pop(state, 1);
                }
                lua_pop(state, 1);
                return true;
            }
            int writePlain(lua_State* state, Operation& operation)
            {
                operation.success = pushPlain(state, operation, *operation.plan, operation.object, 0U);
                return operation.success ? 1 : 0;
            }
            int readPlainOperation(lua_State* state, Operation& operation)
            {
                operation.success = readPlain(state, operation, *operation.plan, 2, 0U);
                return 0;
            }
            LuaValueResult<void> planResult(lua_State* state, Operation& operation, int input, int results) noexcept
            {
                LuaValueFailure failure{ELuaValueError::VM_FAILURE};
                operation.failure = &failure;
                if (operation.plan->stack <= 136U && lua_checkstack(state, static_cast<int>(operation.plan->stack)) &&
                    run(state, operation, input, 0, results)) return {};
                for (auto i = operation.path_size; i > 0U; --i) failure.prepend(operation.path[i - 1U]);
                return lux::cxx::unexpected(failure);
            }
        } // namespace
        bool LuaValueAccess::initialize(lua_State* state) noexcept
        {
            return state != nullptr && lua_checkstack(state, 136) != 0;
        }
        bool LuaValueAccess::prepare(lua_State* state, const LuaCodecPlan& plan) noexcept
        {
            Operation operation{prepareOperation};
            operation.plan = &plan;
            return lua_checkstack(state, static_cast<int>(plan.stack)) && run(state, operation, 0, 0, 0);
        }
        bool LuaValueAccess::prepareShape(lua_State* state, const LuaCodecShape& shape) noexcept
        {
            Operation operation{prepareOperation};
            operation.shape = &shape;
            return run(state, operation, 0, 0, 0);
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
        LuaValueResult<void> LuaValueAccess::writePlan(lua_State* state, const LuaCodecPlan& plan,
            const void* object) noexcept
        {
            Operation operation{writePlain};
            operation.plan = &plan;
            operation.object = object;
            return planResult(state, operation, 0, 1);
        }
        LuaValueResult<void> LuaValueAccess::readPlan(lua_State* state, int index, const LuaCodecPlan& plan,
            std::span<double> scratch) noexcept
        {
            Operation operation{readPlainOperation};
            operation.plan = &plan;
            operation.scratch = scratch;
            return planResult(state, operation, index, 0);
        }
        int LuaValueAccess::failure(lua_State *state, const char *message) noexcept
        {
            Operation operation{failureValues};
            operation.key = message;
            if (run(state, operation, 0, 0, 1)) return 1;
            // The C boundary formats the status after all C++ owners have returned.
            return 0;
        }
        LuaValueResult<void> LuaValueAccess::shape(lua_State *state, int index,
                                                   std::span<const std::string_view> keys, const LuaCodecShape* plan) noexcept
        {
            if (keys.size() > 64U)
                return lux::cxx::unexpected(LuaValueFailure{ELuaValueError::CAPACITY});
            LuaValueFailure failure;
            Operation operation{checkShape};
            operation.keys = keys;
            operation.shape = plan;
            operation.failure = &failure;
            if (run(state, operation, index, 0, 0))
                return {};
            if (operation.success)
                failure.code = ELuaValueError::VM_FAILURE;
            return lux::cxx::unexpected(failure);
        }
        bool LuaValueAccess::field(lua_State* state, int index, std::string_view key,
            const LuaCodecShape* plan, std::size_t ordinal) noexcept
        {
            Operation operation{readField};
            operation.key = key;
            operation.shape = plan;
            operation.count = static_cast<int>(ordinal);
            return run(state, operation, index, 0, 1);
        }
        bool LuaValueAccess::setField(lua_State* state, int index, std::string_view key,
            const LuaCodecShape* plan, std::size_t ordinal) noexcept
        {
            Operation operation{writeField};
            operation.key = key;
            operation.shape = plan;
            operation.count = static_cast<int>(ordinal);
            return run(state, operation, index, lua_gettop(state), 0);
        }
        void LuaValueAccess::restoreScratch(lua_State *state, int top) noexcept
        {
            lua_settop(state, top);
        }
    } // namespace detail
} // namespace lux::script::lua
