#include <lux/engine/function/script/lua/LuaValue.hpp>
#include <lua.hpp>
#include <cassert>
#include <cstdio>
#include <cstdlib>
#include <vector>
#include <string>

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
namespace lux::script::lua
{
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
            return Resource{*value};
        }
    };
} // namespace lux::script::lua
struct Allocation
{
    bool deny{};
    std::size_t failed{};
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
        return std::realloc(pointer, size);
    }
};
static void load(lua_State *state, const char *text)
{
    lua_settop(state, 0);
    assert(luaL_loadstring(state, text) == 0 && lua_pcall(state, 0, 1, 0) == 0);
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
    lua_State *state = lua_newstate(Allocation::allocate, &allocator);
    assert(state);
    luaL_openlibs(state);
    assert(detail::LuaValueAccess::initialize(state));
    load(state, "return {renamed=7,velocity={x=1.5,y=2.25},mode=3}");
    LuaValueReader reader{state, 1};
    auto pose = LuaValueCodec<Pose>::read(reader);
    assert(pose && pose->id == 7 && pose->velocity.x == 1.5f && pose->velocity.y == 2.25 && pose->mode == Mode::RUN);
    assert(lua_gettop(state) == 1);
    LuaValueWriter writer{state};
    assert(LuaValueCodec<Pose>::push(writer, *pose) && lua_gettop(state) == 2);
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
    assert(LuaValueCodec<Pose>::push(writer, *pose));
    lua_close(state);

    // Trampoline initialization itself is protected, including LuaJIT closure allocation.
    Allocation bootstrap;
    state = lua_newstate(Allocation::allocate, &bootstrap);
    assert(state);
    bootstrap.deny = true;
    assert(!detail::LuaValueAccess::initialize(state));
    bootstrap.deny = false;
    assert(lua_gettop(state) == 0 && detail::LuaValueAccess::initialize(state));
    lua_close(state);
    std::puts("LUA_VALUE boundary shape const-construction reverse-cleanup representation OOM recovery PASS");
}
