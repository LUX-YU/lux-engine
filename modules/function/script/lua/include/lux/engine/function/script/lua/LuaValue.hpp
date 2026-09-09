#pragma once

#include <lux/engine/function/visibility.h>
#include <lux/engine/core/semantic/SemanticType.hpp>
#include <lux/cxx/compile_time/expected.hpp>

#include <array>
#include <cmath>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

struct lua_State;

namespace lux::script::lua
{
    enum class ELuaValueError : std::uint8_t
    {
        TYPE,
        RANGE,
        MISSING_FIELD,
        UNKNOWN_FIELD,
        CAPACITY,
        VM_FAILURE,
        UNSUPPORTED,
        CONSTRUCTION
    };

    struct LuaValueFailure final
    {
        ELuaValueError code{ELuaValueError::TYPE};
        std::array<char, 256> path{};
        bool truncated{};
        LUX_FUNCTION_PUBLIC void prepend(std::string_view field) noexcept;
    };

    template <class T> using LuaValueResult = lux::cxx::expected<T, LuaValueFailure>;
    struct LuaValuePolicy final
    {
        inline static constexpr std::string_view name = "lux.lua.value";
        inline static constexpr std::uint32_t version = 1;
    };
    template <class T, class Policy> struct LuaValueOverride
    {
    };
    template <class T> struct LuaGeneratedValue
    {
    };
    class LuaValueReader;
    class LuaValueWriter;

    template <class T>
    inline constexpr bool LuaValueScalar =
        std::is_same_v<T, bool> || std::is_same_v<T, std::int32_t> || std::is_same_v<T, std::uint32_t> ||
        std::is_same_v<T, float> || std::is_same_v<T, double>;

    // An optional owns only a successfully constructed T. Explicit reverse reset is independent of
    // tuple's implementation-defined element destruction order. No T is default constructed.
    template <class... T> class LuaValueSlots final
    {
      public:
        LuaValueSlots() noexcept = default;
        LuaValueSlots(const LuaValueSlots &) = delete;
        LuaValueSlots &operator=(const LuaValueSlots &) = delete;
        ~LuaValueSlots() noexcept { clear(std::index_sequence_for<T...>{}); }
        template <std::size_t I, class V> void put(V &&value) noexcept
        {
            using Value = std::tuple_element_t<I, std::tuple<T...>>;
            static_assert(std::is_nothrow_constructible_v<Value, V &&> && std::is_nothrow_destructible_v<Value>);
            std::get<I>(values_).emplace(std::forward<V>(value));
        }
        template <std::size_t I> auto &get() noexcept { return *std::get<I>(values_); }
        template <class F> decltype(auto) apply(F &&f) noexcept
        {
            return std::apply([&](auto &...slot) noexcept -> decltype(auto) { return f(*slot...); }, values_);
        }

      private:
        template <std::size_t... I> void clear(std::index_sequence<I...>) noexcept
        {
            (std::get<sizeof...(T) - 1 - I>(values_).reset(), ...);
        }
        std::tuple<std::optional<T>...> values_;
    };

