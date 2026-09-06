#pragma once

#include <lux/engine/simulation/scripting/ScriptBackend.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptCoroutine.hpp>

#include <array>
#include <cstring>
#include <functional>
#include <span>
#include <string_view>
#include <tuple>

namespace lux::simulation::script
{
// Non-owning views of canonical Script facts. Generated tables have static lifetime;
// no registry, reflected declarations, provider addresses or runtime slots are stored here.
struct CppStaticValueView final
{
    lux::semantic::Layout layout;
    lux::semantic::EValuePass pass{};
};

struct CppStaticExportEntry final
{
    using StartFn = ScriptCoroutine (*)(void *, ScriptCoroutineContext &, const lux_script_call_frame &,
                                        void *) noexcept;

    lux::script::ScriptSymbolId symbol{};
    std::string_view name;
    std::span<const CppStaticValueView> parameters;
    std::span<const CppStaticValueView> results;
    lux_script_invoke_fn invoke{};
    StartFn start{};
    std::size_t owned_bytes{};
    std::size_t owned_alignment{1U};
};

struct CppStaticObjectOperations final
{
    std::size_t size{};
    std::size_t alignment{1U};
    bool (*construct)(void *) noexcept {};
    void (*destroy)(void *) noexcept {};
    void (*attach)(void *, ScriptBehavior &) noexcept {};
    bool requires_host{};
};

struct CppStaticApiRequirement final
{
    lux::script::ScriptApiContractIdView contract;
    std::uint64_t expected_schema_hash{};
};

struct CppStaticContract final
{
    std::string_view module;
    std::string_view key;
    bool entity_scope{};
    CppStaticObjectOperations object;
    std::span<const CppStaticExportEntry> exports;
    lux::rdesc::ScriptLifecycleRoles lifecycle;
    std::span<const CppStaticApiRequirement> abilities;
    std::span<const lux::script::ScriptEventSourceView> events;
};

namespace detail
{
template <class T>
inline constexpr bool cppStaticArgumentSupported =
    lux::semantic::TypeDeclared<std::remove_cvref_t<T>> && !std::is_pointer_v<std::remove_reference_t<T>> &&
    !std::is_volatile_v<std::remove_reference_t<T>> &&
    ((!std::is_reference_v<T> && std::is_nothrow_copy_constructible_v<T>) ||
        (std::is_lvalue_reference_v<T> && std::is_const_v<std::remove_reference_t<T>>));

template <class T>
inline constexpr bool cppStaticPersistentArgumentSupported =
    cppStaticArgumentSupported<T> &&
    (!std::is_reference_v<T> || (std::is_trivially_copyable_v<std::remove_cvref_t<T>> &&
                                 std::is_trivially_destructible_v<std::remove_cvref_t<T>>));

template <class T> [[nodiscard]] consteval CppStaticValueView cppStaticValue() noexcept
{
    static_assert(cppStaticArgumentSupported<T>, "Unsupported CppStatic semantic parameter");
    using Traits = lux::semantic::TypeTraits<std::remove_cvref_t<T>>;
    static_assert(Traits::Size == sizeof(std::remove_cvref_t<T>) &&
        Traits::Alignment == alignof(std::remove_cvref_t<T>), "CppStatic requires the actual native layout");
    static_assert(Traits::Alignment != 0U && (Traits::Alignment & (Traits::Alignment - 1U)) == 0U);
    return {{lux::semantic::typeId(Traits::CanonicalName), Traits::CanonicalName, Traits::AbiKind, Traits::Size,
             Traits::Alignment},
            std::is_reference_v<T> ? lux::semantic::EValuePass::CONST_REF : lux::semantic::EValuePass::VALUE};
}

template <class T> [[nodiscard]] bool cppStaticSlotMatches(const lux_script_value_slot &slot) noexcept
{
    constexpr auto type = cppStaticValue<T>().layout;
    return slot.data != nullptr && slot.type_id == type.type_id && slot.kind == type.abi_kind &&
           slot.size == type.size && reinterpret_cast<std::uintptr_t>(slot.data) % type.alignment == 0U;
}

template <class... Args> struct CppStaticArguments
{
    static_assert((cppStaticArgumentSupported<Args> && ...), "Unsupported CppStatic parameter");
    static_assert(sizeof...(Args) <= 64U, "CppStatic exports support at most 64 arguments");
    inline static constexpr std::array<CppStaticValueView, sizeof...(Args)> Parameters{cppStaticValue<Args>()...};

    template <std::size_t... Index>
    [[nodiscard]] static bool valid(const lux_script_call_frame &frame, std::index_sequence<Index...>) noexcept
    {
        const bool invalid_count = frame.arg_count != sizeof...(Args);
        const bool missing_storage = frame.arg_count != 0U && frame.args == nullptr;
        return !invalid_count && !missing_storage && (cppStaticSlotMatches<Args>(frame.args[Index]) && ...);
    }
};

template <class Result, class... Args> struct CppStaticSyncShape : CppStaticArguments<Args...>
{
    inline static constexpr auto Results = []() consteval {
        if constexpr (std::is_void_v<Result>)
            return std::array<CppStaticValueView, 0>{};
        else
        {
            static_assert(!std::is_reference_v<Result> && std::is_trivially_copyable_v<Result> &&
                              std::is_trivially_destructible_v<Result>,
                          "CppStatic result must be an owned value");
            return std::array{cppStaticValue<Result>()};
        }
    }();

    template <class Invoke, std::size_t... Index>
    static int call(lux_script_call_frame &frame, Invoke invoke, std::index_sequence<Index...> sequence) noexcept
    {
        if (!CppStaticArguments<Args...>::valid(frame, sequence))
            return -2;
        if constexpr (std::is_void_v<Result>)
        {
            if (frame.return_count != 0U)
                return -2;
            invoke((*static_cast<const std::remove_cvref_t<Args> *>(frame.args[Index].data))...);
        }
        else
        {
            const bool invalid_result = frame.return_count != 1U || frame.returns == nullptr;
            if (invalid_result || !cppStaticSlotMatches<Result>(frame.returns[0]))
                return -2;
            std::construct_at(static_cast<Result *>(frame.returns[0].data),
                              invoke((*static_cast<const std::remove_cvref_t<Args> *>(frame.args[Index].data))...));
        }
        return 0;
    }
};

template <auto Function> struct CppStaticSyncEntry;

template <class Owner, class Result, class... Args, Result (Owner::*Function)(Args...) noexcept>
struct CppStaticSyncEntry<Function> : CppStaticSyncShape<Result, Args...>
{
    static int invoke(lux_script_call_frame *frame) noexcept
    {
        if (frame == nullptr || frame->user_context == nullptr)
            return -1;
        auto *object = static_cast<Owner *>(frame->user_context);
        return CppStaticSyncShape<Result, Args...>::call(
            *frame,
            [object](const std::remove_cvref_t<Args> &...args) noexcept -> Result {
                return (object->*Function)(args...);
            },
            std::index_sequence_for<Args...>{});
    }
};

template <class Owner, class Result, class... Args, Result (Owner::*Function)(Args...) const noexcept>
struct CppStaticSyncEntry<Function> : CppStaticSyncShape<Result, Args...>
{
    static int invoke(lux_script_call_frame *frame) noexcept
    {
        if (frame == nullptr || frame->user_context == nullptr)
            return -1;
        auto *object = static_cast<const Owner *>(frame->user_context);
        return CppStaticSyncShape<Result, Args...>::call(
            *frame,
            [object](const std::remove_cvref_t<Args> &...args) noexcept -> Result {
                return (object->*Function)(args...);
            },
            std::index_sequence_for<Args...>{});
    }
};

template <class Result, class... Args, Result (*Function)(Args...) noexcept>
struct CppStaticSyncEntry<Function> : CppStaticSyncShape<Result, Args...>
{
    static int invoke(lux_script_call_frame *frame) noexcept
    {
        if (frame == nullptr)
            return -1;
        return CppStaticSyncShape<Result, Args...>::call(*frame, Function, std::index_sequence_for<Args...>{});
    }
};

template <class... Args> struct CppStaticOwnedArguments : CppStaticArguments<Args...>
{
    static_assert((cppStaticPersistentArgumentSupported<Args> && ...),
                  "CppStatic coroutine const-reference must be trivial; pointers and mutable references are forbidden");
    struct Layout final
    {
        std::array<std::size_t, sizeof...(Args)> offsets{};
        std::size_t bytes{};
        std::size_t alignment{1U};
    };
    inline static constexpr Layout Owned = []() consteval {
        Layout layout;
        static_assert(((sizeof(std::remove_cvref_t<Args>) <= 65536U) && ...));
        constexpr std::array<std::size_t, sizeof...(Args)> sizes{
            (std::is_reference_v<Args> ? sizeof(std::remove_cvref_t<Args>) : 0U)...};
        constexpr std::array<std::size_t, sizeof...(Args)> alignments{alignof(std::remove_cvref_t<Args>)...};
        for (std::size_t index{}; index < sizes.size(); ++index)
        {
            if (sizes[index] != 0U)
            {
                layout.bytes = (layout.bytes + alignments[index] - 1U) & ~(alignments[index] - 1U);
                layout.offsets[index] = layout.bytes;
                layout.bytes += sizes[index];
                layout.alignment = (std::max)(layout.alignment, alignments[index]);
            }
        }
        return layout;
    }();
    static_assert(Owned.bytes <= 65536U, "CppStatic owned arguments exceed the bounded supported layout");

    template <std::size_t Index>
    [[nodiscard]] static decltype(auto) read(const lux_script_call_frame &frame, void *owned) noexcept
    {
        using T = std::tuple_element_t<Index, std::tuple<Args...>>;
        using Value = std::remove_cvref_t<T>;
        const auto *value = static_cast<const Value *>(frame.args[Index].data);
        if constexpr (std::is_reference_v<T>)
        {
            auto *destination = static_cast<std::byte *>(owned) + Owned.offsets[Index];
            std::memcpy(destination, value, sizeof(Value));
            return *static_cast<const Value *>(static_cast<const void *>(destination));
        }
        else
            return Value(*value);
    }
};

template <auto Function> struct CppStaticCoroutineEntry;

template <class Owner, class... Args, ScriptCoroutine (Owner::*Function)(ScriptCoroutineContext &, Args...) noexcept>
struct CppStaticCoroutineEntry<Function> : CppStaticOwnedArguments<Args...>
{
    template <std::size_t... Index>
    static ScriptCoroutine call(void *object, ScriptCoroutineContext &context, const lux_script_call_frame &frame,
                                void *owned, std::index_sequence<Index...> sequence) noexcept
    {
        const bool invalid_storage = object == nullptr || frame.return_count != 0U ||
                                     (CppStaticOwnedArguments<Args...>::Owned.bytes != 0U && owned == nullptr);
        if (invalid_storage || !CppStaticArguments<Args...>::valid(frame, sequence))
            return {};
        return (static_cast<Owner *>(object)->*Function)(
            context, CppStaticOwnedArguments<Args...>::template read<Index>(frame, owned)...);
    }

