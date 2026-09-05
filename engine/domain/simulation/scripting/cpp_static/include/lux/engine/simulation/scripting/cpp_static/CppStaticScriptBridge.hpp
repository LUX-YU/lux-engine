#pragma once

#include <lux/engine/core/semantic/SemanticType.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/simulation/scripting/ScriptBackend.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptCoroutine.hpp>
#include <lux/engine/simulation/scripting/cpp_static/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <utility>

namespace lux::simulation::script
{
    enum class ECppStaticScriptBridgeError : std::uint8_t
    {
        INVALID_DESCRIPTOR,
        INVALID_CLASS,
        METHOD_NOT_PUBLIC,
        METHOD_NOT_NOEXCEPT,
        FUNCTION_NOT_NOEXCEPT,
        MISSING_INVOKER,
        VARIADIC_NOT_SUPPORTED,
        MUTABLE_REFERENCE_NOT_SUPPORTED,
        RVALUE_REFERENCE_NOT_SUPPORTED,
        POINTER_NOT_SUPPORTED,
        UNSUPPORTED_TYPE,
        RETURN_NOT_SUPPORTED,
        DUPLICATE_SYMBOL,
        ALLOCATION_FAILURE,
    };

    struct CppStaticRecordSemanticResolver final
    {
        void* context{};
        bool (*resolve)(
            void* context,
            const lux::meta::RefType& type,
            lux::semantic::Layout& result
        ) noexcept{};
    };

    struct CppStaticCoroutineExport final
    {
        using InvokeFn = ScriptCoroutine (*)(
            void*,
            ScriptCoroutineContext&,
            lux_script_call_frame&
        ) noexcept;

        lux::script::ScriptSymbolId symbol{};
        const lux::meta::RefMethod* method{};
        const lux::meta::RefFunction* function{};
        InvokeFn invoke{};
        std::size_t visible_parameter_count{};
    };

    struct CppStaticMethodExport final
    {
        using InvokeFn = int (*)(lux_script_call_frame*) noexcept;
        lux::script::ScriptSymbolId symbol{};
        const lux::meta::RefMethod* method{};
        const lux::meta::RefFunction* function{};
        InvokeFn invoke{};
        std::uint64_t owner_type_hash{};
        bool (*matches)(const lux::meta::RefInvokable&, const lux::rdesc::ScriptFunction&) noexcept{};
    };

    namespace detail
    {
        template <class Argument>
        inline constexpr bool kCppMethodArgumentSupported =
            lux::semantic::TypeDeclared<std::remove_cvref_t<Argument>> &&
            !std::is_pointer_v<std::remove_reference_t<Argument>> &&
            (!std::is_reference_v<Argument> ||
                (std::is_lvalue_reference_v<Argument> && std::is_const_v<std::remove_reference_t<Argument>>));

        template <class Value>
        [[nodiscard]] bool cppMethodSlotMatches(const lux_script_value_slot& slot) noexcept
        {
            using Traits = lux::semantic::TypeTraits<std::remove_cvref_t<Value>>;
            return slot.data != nullptr && slot.type_id == lux::semantic::typeId(Traits::CanonicalName) &&
                slot.kind == Traits::AbiKind && slot.size == Traits::Size &&
                reinterpret_cast<std::uintptr_t>(slot.data) % Traits::Alignment == 0U;
        }

        template <class Result, class... Arguments, class Invoke, std::size_t... Index>
        int invokeCppMethod(lux_script_call_frame& frame, Invoke invoke, std::index_sequence<Index...>) noexcept
        {
            static_assert((kCppMethodArgumentSupported<Arguments> && ...));
            static_assert(sizeof...(Arguments) <= 64U, "CppStatic exports support at most 64 arguments");
            const bool is_invalid_arguments = frame.arg_count != sizeof...(Arguments) ||
                (frame.arg_count != 0U && frame.args == nullptr);
            if (is_invalid_arguments || !(cppMethodSlotMatches<Arguments>(frame.args[Index]) && ...))
                return -2;
            if constexpr (std::is_void_v<Result>)
            {
                if (frame.return_count != 0U)
                    return -2;
                invoke((*static_cast<const std::remove_cvref_t<Arguments>*>(frame.args[Index].data))...);
            }
            else
            {
                static_assert(lux::semantic::TypeDeclared<Result> && std::is_trivially_copyable_v<Result> &&
                    std::is_trivially_destructible_v<Result>);
                const bool is_invalid_result = frame.return_count != 1U || frame.returns == nullptr;
                if (is_invalid_result || !cppMethodSlotMatches<Result>(frame.returns[0]))
                    return -2;
                std::construct_at(static_cast<Result*>(frame.returns[0].data),
                    invoke((*static_cast<const std::remove_cvref_t<Arguments>*>(frame.args[Index].data))...));
            }
            return 0;
        }