    namespace detail
    {
        enum class ELuaPlainKind : std::uint8_t { NUMBER, BOOLEAN, RECORD };
        struct LuaPlainNumber final { double value{}; bool valid{true}; };
        struct LuaFieldLookup final { std::uint64_t hash; std::uint32_t ordinal; };
        constexpr std::uint64_t luaFieldHash(std::string_view value) noexcept
        {
            std::uint64_t hash = 14695981039346656037ULL;
            for (unsigned char c : value) hash = (hash ^ c) * 1099511628211ULL;
            return hash;
        }
        template <std::size_t N>
        consteval auto makeLuaFieldLookup(const std::array<std::string_view, N>& keys) noexcept
        {
            std::array<LuaFieldLookup, N> result{};
            for (std::size_t i{}; i < N; ++i) result[i] = {luaFieldHash(keys[i]), static_cast<std::uint32_t>(i)};
            for (std::size_t i = 1U; i < N; ++i)
                for (std::size_t j = i; j != 0U && result[j].hash < result[j - 1U].hash; --j)
                {
                    const auto previous = result[j - 1U];
                    result[j - 1U] = result[j];
                    result[j] = previous;
                }
            return result;
        }
        struct LuaCodecShape final
        {
            std::span<const std::string_view> keys;
            std::span<const LuaFieldLookup> lookup;
        };
        struct LuaCodecPlan;
        struct LuaCodecField final
        {
            std::string_view name;
            const void* (*member)(const void*) noexcept;
            const LuaCodecPlan& (*plan)() noexcept;
        };
        // Only immutable type structure. No invocation object addresses are stored here.
        struct LuaCodecPlan final
        {
            ELuaPlainKind kind;
            std::uint32_t stack;
            std::span<const LuaCodecField> fields;
            LuaPlainNumber (*read)(const void*) noexcept;
            bool (*validate)(double) noexcept;
            const LuaCodecShape* shape;
        };
        class LUX_FUNCTION_PUBLIC LuaValueAccess final
        {
          public:
            // Initialization is a protected cold operation, including trampoline allocation.
            [[nodiscard]] static bool initialize(lua_State *) noexcept;
            [[nodiscard]] static int top(lua_State *) noexcept;
            [[nodiscard]] static int absolute(lua_State *, int) noexcept;
            [[nodiscard]] static bool boolean(lua_State *, int, bool &) noexcept;
            [[nodiscard]] static bool number(lua_State *, int, double &) noexcept;
            [[nodiscard]] static bool pushBoolean(lua_State *, bool) noexcept;
            [[nodiscard]] static bool pushNumber(lua_State *, double) noexcept;
            [[nodiscard]] static int failure(lua_State *, const char *) noexcept;
            [[nodiscard]] static bool table(lua_State *, int fields) noexcept;
            [[nodiscard]] static bool prepare(lua_State*, const LuaCodecPlan&) noexcept;
            [[nodiscard]] static bool prepareShape(lua_State*, const LuaCodecShape&) noexcept;
            [[nodiscard]] static LuaValueResult<void> writePlan(lua_State*, const LuaCodecPlan&, const void*) noexcept;
            [[nodiscard]] static LuaValueResult<void> readPlan(lua_State*, int, const LuaCodecPlan&, std::span<double>) noexcept;
            [[nodiscard]] static LuaValueResult<void> shape(lua_State *, int,
                                                            std::span<const std::string_view>, const LuaCodecShape* = nullptr) noexcept;
            [[nodiscard]] static bool field(lua_State*, int, std::string_view, const LuaCodecShape* = nullptr, std::size_t = 0U) noexcept;
            [[nodiscard]] static bool setField(lua_State*, int, std::string_view, const LuaCodecShape* = nullptr, std::size_t = 0U) noexcept;
            // Only discard scratch values created by this API. They are never marked to-be-closed.
            static void restoreScratch(lua_State *, int) noexcept;
        };
    } // namespace detail

    class LuaValueReader final
    {
      public:
        // Trusted adapter entry; codecs only receive the resulting restricted reader.
        LuaValueReader(lua_State *state, int index, std::size_t depth = 0) noexcept
            : state_(state), index_(index > 0 ? index : detail::LuaValueAccess::absolute(state, index)), depth_(depth)
        {
        }
        [[nodiscard]] LuaValueResult<bool> boolean() const noexcept
        {
            bool value{};
            if (!detail::LuaValueAccess::boolean(state_, index_, value))
                return lux::cxx::unexpected(LuaValueFailure{});
            return value;
        }
        template <class T> [[nodiscard]] LuaValueResult<T> number() const noexcept
        {
            static_assert(LuaValueScalar<T> && !std::is_same_v<T, bool>);
            double value{};
            if (!detail::LuaValueAccess::number(state_, index_, value))
                return lux::cxx::unexpected(LuaValueFailure{});
            bool invalid = !std::isfinite(value);
            if constexpr (std::is_integral_v<T>)
                invalid = invalid || std::trunc(value) != value ||
                          value < static_cast<double>((std::numeric_limits<T>::lowest)()) ||
                          value > static_cast<double>((std::numeric_limits<T>::max)());
            else
                invalid = invalid || value < -static_cast<double>((std::numeric_limits<T>::max)()) ||
                          value > static_cast<double>((std::numeric_limits<T>::max)());
            if (invalid)
                return lux::cxx::unexpected(LuaValueFailure{ELuaValueError::RANGE});
            return static_cast<T>(value);
        }
        [[nodiscard]] LuaValueResult<void> shape(std::span<const std::string_view> keys, const detail::LuaCodecShape* plan = nullptr) const noexcept
        {
            if (depth_ >= 32 || keys.size() > 64)
                return lux::cxx::unexpected(LuaValueFailure{ELuaValueError::CAPACITY});
            return detail::LuaValueAccess::shape(state_, index_, keys, plan);
        }
        template <class T, class Policy = LuaValuePolicy>
        [[nodiscard]] LuaValueResult<T> field(std::string_view, const detail::LuaCodecShape* = nullptr, std::size_t = 0U) const noexcept;

