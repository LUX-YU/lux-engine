#include <lux/engine/function/script/lua/LuaValue.hpp>
#include <lua.hpp>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>
#include <cmath>
#include <limits>

using namespace lux::script::lua;
struct NamedA
{
    std::int32_t x;
};
struct NamedB
{
    std::int32_t x;
};
namespace lux::semantic
{
    template <> struct TypeTraits<NamedA>
    {
        inline static constexpr std::string_view CanonicalName = "test.semantic.a";
        inline static constexpr std::uint8_t AbiKind = 10;
    };
    template <> struct TypeTraits<NamedB>
    {
        inline static constexpr std::string_view CanonicalName = "test.semantic.b";
        inline static constexpr std::uint8_t AbiKind = 10;
    };
} // namespace lux::semantic
struct Velocity
{
    float x;
    double y;
};
enum class Mode : std::int32_t
{
    WALK = 1,
    RUN = 3
};
struct Pose
{
    const std::int32_t id;
    Velocity velocity;
    Mode mode;
};
struct Angle
{
    float radians;
};
struct AngleHolder
{
    Angle angle;
};
struct LargeValue
{
    char bytes[65536];
};
struct LargePolicy
{
    inline static constexpr std::string_view name = "test.large";
    inline static constexpr unsigned version = 1;
};
template <int N> struct Nested
{
    Nested<N - 1> child;
};
template <> struct Nested<0>
{
    std::int32_t leaf;
};
struct Resource
{
    static inline int live{};
    static inline lua_State* mutation_state{};
    static inline int read_mutations{};
    static inline std::vector<int> released;
    int id;
    Resource() = delete;
    explicit Resource(int value) noexcept : id(value) { ++live; }
    Resource(Resource &&other) noexcept : id(std::exchange(other.id, -1)) {}
    Resource(const Resource &) = delete;
    ~Resource() noexcept
    {
        if (id >= 0)
        {
            --live;
            released.push_back(id);
        }
    }
};
struct ResourceRecord
{
    Resource first;
    std::int32_t later;
};
namespace lux::script::lua
{
    template <>
    struct LuaGeneratedValue<ResourceRecord>
        : LuaRecordValue<ResourceRecord, LuaValueField<&ResourceRecord::first, "first">,
              LuaValueField<&ResourceRecord::later, "later">>
    {
    };
    template <> struct LuaGeneratedValue<NamedA> : LuaRecordValue<NamedA, LuaValueField<&NamedA::x, "x">>
    {
    };
    template <> struct LuaGeneratedValue<NamedB> : LuaRecordValue<NamedB, LuaValueField<&NamedB::x, "x">>
    {
    };
    template <int N>
    struct LuaGeneratedValue<Nested<N>> : LuaRecordValue<Nested<N>, LuaValueField<&Nested<N>::child, "child">>
    {
    };
    template <> struct LuaGeneratedValue<Nested<0>> : LuaRecordValue<Nested<0>, LuaValueField<&Nested<0>::leaf, "leaf">>
    {
    };
    template <>
    struct LuaGeneratedValue<AngleHolder> : LuaRecordValue<AngleHolder, LuaValueField<&AngleHolder::angle, "angle">>
    {
    };
    template <> struct LuaValueOverride<Angle, LargePolicy>
    {
        inline static constexpr std::string_view name = "test.large-angle";
        inline static constexpr unsigned version = 1;
        inline static constexpr std::size_t storage = 65536;
    };
    // Boundary tests precede the formal generator integration. Vertical tests use generated output.
    template <>
    struct LuaGeneratedValue<Velocity>
        : LuaRecordValue<Velocity, LuaValueField<&Velocity::x, "x">, LuaValueField<&Velocity::y, "y">>
    {
    };
    template <> struct LuaGeneratedValue<Mode> : LuaEnumValue<Mode, Mode::WALK, Mode::RUN>
    {
    };
    template <>
    struct LuaGeneratedValue<Pose>
        : LuaRecordValue<Pose, LuaValueField<&Pose::id, "renamed">, LuaValueField<&Pose::velocity, "velocity">,
                         LuaValueField<&Pose::mode, "mode">>
    {
    };
    template <> struct LuaGeneratedValue<Angle> : LuaRecordValue<Angle, LuaValueField<&Angle::radians, "radians">>
    {
    };
    template <> struct LuaValueOverride<Angle, LuaValuePolicy>
    {
        inline static constexpr std::string_view name = "test.angle.degrees";
        inline static constexpr std::uint32_t version = 1;
        static LuaValueResult<void> push(LuaValueWriter &out, const Angle &value) noexcept
        {
            return out.number(value.radians * 57.29577951308232f);
        }
    };
    template <> struct LuaValueOverride<Resource, LuaValuePolicy>
    {
        inline static constexpr std::string_view name = "test.resource";
        inline static constexpr std::uint32_t version = 1;
        static LuaValueResult<Resource> read(LuaValueReader &input) noexcept
        {
            auto value = input.number<std::int32_t>();
            if (!value)
                return lux::cxx::unexpected(value.error());
            if (Resource::mutation_state)
            {
                lua_pushinteger(Resource::mutation_state, 73);
                lua_setfield(Resource::mutation_state, 1, "later");
                ++Resource::read_mutations;
            }
            return Resource{*value};
        }
    };
} // namespace lux::script::lua
struct Allocation
{
    bool deny{};
    std::size_t failed{};
    void* observation_context{};
    void (*observe_allocation)(void*) noexcept{};
    static void *allocate(void *context, void *pointer, std::size_t old_size, std::size_t size)
    {
        auto &self = *static_cast<Allocation *>(context);
        if (!size)
        {
            std::free(pointer);
            return nullptr;
        }
        if (self.deny && (pointer == nullptr || size > old_size))
        {
            ++self.failed;
            return nullptr;
        }
        if (const auto callback = std::exchange(self.observe_allocation, nullptr))
            callback(self.observation_context);
        return std::realloc(pointer, size);
    }
};
static void load(lua_State *state, const char *text)
{
    lua_settop(state, 0);
    assert(luaL_loadstring(state, text) == 0 && lua_pcall(state, 0, 1, 0) == 0);
}
static std::size_t protected_calls{};
static void countProtectedCall(lua_State*, lua_Debug*) { ++protected_calls; }