        template <class Value>
        [[nodiscard]] bool cppMethodSemanticMatches(const lux::rdesc::ScriptValueType& value) noexcept
        {
            using Type = std::remove_cvref_t<Value>;
            using Traits = lux::semantic::TypeTraits<Type>;
            constexpr auto pass = std::is_reference_v<Value> ? lux::semantic::EValuePass::CONST_REF :
                lux::semantic::EValuePass::VALUE;
            return value.type_id == lux::semantic::typeId(Traits::CanonicalName) && value.pass == pass &&
                value.abi_kind == Traits::AbiKind && value.size == Traits::Size && value.alignment == Traits::Alignment;
        }

        template <class Result, class... Args>
        struct CppMethodSignature final
        {
            static bool matches(const lux::meta::RefInvokable& method,
                const lux::rdesc::ScriptFunction& signature) noexcept
            {
                if (method.parameters.size() != sizeof...(Args) || signature.args.size() != sizeof...(Args))
                    return false;
                const auto arguments_match = [&]<std::size_t... Index>(std::index_sequence<Index...>) noexcept {
                    return ((method.parameters[Index].value_type_hash ==
                        lux::cxx::type_hash<std::remove_cvref_t<Args>>() &&
                        cppMethodSemanticMatches<Args>(signature.args[Index])) && ...);
                };
                if (!arguments_match(std::index_sequence_for<Args...>{}))
                    return false;
                if constexpr (std::is_void_v<Result>)
                    return signature.returns.empty();
                else
                    return signature.returns.size() == 1U && cppMethodSemanticMatches<Result>(signature.returns[0]);
            }
        };

        template <auto Method> struct CppMethodTraits;

        template <class Owner, class Result, class... Args, Result (Owner::*Method)(Args...) noexcept>
        struct CppMethodTraits<Method> final
        {
            static constexpr auto OwnerHash = lux::cxx::type_hash<Owner>();
            static constexpr auto matches = &CppMethodSignature<Result, Args...>::matches;
            static int invoke(lux_script_call_frame* pointer) noexcept
            {
                if (pointer == nullptr || pointer->user_context == nullptr)
                    return -1;
                auto& frame = *pointer;
                auto* object = frame.user_context;
                return invokeCppMethod<Result, Args...>(frame,
                    [object](const std::remove_cvref_t<Args>&... args) noexcept -> Result {
                        return (static_cast<Owner*>(object)->*Method)(args...);
                    }, std::index_sequence_for<Args...>{});
            }
        };

        template <class Owner, class Result, class... Args, Result (Owner::*Method)(Args...) const noexcept>
        struct CppMethodTraits<Method> final
        {
            static constexpr auto OwnerHash = lux::cxx::type_hash<Owner>();
            static constexpr auto matches = &CppMethodSignature<Result, Args...>::matches;
            static int invoke(lux_script_call_frame* pointer) noexcept
            {
                if (pointer == nullptr || pointer->user_context == nullptr)
                    return -1;
                auto& frame = *pointer;
                auto* object = frame.user_context;
                return invokeCppMethod<Result, Args...>(frame,
                    [object](const std::remove_cvref_t<Args>&... args) noexcept -> Result {
                        return (static_cast<const Owner*>(object)->*Method)(args...);
                    }, std::index_sequence_for<Args...>{});
            }
        };

        template <class Result, class... Args, Result (*Function)(Args...) noexcept>
        struct CppMethodTraits<Function> final
        {
            static constexpr std::uint64_t OwnerHash{};
            static constexpr auto matches = &CppMethodSignature<Result, Args...>::matches;
            static int invoke(lux_script_call_frame* frame) noexcept
            {
                if (frame == nullptr)
                    return -1;
                return invokeCppMethod<Result, Args...>(*frame, Function, std::index_sequence_for<Args...>{});
            }
        };

        template <class Argument>
        [[nodiscard]] bool cppCoroutineArgumentMatches(const lux_script_value_slot& slot) noexcept
        {
            using Type = std::remove_cvref_t<Argument>;
            using Traits = lux::semantic::TypeTraits<Type>;
            return slot.data != nullptr && slot.type_id == lux::semantic::typeId(Traits::CanonicalName) &&
                slot.kind == Traits::AbiKind && slot.size == Traits::Size;
        }

        template <auto Function>
        struct CppCoroutineFunctionTraits;