      private:
        template <class, class> friend struct LuaValueCodec;
        lua_State *state_{};
        int index_{};
        std::size_t depth_{};
    };

    class LuaValueWriter final
    {
      public:
        explicit LuaValueWriter(lua_State *state, std::size_t depth = 0) noexcept : state_(state), depth_(depth) {}
        [[nodiscard]] LuaValueResult<void> boolean(bool value) noexcept
        {
            if (!detail::LuaValueAccess::pushBoolean(state_, value))
                return lux::cxx::unexpected(LuaValueFailure{ELuaValueError::VM_FAILURE});
            return {};
        }
        template <class T> [[nodiscard]] LuaValueResult<void> number(T value) noexcept
        {
            static_assert(LuaValueScalar<T> && !std::is_same_v<T, bool>);
            if (!detail::LuaValueAccess::pushNumber(state_, static_cast<double>(value)))
                return lux::cxx::unexpected(LuaValueFailure{ELuaValueError::VM_FAILURE});
            return {};
        }
        template <class F> [[nodiscard]] LuaValueResult<void> record(std::size_t count, F &&fields) noexcept
        {
            if (depth_ >= 32 || count > 64)
                return lux::cxx::unexpected(LuaValueFailure{ELuaValueError::CAPACITY});
            const int base = detail::LuaValueAccess::top(state_);
            if (!detail::LuaValueAccess::table(state_, static_cast<int>(count)))
                return lux::cxx::unexpected(LuaValueFailure{ELuaValueError::VM_FAILURE});
            LuaValueWriter child{state_, depth_ + 1};
            child.table_ = base + 1;
            auto result = fields(child);
            if (!result)
                detail::LuaValueAccess::restoreScratch(state_, base);
            return result;
        }
        template <class T, class Policy = LuaValuePolicy>
        [[nodiscard]] LuaValueResult<void> field(std::string_view, const T&, const detail::LuaCodecShape* = nullptr, std::size_t = 0U) noexcept;

      private:
        lua_State *state_{};
        std::size_t depth_{};
        int table_{};
        template <class, class> friend struct LuaValueCodec;
    };

    template <class T> struct LuaScalarValue
    {
        inline static constexpr std::string_view name = [] {
            if constexpr (std::is_same_v<T, bool>)
                return "lux.lua.bool";
            else if constexpr (std::is_same_v<T, std::int32_t>)
                return "lux.lua.i32";
            else if constexpr (std::is_same_v<T, std::uint32_t>)
                return "lux.lua.u32";
            else if constexpr (std::is_same_v<T, float>)
                return "lux.lua.f32";
            else
                return "lux.lua.f64";
        }();
        inline static constexpr std::uint32_t version = 1;
        inline static constexpr std::size_t storage = sizeof(T);
        inline static constexpr std::size_t depth = 0;
        static LuaValueResult<T> read(LuaValueReader &input) noexcept
        {
            if constexpr (std::is_same_v<T, bool>)
                return input.boolean();
            else
                return input.number<T>();
        }
        static LuaValueResult<void> push(LuaValueWriter &output, const T &value) noexcept
        {
            if constexpr (std::is_same_v<T, bool>)
                return output.boolean(value);
            else
                return output.number(value);
        }
    };

