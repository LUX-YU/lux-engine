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
        enum class ELuaPlainKind : std::uint8_t { NUMBER, BOOLEAN, RECORD, INVALID_ENUM };
        // Trivial staging only. No T, destructor, custom codec or callback enters the protected VM operation.
        struct LuaPlainNode final
        {
            std::string_view field;
            double number{};
            std::uint32_t parent{};
            std::uint32_t children{};
            ELuaPlainKind kind{ELuaPlainKind::NUMBER};
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
            [[nodiscard]] static LuaValueResult<void> plain(lua_State *, std::span<const LuaPlainNode>, std::size_t) noexcept;
            [[nodiscard]] static LuaValueResult<void> shape(lua_State *, int,
                                                            std::span<const std::string_view>) noexcept;
            [[nodiscard]] static bool field(lua_State *, int, std::string_view) noexcept;
            [[nodiscard]] static bool setField(lua_State *, int, std::string_view) noexcept;
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
        [[nodiscard]] LuaValueResult<void> shape(std::span<const std::string_view> keys) const noexcept
        {
            if (depth_ >= 32 || keys.size() > 64)
                return lux::cxx::unexpected(LuaValueFailure{ELuaValueError::CAPACITY});
            return detail::LuaValueAccess::shape(state_, index_, keys);
        }
        template <class T, class Policy = LuaValuePolicy>
        [[nodiscard]] LuaValueResult<T> field(std::string_view) const noexcept;

      private:
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
        [[nodiscard]] LuaValueResult<void> plain(std::span<const detail::LuaPlainNode> nodes) noexcept
        {
            return detail::LuaValueAccess::plain(state_, nodes, depth_);
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
        [[nodiscard]] LuaValueResult<void> field(std::string_view, const T &) noexcept;

      private:
        lua_State *state_{};
        std::size_t depth_{};
        int table_{};
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
            if constexpr (can_read && bounded && requires { Rule::template read<Policy>(input); })
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
        static void appendPlain(std::span<detail::LuaPlainNode> nodes, std::size_t &cursor,
            std::uint32_t parent, std::string_view field, const Value &value) noexcept
        {
            if constexpr (LuaValueScalar<Value>)
                nodes[cursor++] = {field, static_cast<double>(value), parent, 0U,
                    std::is_same_v<Value, bool> ? detail::ELuaPlainKind::BOOLEAN : detail::ELuaPlainKind::NUMBER};
            else Rule::template appendPlain<Policy>(nodes, cursor, parent, field, value);
        }
        static LuaValueResult<void> push(LuaValueWriter &output, const Value &value) noexcept
        {
            if constexpr (can_push && bounded && plainCount() > 1U &&
                plainCount() * sizeof(detail::LuaPlainNode) <= 65536U)
            {
                std::array<detail::LuaPlainNode, plainCount()> nodes{};
                std::size_t cursor{};
                appendPlain(nodes, cursor, 0U, {}, value);
                return output.plain(nodes);
            }
            else if constexpr (can_push && bounded && requires { Rule::template push<Policy>(output, value); })
                return Rule::template push<Policy>(output, value);
            else if constexpr (can_push && bounded)
                return Rule::push(output, value);
            else
                return lux::cxx::unexpected(LuaValueFailure{ELuaValueError::UNSUPPORTED});
        }
    };

    template <class T, class Policy> LuaValueResult<T> LuaValueReader::field(std::string_view name) const noexcept
    {
        const auto base = detail::LuaValueAccess::top(state_);
        if (!detail::LuaValueAccess::field(state_, index_, name))
            return lux::cxx::unexpected(LuaValueFailure{ELuaValueError::VM_FAILURE});
        LuaValueReader child{state_, base + 1, depth_ + 1};
        auto value = LuaValueCodec<T, Policy>::read(child);
        detail::LuaValueAccess::restoreScratch(state_, base);
        if (!value)
            value.error().prepend(name);
        return value;
    }
    template <class T, class Policy>
    LuaValueResult<void> LuaValueWriter::field(std::string_view name, const T &value) noexcept
    {
        const auto base = detail::LuaValueAccess::top(state_);
        auto result = LuaValueCodec<T, Policy>::push(*this, value);
        if (result && detail::LuaValueAccess::top(state_) != base + 1)
            result = lux::cxx::unexpected(LuaValueFailure{ELuaValueError::CONSTRUCTION});
        if (result && !detail::LuaValueAccess::setField(state_, table_, name))
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
        template <class Policy> static void appendPlain(std::span<detail::LuaPlainNode> nodes,
            std::size_t &cursor, std::uint32_t parent, std::string_view field, const T &value) noexcept
        {
            const auto own = static_cast<std::uint32_t>(cursor);
            nodes[cursor++] = {field, 0.0, parent, sizeof...(Field), detail::ELuaPlainKind::RECORD};
            (LuaValueCodec<typename Field::template Value<T>, Policy>::appendPlain(
                nodes, cursor, own, Field::name, value.*Field::member), ...);
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
            auto shape = input.shape(keys);
            if (!shape)
                return lux::cxx::unexpected(shape.error());
            Slots values;
            LuaValueResult<void> result;
            [&]<std::size_t... I>(std::index_sequence<I...>) noexcept {
                (([&]() noexcept {
                     if (!result)
                         return;
                     using F = std::tuple_element_t<I, std::tuple<Field...>>;
                     auto value = input.template field<typename F::template Value<T>, Policy>(F::name);
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
                (([&]() noexcept {
                     if (result)
                         result = table.template field<typename Field::template Value<T>, Policy>(Field::name,
                                                                                                  value.*Field::member);
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
        template <class Policy> static void appendPlain(std::span<detail::LuaPlainNode> nodes,
            std::size_t &cursor, std::uint32_t parent, std::string_view field, T value) noexcept
        {
            nodes[cursor++] = {field, static_cast<double>(static_cast<Underlying>(value)), parent, 0U,
                valid(value) ? detail::ELuaPlainKind::NUMBER : detail::ELuaPlainKind::INVALID_ENUM};
        }
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
