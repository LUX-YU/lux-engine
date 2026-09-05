#pragma once

#include <lux/engine/simulation/scripting/cpp_static/CppStaticScriptContract.hpp>
#include <lux/engine/simulation/scripting/cpp_static/visibility.h>

#include <memory>
#include <span>

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

using CppStaticDescriptionResult = lux::cxx::expected<lux::rdesc::Script, ECppStaticScriptBridgeError>;

// Asset production only: the backend retains the generated constant contract, not this owning copy.
[[nodiscard]] LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_PUBLIC CppStaticDescriptionResult
materializeCppStaticScript(const CppStaticContract &contract) noexcept;

struct CppStaticScriptPoolDescription final
{
    const CppStaticContract *descriptor{};
    std::size_t instance_capacity{};
    std::size_t coroutine_capacity{};
    std::size_t coroutine_frame_storage_bytes{};
    std::size_t coroutine_frame_storage_alignment{alignof(std::max_align_t)};
    std::size_t prepared_method_capacity{};
    std::size_t max_coroutine_frame_bytes{512U};
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

class LUX_ENGINE_SIMULATION_SCRIPT_CPP_STATIC_PUBLIC CppStaticScriptBackend final
{
  public:
    [[nodiscard]] static lux::cxx::expected<CppStaticScriptBackend, ECppStaticScriptBridgeError> create(
        std::span<const CppStaticScriptPoolDescription> pools) noexcept;
    ~CppStaticScriptBackend();
    CppStaticScriptBackend(CppStaticScriptBackend &&) noexcept;
    CppStaticScriptBackend &operator=(CppStaticScriptBackend &&) noexcept;
    CppStaticScriptBackend(const CppStaticScriptBackend &) = delete;
    CppStaticScriptBackend &operator=(const CppStaticScriptBackend &) = delete;

    [[nodiscard]] explicit operator bool() const noexcept;
    [[nodiscard]] CppStaticScriptBackendStats stats() const noexcept;
    [[nodiscard]] ScriptBackendDescriptor descriptor() noexcept;

  private:
    struct State;
    explicit CppStaticScriptBackend(std::unique_ptr<State> state) noexcept;
    std::unique_ptr<State> state_;
};
} // namespace lux::simulation::script