    template <class T, class Policy = LuaValuePolicy> struct LuaValueCodec
    {
        using Value = std::remove_cvref_t<T>;
        using Override = LuaValueOverride<Value, Policy>;
        inline static constexpr bool custom = requires { Override::name; };
        // Selection is for the complete representation, not separately for read and push.
        using Rule = std::conditional_t<
            custom, Override,
            std::conditional_t<LuaValueScalar<Value>, LuaScalarValue<Value>, LuaGeneratedValue<Value>>>;
        inline static constexpr bool can_read = requires(LuaValueReader &input) {
            { Rule::template read<Policy>(input) } noexcept -> std::same_as<LuaValueResult<Value>>;
        } || requires(LuaValueReader &input) {
            { Rule::read(input) } noexcept -> std::same_as<LuaValueResult<Value>>;
        };
        inline static constexpr bool can_push = requires(LuaValueWriter &output, const Value &value) {
            { Rule::template push<Policy>(output, value) } noexcept -> std::same_as<LuaValueResult<void>>;
        } || requires(LuaValueWriter &output, const Value &value) {
            { Rule::push(output, value) } noexcept -> std::same_as<LuaValueResult<void>>;
        };
        static consteval std::size_t storageSize() noexcept
        {
            constexpr std::size_t base =
                sizeof(LuaValueResult<Value>) + sizeof(std::optional<Value>) + 2 * sizeof(LuaValueFailure);
            std::size_t extra{};
            if constexpr (requires { Rule::template storageFor<Policy>(); })
                extra = Rule::template storageFor<Policy>();
            else if constexpr (requires { Rule::storage; })
                extra = Rule::storage;
            return base > 65536 || extra > 65536 - base ? std::size_t{65537} : base + extra;
        }
        static consteval std::size_t depthSize() noexcept
        {
            if constexpr (requires { Rule::template depthFor<Policy>(); })
                return Rule::template depthFor<Policy>();
            else if constexpr (requires { Rule::depth; })
                return Rule::depth;
            else
                return std::size_t{1};
        }
        inline static constexpr std::size_t storage = storageSize();
        inline static constexpr std::size_t depth = depthSize();
        inline static constexpr bool bounded = storage <= 65536 && depth <= 32;
        static consteval std::uint64_t representation() noexcept
        {
            std::uint64_t hash = 14695981039346656037ULL;
            const auto text = [&](std::string_view value) constexpr {
                for (unsigned char c : value)
                {
                    hash ^= c;
                    hash *= 1099511628211ULL;
                }
                hash ^= 0;
                hash *= 1099511628211ULL;
            };
            const auto integer = [&](std::uint64_t value) constexpr {
                for (unsigned i{}; i < 8; ++i)
                {
                    hash ^= (value >> (i * 8)) & 255;
                    hash *= 1099511628211ULL;
                }
            };
            text(Policy::name);
            integer(Policy::version);
            if constexpr (requires {
                              Rule::name;
                              Rule::version;
                          })
            {
                text(Rule::name);
                integer(Rule::version);
            }
            else
                text("unsupported");
            integer(can_read);
            integer(can_push);
            integer(sizeof(Value));
            integer(alignof(Value));
            if constexpr (lux::semantic::TypeDeclared<Value>)
                text(lux::semantic::TypeTraits<Value>::CanonicalName);
            if constexpr (requires { Rule::template fieldsFingerprint<Policy>(); })
                integer(Rule::template fieldsFingerprint<Policy>());
            return hash;
        }
        static LuaValueResult<Value> read(LuaValueReader &input) noexcept
        {
            if constexpr (can_read && bounded && plainCount() > 1U && plainCount() <= 8192U)
            {
                if (input.depth_ + depth > 32U)
                    return lux::cxx::unexpected(LuaValueFailure{ELuaValueError::CAPACITY});
                std::array<double, plainCount()> scratch;
                const auto read = detail::LuaValueAccess::readPlan(input.state_, input.index_, plan(), scratch);
                if (!read) return lux::cxx::unexpected(read.error());
                std::size_t cursor{};
                return consumePlain(scratch, cursor);
            }
            else if constexpr (can_read && bounded && requires { Rule::template read<Policy>(input); })
                return Rule::template read<Policy>(input);
            else if constexpr (can_read && bounded)
                return Rule::read(input);
            else
                return lux::cxx::unexpected(LuaValueFailure{ELuaValueError::UNSUPPORTED});
        }
        static consteval std::size_t plainCount() noexcept
        {
            if constexpr (custom || !std::is_trivially_copyable_v<Value> || !std::is_trivially_destructible_v<Value>)
                return 0;
            else if constexpr (LuaValueScalar<Value>)
                return 1;
            else if constexpr (requires { Rule::template plainCount<Policy>(); })
            {
                if constexpr (std::is_enum_v<Value> || std::is_aggregate_v<Value>)
                    return Rule::template plainCount<Policy>();
                else return 0;
            }
            else return 0;
        }
        static const detail::LuaCodecPlan& plan() noexcept
        {
            if constexpr (LuaValueScalar<Value>)
            {
                static constexpr detail::LuaCodecPlan result{
                    std::is_same_v<Value, bool> ? detail::ELuaPlainKind::BOOLEAN : detail::ELuaPlainKind::NUMBER,
                    2U, {}, [](const void* pointer) noexcept {
                        return detail::LuaPlainNumber{static_cast<double>(*static_cast<const Value*>(pointer)), true};
                    }, [](double value) noexcept {
                        if (!std::isfinite(value)) return false;
                        if constexpr (std::is_same_v<Value, bool>) return true;
                        else if constexpr (std::is_integral_v<Value>)
                            return std::trunc(value) == value &&
                                value >= static_cast<double>((std::numeric_limits<Value>::lowest)()) &&
                                value <= static_cast<double>((std::numeric_limits<Value>::max)());
                        else return value >= -static_cast<double>((std::numeric_limits<Value>::max)()) &&
                            value <= static_cast<double>((std::numeric_limits<Value>::max)());
                    }, nullptr
                };
                return result;
            }
            else return Rule::template plan<Policy>();
        }
        static bool prepare(lua_State* state) noexcept
        {
            if constexpr (!custom && requires { Rule::template prepare<Policy>(state); })
                return Rule::template prepare<Policy>(state);
            else return true;
        }
        static Value consumePlain(std::span<const double> values, std::size_t& cursor) noexcept
        {
            if constexpr (LuaValueScalar<Value>) return static_cast<Value>(values[cursor++]);
            else return Rule::template consumePlain<Policy>(values, cursor);
        }
        static LuaValueResult<void> push(LuaValueWriter &output, const Value &value) noexcept
        {
            if constexpr (can_push && bounded && plainCount() > 1U && plainCount() <= 8192U)
            {
                if (output.depth_ + depth > 32U)
                    return lux::cxx::unexpected(LuaValueFailure{ELuaValueError::CAPACITY});
                return detail::LuaValueAccess::writePlan(output.state_, plan(), std::addressof(value));
            }
            else if constexpr (can_push && bounded && requires { Rule::template push<Policy>(output, value); })
                return Rule::template push<Policy>(output, value);
            else if constexpr (can_push && bounded)
                return Rule::push(output, value);
            else
                return lux::cxx::unexpected(LuaValueFailure{ELuaValueError::UNSUPPORTED});
        }
    };

