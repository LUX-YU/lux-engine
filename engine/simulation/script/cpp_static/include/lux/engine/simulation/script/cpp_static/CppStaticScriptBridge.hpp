#pragma once

#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/simulation/script/ScriptBackend.hpp>
#include <lux/engine/simulation/script/cpp_static/visibility.h>

#include <lux/cxx/compile_time/expected.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

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
            lux::script::ScriptSemanticLayout& result
        ) noexcept{};
    };

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
            void (*attach)(void*, ScriptBehavior&) noexcept = nullptr
        ) noexcept;

    [[nodiscard]] LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_PUBLIC
    lux::cxx::expected<
        CppStaticScriptDescriptor,
        ECppStaticScriptBridgeError> projectCppStaticGlobalScript(
            std::string_view module_name,
            std::string_view descriptor_key,
            std::span<const lux::meta::RefFunction* const> functions,
            std::span<const lux::script::ScriptSymbolId> symbols,
            CppStaticRecordSemanticResolver record_types = {}
        ) noexcept;

    class LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_PUBLIC
        CppStaticScriptBackend final
    {
      public:
        CppStaticScriptBackend(
            std::span<const CppStaticScriptDescriptor* const> descriptors,
            std::size_t instance_capacity
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
        [[nodiscard]] ScriptBackendDescriptor descriptor() noexcept;

      private:
        struct State;
        std::unique_ptr<State> state_;
    };
}
