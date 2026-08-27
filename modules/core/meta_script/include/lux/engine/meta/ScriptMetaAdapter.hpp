#pragma once

#include <lux/engine/function/script/BoundScriptCall.hpp>
#include <lux/engine/function/script/ScriptSemantic.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/core/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <memory>
#include <span>
#include <string_view>

namespace lux::meta
{
    enum class EScriptMetaAdapterError : std::uint8_t
    {
        METHOD_NOT_NOEXCEPT,
        METHOD_NOT_PUBLIC,
        MISSING_INVOKER,
        VARIADIC_NOT_SUPPORTED,
        MUTABLE_REFERENCE_NOT_SUPPORTED,
        RVALUE_REFERENCE_NOT_SUPPORTED,
        POINTER_NOT_SUPPORTED,
        UNSUPPORTED_TYPE,
        RETURN_NOT_SUPPORTED,
        CAPACITY_EXCEEDED,
        ALLOCATION_FAILURE,
    };

    struct ScriptRecordNameResolver final
    {
        void* context{};
        std::string_view (*canonicalName)(
            void* context,
            const RefType& type
        ) noexcept{};
    };

    struct AdaptedScriptSignature final
    {
        std::span<const lux::script::ScriptSemanticType> parameters;
        std::span<const lux::script::ScriptSemanticType> returns;
    };

    [[nodiscard]] LUX_CORE_PUBLIC lux::cxx::expected<
        AdaptedScriptSignature,
        EScriptMetaAdapterError> adaptScriptSignature(
            const RefMethod& method,
            ScriptRecordNameResolver resolver,
            std::span<lux::script::ScriptSemanticType> parameter_storage,
            std::span<lux::script::ScriptSemanticType> return_storage
        ) noexcept;

    class LUX_CORE_PUBLIC ReflectedScriptCall final
    {
      public:
        struct State;

        [[nodiscard]] static lux::cxx::expected<
            ReflectedScriptCall,
            EScriptMetaAdapterError> create(
                const RefMethod& method,
                void* object,
                std::size_t argument_capacity
            ) noexcept;

        ReflectedScriptCall(ReflectedScriptCall&&) noexcept;
        ReflectedScriptCall& operator=(ReflectedScriptCall&&) noexcept;
        ~ReflectedScriptCall();
        ReflectedScriptCall(const ReflectedScriptCall&) = delete;
        ReflectedScriptCall& operator=(const ReflectedScriptCall&) = delete;

        [[nodiscard]] lux::script::BoundScriptCall boundCall() noexcept;

      private:
        explicit ReflectedScriptCall(std::unique_ptr<State> state) noexcept;
        std::unique_ptr<State> state_;
    };
}