    template <class T, class Policy> LuaValueResult<T> LuaValueReader::field(std::string_view name, const detail::LuaCodecShape* plan, std::size_t ordinal) const noexcept
    {
        const auto base = detail::LuaValueAccess::top(state_);
        if (!detail::LuaValueAccess::field(state_, index_, name, plan, ordinal))
            return lux::cxx::unexpected(LuaValueFailure{ELuaValueError::VM_FAILURE});
        LuaValueReader child{state_, base + 1, depth_ + 1};
        auto value = LuaValueCodec<T, Policy>::read(child);
        detail::LuaValueAccess::restoreScratch(state_, base);
        if (!value)
            value.error().prepend(name);
        return value;
    }
    template <class T, class Policy>
    LuaValueResult<void> LuaValueWriter::field(std::string_view name, const T& value, const detail::LuaCodecShape* plan, std::size_t ordinal) noexcept
    {
        const auto base = detail::LuaValueAccess::top(state_);
        auto result = LuaValueCodec<T, Policy>::push(*this, value);
        if (result && detail::LuaValueAccess::top(state_) != base + 1)
            result = lux::cxx::unexpected(LuaValueFailure{ELuaValueError::CONSTRUCTION});
        if (result && !detail::LuaValueAccess::setField(state_, table_, name, plan, ordinal))
            result = lux::cxx::unexpected(LuaValueFailure{ELuaValueError::VM_FAILURE});
        detail::LuaValueAccess::restoreScratch(state_, base);
        if (!result)
            result.error().prepend(name);
        return result;
    }