static void testNumericContract(lua_State* state)
{
    static_assert(!LuaValueCodec<std::int64_t>::can_read && !LuaValueCodec<std::int64_t>::can_push);
    static_assert(!LuaValueCodec<std::uint64_t>::can_read && !LuaValueCodec<std::uint64_t>::can_push);
    for (const auto value : {9007199254740991.0, 9007199254740992.0, 9007199254740994.0})
    {
        lua_settop(state, 0);
        lua_pushnumber(state, value);
        LuaValueReader reader{state, 1};
        const auto floating = LuaValueCodec<double>::read(reader);
        assert(floating && *floating == value);
        const auto integer = LuaValueCodec<std::int32_t>::read(reader);
        assert(!integer && integer.error().code == ELuaValueError::RANGE);
        assert(lua_gettop(state) == 1);
    }
    for (const auto value : {(std::numeric_limits<lua_Integer>::min)(), (std::numeric_limits<lua_Integer>::max)()})
    {
        lua_settop(state, 0);
        lua_pushinteger(state, value);
        LuaValueReader reader{state, 1};
        assert(!LuaValueCodec<std::int32_t>::read(reader));
        assert(!LuaValueCodec<std::uint32_t>::read(reader));
    }
    for (const auto* text : {"return 0/0", "return math.huge", "return -math.huge"})
    {
        load(state, text);
        LuaValueReader reader{state, 1};
        assert(!LuaValueCodec<double>::read(reader));
        assert(!LuaValueCodec<float>::read(reader));
    }
    lua_settop(state, 0);
    LuaValueWriter writer{state};
    assert(LuaValueCodec<double>::push(writer, -0.0));
    LuaValueReader reader{state, 1};
    const auto zero = LuaValueCodec<double>::read(reader);
    assert(zero && *zero == 0.0 && std::signbit(*zero));
    load(state, "return (-7 // 2) + (7 & 3) + (1 << 4)");
    LuaValueReader operators{state, 1};
    const auto combined = LuaValueCodec<std::int32_t>::read(operators);
    assert(combined && *combined == 15);
    std::puts("LUA_INTEGER_OPERATORS,division=-4,bitand=3,shift=16,typed_i32=15");
    lua_settop(state, 0);
    std::puts("LUA_NUMERIC_CONTRACT,i32_u32_bounds=1,f64_2pow53=1,i64_u64_unsupported=1,finite=1,negative_zero=1");
}