    static ScriptCoroutine start(void *object, ScriptCoroutineContext &context, const lux_script_call_frame &frame,
                                 void *owned) noexcept
    {
        return call(object, context, frame, owned, std::index_sequence_for<Args...>{});
    }
};

template <class... Args, ScriptCoroutine (*Function)(ScriptCoroutineContext &, Args...) noexcept>
struct CppStaticCoroutineEntry<Function> : CppStaticOwnedArguments<Args...>
{
    template <std::size_t... Index>
    static ScriptCoroutine call(ScriptCoroutineContext &context, const lux_script_call_frame &frame, void *owned,
                                std::index_sequence<Index...> sequence) noexcept
    {
        const bool invalid_storage =
            frame.return_count != 0U || (CppStaticOwnedArguments<Args...>::Owned.bytes != 0U && owned == nullptr);
        if (invalid_storage || !CppStaticArguments<Args...>::valid(frame, sequence))
            return {};
        return Function(context, CppStaticOwnedArguments<Args...>::template read<Index>(frame, owned)...);
    }

    static ScriptCoroutine start(void *, ScriptCoroutineContext &context, const lux_script_call_frame &frame,
                                 void *owned) noexcept
    {
        return call(context, frame, owned, std::index_sequence_for<Args...>{});
    }
};

template <class Owner, auto Attach = nullptr>
[[nodiscard]] consteval CppStaticObjectOperations cppStaticObject() noexcept
{
    static_assert(std::is_nothrow_destructible_v<Owner>);
    CppStaticObjectOperations result{sizeof(Owner), alignof(Owner),
            [](void *storage) noexcept {
                try
                {
                    std::construct_at(static_cast<Owner *>(storage));
                    return true;
                }
                catch (...)
                {
                    return false;
                }
            },
            [](void *object) noexcept { std::destroy_at(static_cast<Owner *>(object)); }, nullptr};
    if constexpr (!std::is_same_v<decltype(Attach), std::nullptr_t>)
    {
        static_assert(std::is_same_v<decltype(Attach), void (Owner::*)(ScriptBehavior&) noexcept>,
            "Script attach must be void(ScriptBehavior&) noexcept on a mutable object");
        result.requires_host = true;
        result.attach = [](void* object, ScriptBehavior& behavior) noexcept {
            std::invoke(Attach, *static_cast<Owner*>(object), behavior);
        };
    }
    return result;
}
} // namespace detail
} // namespace lux::simulation::script