    template <std::size_t N> struct LuaFieldName final
    {
        char text[N];
        constexpr LuaFieldName(const char (&value)[N]) noexcept
        {
            for (std::size_t i{}; i < N; ++i)
                text[i] = value[i];
        }
        constexpr operator std::string_view() const noexcept { return {text, N - 1}; }
    };
    template <auto Member, LuaFieldName Name> struct LuaValueField final
    {
        inline static constexpr auto member = Member;
        inline static constexpr std::string_view name = Name;
        template <class T> using Value = std::remove_cvref_t<decltype(std::declval<T>().*Member)>;
    };

    template <class T, class... Field> struct LuaRecordValue
    {
        static_assert(!std::is_union_v<T>, "Lua values do not support unions");
        inline static constexpr std::string_view name = "lux.lua.raw-record";
        inline static constexpr std::uint32_t version = 1;
        inline static constexpr std::array<std::string_view, sizeof...(Field)> keys{Field::name...};
        inline static constexpr auto lookup = detail::makeLuaFieldLookup(keys);
        inline static constexpr detail::LuaCodecShape shape_plan{keys, lookup};
        template <class Policy> static consteval std::uint64_t fieldsFingerprint() noexcept
        {
            std::uint64_t hash = 14695981039346656037ULL;
            (([&]() constexpr {
                 for (unsigned char c : Field::name)
                 {
                     hash ^= c;
                     hash *= 1099511628211ULL;
                 }
                 hash ^= LuaValueCodec<typename Field::template Value<T>, Policy>::representation();
                 hash *= 1099511628211ULL;
             }()),
             ...);
            return hash;
        }
        template <class Policy> static consteval std::size_t plainCount() noexcept
        {
            if constexpr (sizeof...(Field) > 64U ||
                !((LuaValueCodec<typename Field::template Value<T>, Policy>::plainCount() != 0U) && ...))
                return 0U;
            else return 1U + (std::size_t{0U} + ... +
                LuaValueCodec<typename Field::template Value<T>, Policy>::plainCount());
        }
        template <class Policy> static const detail::LuaCodecPlan& plan() noexcept
        {
            static constexpr std::array<detail::LuaCodecField, sizeof...(Field)> fields{{
                {Field::name, [](const void* parent) noexcept -> const void* {
                    return std::addressof(static_cast<const T*>(parent)->*Field::member);
                }, &LuaValueCodec<typename Field::template Value<T>, Policy>::plan}...
            }};
            static constexpr detail::LuaCodecPlan result{detail::ELuaPlainKind::RECORD,
                static_cast<std::uint32_t>(depthFor<Policy>() * 4U + 8U), fields, nullptr, nullptr, &shape_plan};
            return result;
        }
        template <class Policy> static bool prepare(lua_State* state) noexcept
        {
            return detail::LuaValueAccess::prepareShape(state, shape_plan) &&
                (LuaValueCodec<typename Field::template Value<T>, Policy>::prepare(state) && ...);
        }
        template <class Policy>
        static T consumePlain(std::span<const double> values, std::size_t& cursor) noexcept
        {
            ++cursor; // A record has no scalar payload. Never read its uninitialized scratch slot.
            return T{LuaValueCodec<typename Field::template Value<T>, Policy>::consumePlain(values, cursor)...};
        }
        using Slots = LuaValueSlots<typename Field::template Value<T>...>;
        template <class Policy> static consteval std::size_t storageFor() noexcept
        {
            std::size_t bytes = sizeof(Slots) + sizeof(T) + 2 * sizeof(LuaValueResult<void>);
            for (auto child : {std::size_t{0}, LuaValueCodec<typename Field::template Value<T>, Policy>::storage...})
                bytes = bytes > 65536 || child > 65536 - bytes ? 65537 : bytes + child;
            return bytes;
        }
        template <class Policy> static consteval std::size_t depthFor() noexcept
        {
            std::size_t value{};
            for (auto child : {std::size_t{0}, LuaValueCodec<typename Field::template Value<T>, Policy>::depth...})
                if (child > value)
                    value = child;
            return value > 32 ? std::size_t{33} : value + 1;
        }
        template <class Policy = LuaValuePolicy>
        static LuaValueResult<T> read(LuaValueReader &input) noexcept
            requires(std::is_aggregate_v<T> &&
                     (LuaValueCodec<typename Field::template Value<T>, Policy>::can_read && ...) &&
                     requires { T{std::declval<typename Field::template Value<T>>()...}; })
        {
            static_assert(noexcept(T{std::declval<typename Field::template Value<T>>()...}));
            auto shape = input.shape(keys, &shape_plan);
            if (!shape)
                return lux::cxx::unexpected(shape.error());
            Slots values;
            LuaValueResult<void> result;
            [&]<std::size_t... I>(std::index_sequence<I...>) noexcept {
                (([&]() noexcept {
                     if (!result)
                         return;
                     using F = std::tuple_element_t<I, std::tuple<Field...>>;
                     auto value = input.template field<typename F::template Value<T>, Policy>(F::name, &shape_plan, I);
                     if (value)
                         values.template put<I>(std::move(*value));
                     else
                         result = lux::cxx::unexpected(value.error());
                 }()),
                 ...);
            }(std::index_sequence_for<Field...>{});
            if (!result)
                return lux::cxx::unexpected(result.error());
            return values.apply([](auto &...field) noexcept { return T{std::move(field)...}; });
        }
        template <class Policy = LuaValuePolicy>
        static LuaValueResult<void> push(LuaValueWriter &output, const T &value) noexcept
            requires((LuaValueCodec<typename Field::template Value<T>, Policy>::can_push && ...))
        {
            return output.record(sizeof...(Field), [&](LuaValueWriter &table) noexcept {
                LuaValueResult<void> result;
                std::size_t ordinal{};
                (([&]() noexcept {
                     if (result)
                         result = table.template field<typename Field::template Value<T>, Policy>(Field::name,
                                                                                                  value.*Field::member, &shape_plan, ordinal++);
                 }()),
                 ...);
                return result;
            });
        }
    };