int main()
{
    static_assert(LuaValueCodec<NamedA>::representation() != LuaValueCodec<NamedB>::representation());
    static_assert(!LuaValueCodec<Angle>::can_read && LuaValueCodec<Angle>::can_push);
    static_assert(!std::is_default_constructible_v<Resource>);
    static_assert(LuaValueCodec<Nested<31>>::depth == 32 && LuaValueCodec<Nested<31>>::bounded);
    static_assert(!LuaValueCodec<Nested<32>>::bounded);
    static_assert(!LuaValueCodec<LargeValue>::bounded);
    static_assert(!LuaValueCodec<AngleHolder, LargePolicy>::bounded);
    static_assert(LuaValueCodec<std::int32_t>::representation() != LuaValueCodec<std::uint32_t>::representation());
    Allocation allocator;
    lua_State *state = lua_newstate(Allocation::allocate, &allocator, 1592598566U);
    assert(state);
    luaL_openlibs(state);
    assert(detail::LuaValueAccess::initialize(state));
    testNumericContract(state);
    load(state, "return {renamed=7,velocity={x=1.5,y=2.25},mode=3}");
    LuaValueReader reader{state, 1};
    auto pose = LuaValueCodec<Pose>::read(reader);
    assert(pose && pose->id == 7 && pose->velocity.x == 1.5f && pose->velocity.y == 2.25 && pose->mode == Mode::RUN);
    assert(lua_gettop(state) == 1);
    LuaValueWriter writer{state};
    assert(LuaValueCodec<Pose>::push(writer, *pose) && lua_gettop(state) == 2);
    static_assert(LuaValueCodec<Pose>::plainCount() == 6U);
    static_assert(LuaValueCodec<Angle>::plainCount() == 0U);
    static_assert(LuaValueCodec<Resource>::plainCount() == 0U);
    lua_pop(state, 1);
    lua_sethook(state, &countProtectedCall, LUA_MASKCALL, 0);
    assert(LuaValueCodec<Pose>::push(writer, *pose) && lua_gettop(state) == 2);
    lua_sethook(state, nullptr, 0, 0);
    assert(protected_calls == 1U);
    lua_setglobal(state, "snapshot");
    assert(luaL_dostring(state, "assert(snapshot.renamed==7 and snapshot.velocity.x==1.5 and snapshot.mode==3)") == 0);
    for (const char *input :
         {"return {renamed=7,velocity={x=1,y=2}}", "return {renamed=7,velocity={x=1,y=2},mode=2}",
          "return {renamed='7',velocity={x=1,y=2},mode=1}", "return {renamed=7.5,velocity={x=1,y=2},mode=1}",
          "return {renamed=7,velocity={x=1,y=2},mode=1,extra=true}",
          "return {renamed=7,velocity={x=1,y=2},mode=1,[1]=1}",
          "return setmetatable({velocity={x=1,y=2},mode=1},{__index=function() error('must not run') end})"})
    {
        load(state, input);
        LuaValueReader invalid{state, 1};
        assert(!LuaValueCodec<Pose>::read(invalid) && lua_gettop(state) == 1);
    }
    load(state, "return {renamed=7,velocity={x='bad',y=2},mode=1}");
    LuaValueReader path_input{state, 1};
    auto path_failure = LuaValueCodec<Pose>::read(path_input);
    assert(!path_failure && std::string_view{path_failure.error().path.data()} == "velocity.x");
    load(state, "return -1");
    LuaValueReader numeric{state, 1};
    assert(!numeric.number<std::uint32_t>());
    assert(!LuaValueCodec<Mode>::push(writer, static_cast<Mode>(8)) && lua_gettop(state) == 1);
    assert(writer.number(std::numeric_limits<double>::infinity()));
    LuaValueReader infinite{state, -1};
    assert(!infinite.number<double>());
    Resource::released.reserve(32);
    {
        LuaValueSlots<Resource, Resource> values;
        values.put<0>(Resource{1});
        values.put<1>(Resource{2});
        assert(Resource::live == 2);
    }
    assert(Resource::live == 0 && (Resource::released == std::vector<int>{2, 1}));
    Resource::released.clear();
    {
        const int top = lua_gettop(state);
        Pose invalid{7, {1.0f, 2.0}, static_cast<Mode>(8)};
        const auto rejected = LuaValueCodec<Pose>::push(writer, invalid);
        assert(!rejected && rejected.error().code == ELuaValueError::RANGE &&
            std::string_view{rejected.error().path.data()} == "mode" && lua_gettop(state) == top);
        LuaValueWriter deep{state, 32U};
        const auto depth = LuaValueCodec<Pose>::push(deep, *pose);
        assert(!depth && depth.error().code == ELuaValueError::CAPACITY && lua_gettop(state) == top);
        LuaValueSlots<Resource> values;
        values.put<0>(Resource{23});
        allocator.deny = true;
        const auto failed = LuaValueCodec<Pose>::push(writer, *pose);
        allocator.deny = false;
        assert(!failed && failed.error().code == ELuaValueError::VM_FAILURE &&
            Resource::live == 1 && lua_gettop(state) == top);
        assert(LuaValueCodec<Pose>::push(writer, *pose));
        lua_settop(state, top);
    }
    assert(Resource::live == 0 && (Resource::released == std::vector<int>{23}));
    std::puts("PLAIN_TREE_OK,nodes=6,pcall=1,enum_path=1,depth=1,oom=1,outer_owner=1,recovery=1");
    Resource::released.clear();
    {
        Velocity changing{1.0f, 2.0};
        allocator.observation_context = &changing;
        allocator.observe_allocation = [](void* context) noexcept {
            static_cast<Velocity*>(context)->x = 17.0f;
        };
        const int top = lua_gettop(state);
        assert(LuaValueCodec<Velocity>::push(writer, changing));
        assert(allocator.observe_allocation == nullptr && changing.x == 17.0f);
        lua_getfield(state, -1, "x");
        assert(lua_tonumber(state, -1) == 17.0);
        lua_settop(state, top);
        std::puts("PLAIN_TREE_FIELD_ORDER_OK,allocation_callback=1,observed_x=17");
    }
    load(state, "return 9");
    {
        LuaValueSlots<Resource, Resource> values;
        LuaValueReader input{state, 1};
        auto first = LuaValueCodec<Resource>::read(input);
        assert(first);
        values.put<0>(std::move(*first));
        // A subsequent Lua allocation fails while the first nontrivial object is in an outer frame.
        allocator.deny = true;
        auto failed = writer.record(64, [](LuaValueWriter &) noexcept -> LuaValueResult<void> { return {}; });
        allocator.deny = false;
        assert(!failed && allocator.failed && lua_gettop(state) == 1 && Resource::live == 1);
    }
    assert(Resource::live == 0 && (Resource::released == std::vector<int>{9}));
    assert(LuaValueCodec<Pose>::push(writer, *pose));
    lua_settop(state, 0);
    const std::string long_key(1024, 'k');
    const std::string message(2048, 'e');
    {
        LuaValueSlots<Resource> live;
        live.put<0>(Resource{17});
        const auto before = allocator.failed;
        auto key_failure = writer.record(0, [&](LuaValueWriter &table) noexcept {
            allocator.deny = true;
            return table.field(long_key, std::int32_t{4});
        });
        allocator.deny = false;
        assert(!key_failure && allocator.failed > before && lua_gettop(state) == 0 && Resource::live == 1);
        allocator.deny = true;
        assert(detail::LuaValueAccess::failure(state, message.c_str()) == 2);
        assert(lua_gettop(state) == 2 && lua_type(state, 1) == LUA_TBOOLEAN && !lua_toboolean(state, 1));
        assert(lua_isnil(state, 2));
        assert(lua_checkstack(state, 100000) == 0);
        allocator.deny = false;
        lua_settop(state, 0);
    }
    assert(Resource::live == 0 && Resource::released.back() == 17);
    load(state, "return {first=29,later=2}");
    Resource::mutation_state = state;
    {
        LuaValueReader changing{state, 1};
        const auto observed = LuaValueCodec<ResourceRecord>::read(changing);
        Resource::mutation_state = nullptr;
        assert(observed && observed->first.id == 29 && observed->later == 73);
        assert(Resource::read_mutations == 1 && Resource::live == 1 && lua_gettop(state) == 1);
    }
    assert(Resource::live == 0 && Resource::released.back() == 29);
    std::puts("CUSTOM_READ_FIELD_ORDER,first=29,later=73,mutations=1,live=0 PASS");
    assert(LuaValueCodec<Pose>::push(writer, *pose));
    lua_close(state);

    // Trampoline initialization itself is protected, including allocation failure.
    Allocation bootstrap;
    state = lua_newstate(Allocation::allocate, &bootstrap, 1592598566U);
    assert(state);
    bootstrap.deny = true;
    assert(!detail::LuaValueAccess::initialize(state));
    bootstrap.deny = false;
    assert(lua_gettop(state) == 0 && detail::LuaValueAccess::initialize(state));
    lua_close(state);
    std::puts("LUA_VALUE boundary shape const-construction reverse-cleanup representation OOM recovery PASS");
}