        template <class Argument>
        inline constexpr bool kCppCoroutineArgumentSupported =
            !std::is_pointer_v<std::remove_reference_t<Argument>> &&
            (!std::is_reference_v<Argument> ||
                (std::is_lvalue_reference_v<Argument> &&
                    std::is_const_v<std::remove_reference_t<Argument>> &&
                    std::is_trivially_copyable_v<std::remove_cvref_t<Argument>> &&
                    std::is_trivially_destructible_v<std::remove_cvref_t<Argument>>)) &&
            lux::semantic::TypeDeclared<std::remove_cvref_t<Argument>>;

        template <class Owner, class... Arguments,
                  ScriptCoroutine (Owner::*Function)(ScriptCoroutineContext&, Arguments...) noexcept>
        struct CppCoroutineFunctionTraits<Function> final
        {
            static_assert((kCppCoroutineArgumentSupported<Arguments> && ...));

            template <std::size_t... Index>
            [[nodiscard]] static ScriptCoroutine invoke(
                void* object,
                ScriptCoroutineContext& context,
                lux_script_call_frame& frame,
                std::index_sequence<Index...>
            ) noexcept
            {
                if (object == nullptr || frame.arg_count != sizeof...(Arguments) ||
                    (frame.arg_count != 0U && frame.args == nullptr) ||
                    !(cppCoroutineArgumentMatches<Arguments>(frame.args[Index]) && ...))
                {
                    return {};
                }
                return (static_cast<Owner*>(object)->*Function)(
                    context,
                    (*static_cast<const std::remove_cvref_t<Arguments>*>(frame.args[Index].data))...
                );
            }

            [[nodiscard]] static ScriptCoroutine invokeErased(
                void* object,
                ScriptCoroutineContext& context,
                lux_script_call_frame& frame
            ) noexcept
            {
                return invoke(object, context, frame, std::index_sequence_for<Arguments...>{});
            }

            static constexpr std::size_t ParameterCount{sizeof...(Arguments)};
        };

        template <class... Arguments, ScriptCoroutine (*Function)(ScriptCoroutineContext&, Arguments...) noexcept>
        struct CppCoroutineFunctionTraits<Function> final
        {
            static_assert((kCppCoroutineArgumentSupported<Arguments> && ...));

            template <std::size_t... Index>
            [[nodiscard]] static ScriptCoroutine invoke(
                void*,
                ScriptCoroutineContext& context,
                lux_script_call_frame& frame,
                std::index_sequence<Index...>
            ) noexcept
            {
                if (frame.arg_count != sizeof...(Arguments) || (frame.arg_count != 0U && frame.args == nullptr) ||
                    !(cppCoroutineArgumentMatches<Arguments>(frame.args[Index]) && ...))
                {
                    return {};
                }
                return Function(
                    context,
                    (*static_cast<const std::remove_cvref_t<Arguments>*>(frame.args[Index].data))...
                );
            }

            [[nodiscard]] static ScriptCoroutine invokeErased(
                void* object,
                ScriptCoroutineContext& context,
                lux_script_call_frame& frame
            ) noexcept
            {
                return invoke(object, context, frame, std::index_sequence_for<Arguments...>{});
            }

            static constexpr std::size_t ParameterCount{sizeof...(Arguments)};
        };
    }

    template <auto Method>
    [[nodiscard]] CppStaticMethodExport makeCppStaticMethodExport(
        const lux::meta::RefMethod& reflected, lux::script::ScriptSymbolId symbol
    ) noexcept
    {
        using Traits = detail::CppMethodTraits<Method>;
        return {symbol, &reflected, nullptr, &Traits::invoke, Traits::OwnerHash, Traits::matches};
    }

    template <auto Function>
    [[nodiscard]] CppStaticMethodExport makeCppStaticMethodExport(
        const lux::meta::RefFunction& reflected, lux::script::ScriptSymbolId symbol
    ) noexcept
    {
        using Traits = detail::CppMethodTraits<Function>;
        return {symbol, nullptr, &reflected, &Traits::invoke, Traits::OwnerHash, Traits::matches};
    }

    template <auto Method>
    [[nodiscard]] CppStaticCoroutineExport makeCppStaticCoroutineExport(
        const lux::meta::RefMethod& reflected,
        lux::script::ScriptSymbolId symbol
    ) noexcept
    {
        using Traits = detail::CppCoroutineFunctionTraits<Method>;
        return {symbol, std::addressof(reflected), nullptr, &Traits::invokeErased, Traits::ParameterCount};
    }

    template <auto Function>
    [[nodiscard]] CppStaticCoroutineExport makeCppStaticCoroutineExport(
        const lux::meta::RefFunction& reflected,
        lux::script::ScriptSymbolId symbol
    ) noexcept
    {
        using Traits = detail::CppCoroutineFunctionTraits<Function>;
        return {symbol, nullptr, std::addressof(reflected), &Traits::invokeErased, Traits::ParameterCount};
    }