    template <class T, T... Values> struct LuaEnumValue
    {
        using Underlying = std::underlying_type_t<T>;
        static_assert(std::is_same_v<Underlying, std::int32_t> || std::is_same_v<Underlying, std::uint32_t>);
        inline static constexpr std::string_view name = "lux.lua.finite-enum";
        inline static constexpr std::uint32_t version = 1;
        template <class Policy> static consteval std::uint64_t fieldsFingerprint() noexcept
        {
            std::uint64_t hash = 14695981039346656037ULL;
            ((hash = (hash ^ static_cast<std::uint64_t>(Values)) * 1099511628211ULL), ...);
            return hash;
        }
        static constexpr bool valid(T value) noexcept { return ((value == Values) || ...); }
        template <class Policy> static consteval std::size_t plainCount() noexcept { return 1U; }
        template <class Policy> static const detail::LuaCodecPlan& plan() noexcept
        {
            static constexpr detail::LuaCodecPlan result{detail::ELuaPlainKind::NUMBER, 2U, {},
                [](const void* pointer) noexcept {
                    const auto current = *static_cast<const T*>(pointer);
                    return detail::LuaPlainNumber{static_cast<double>(static_cast<Underlying>(current)), valid(current)};
                }, [](double number) noexcept {
                    if (!LuaValueCodec<Underlying, Policy>::plan().validate(number)) return false;
                    return valid(static_cast<T>(static_cast<Underlying>(number)));
                }, nullptr};
            return result;
        }
        template <class Policy>
        static T consumePlain(std::span<const double> values, std::size_t& cursor) noexcept
        { return static_cast<T>(static_cast<Underlying>(values[cursor++])); }
        static LuaValueResult<T> read(LuaValueReader &input) noexcept
        {
            auto value = input.template number<Underlying>();
            if (!value)
                return lux::cxx::unexpected(value.error());
            const auto result = static_cast<T>(*value);
            if (!valid(result))
                return lux::cxx::unexpected(LuaValueFailure{ELuaValueError::RANGE});
            return result;
        }
        static LuaValueResult<void> push(LuaValueWriter &output, const T &value) noexcept
        {
            if (!valid(value))
                return lux::cxx::unexpected(LuaValueFailure{ELuaValueError::RANGE});
            return output.number(static_cast<Underlying>(value));
        }
    };
} // namespace lux::script::lua