    class LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_PUBLIC
        CppStaticScriptDescriptor final
    {
      public:
        struct State;

        CppStaticScriptDescriptor() noexcept;
        explicit CppStaticScriptDescriptor(std::unique_ptr<State>) noexcept;
        CppStaticScriptDescriptor(CppStaticScriptDescriptor&&) noexcept;
        CppStaticScriptDescriptor& operator=(
            CppStaticScriptDescriptor&&
        ) noexcept;
        ~CppStaticScriptDescriptor();
        CppStaticScriptDescriptor(const CppStaticScriptDescriptor&) = delete;
        CppStaticScriptDescriptor& operator=(
            const CppStaticScriptDescriptor&
        ) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] const lux::rdesc::Script& description() const noexcept;
        [[nodiscard]] std::string_view key() const noexcept;

      private:
        std::unique_ptr<State> state_;
        friend class CppStaticScriptBackend;
    };

    [[nodiscard]] LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_PUBLIC
    lux::cxx::expected<
        CppStaticScriptDescriptor,
        ECppStaticScriptBridgeError> projectCppStaticEntityScript(
            std::string_view module_name,
            std::string_view descriptor_key,
            const lux::meta::RefClass& reflected_class,
            std::span<const lux::meta::RefMethod* const> methods,
            std::span<const lux::script::ScriptSymbolId> symbols,
            CppStaticRecordSemanticResolver record_types,
            void (*attach)(void*, ScriptBehavior&) noexcept = nullptr,
            lux::rdesc::ScriptLifecycleRoles lifecycle = {},
            std::span<const CppStaticCoroutineExport> coroutines = {},
            std::span<const lux::rdesc::ScriptApiRequirement> abilities = {},
            std::span<const lux::script::ScriptEventSourceDescription> events = {},
            std::span<const CppStaticMethodExport> typed_methods = {}
        ) noexcept;

    [[nodiscard]] LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_PUBLIC
    lux::cxx::expected<
        CppStaticScriptDescriptor,
        ECppStaticScriptBridgeError> projectCppStaticGlobalScript(
            std::string_view module_name,
            std::string_view descriptor_key,
            std::span<const lux::meta::RefFunction* const> functions,
            std::span<const lux::script::ScriptSymbolId> symbols,
            CppStaticRecordSemanticResolver record_types = {},
            lux::rdesc::ScriptLifecycleRoles lifecycle = {},
            std::span<const CppStaticCoroutineExport> coroutines = {},
            std::span<const lux::rdesc::ScriptApiRequirement> abilities = {},
            std::span<const lux::script::ScriptEventSourceDescription> events = {},
            std::span<const CppStaticMethodExport> typed_methods = {}
        ) noexcept;

    struct CppStaticScriptPoolDescription final
    {
        const CppStaticScriptDescriptor* descriptor{};
        std::size_t instance_capacity{};
        std::size_t coroutine_capacity{};
        std::size_t coroutine_frame_storage_bytes{};
        std::size_t coroutine_frame_storage_alignment{alignof(std::max_align_t)};
        std::size_t prepared_method_capacity{};
        std::span<const detail::StorageClassPlan> coroutine_frame_classes;
    };

    struct CppStaticScriptBackendStats final
    {
        std::size_t frame_storage_bytes{};
        std::size_t active_frames{};
        std::size_t frame_high_water{};
        std::size_t frame_capacity_failures{};
        std::size_t heap_frame_allocations{};
        std::size_t prepared_method_storage_bytes{};
        std::size_t active_prepared_methods{};
    };

    class LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_PUBLIC
        CppStaticScriptBackend final
    {
      public:
        [[nodiscard]] static lux::cxx::expected<CppStaticScriptBackend, ECppStaticScriptBridgeError>
        create(
            std::span<const CppStaticScriptPoolDescription> pools
        ) noexcept;
        ~CppStaticScriptBackend();
        CppStaticScriptBackend(
            CppStaticScriptBackend&&
        ) noexcept;
        CppStaticScriptBackend& operator=(
            CppStaticScriptBackend&&
        ) noexcept;
        CppStaticScriptBackend(
            const CppStaticScriptBackend&
        ) = delete;
        CppStaticScriptBackend& operator=(
            const CppStaticScriptBackend&
        ) = delete;

        [[nodiscard]] explicit operator bool() const noexcept;
        [[nodiscard]] CppStaticScriptBackendStats stats() const noexcept;
        [[nodiscard]] ScriptBackendDescriptor descriptor() noexcept;

      private:
        struct State;
        explicit CppStaticScriptBackend(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };
}
