#pragma once
/**
 * @file ScriptRuntime.hpp
 * @brief Language-runtime dispatcher shared by all script backends.
 */

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include <lux/engine/function/visibility.h>
#include <lux/engine/function/script/ScriptBackend.hpp>
#include <lux/engine/function/script/ScriptResult.hpp>
#include <lux/engine/function/script/ScriptSignature.hpp>

namespace lux::script
{
    using ModuleHandle = std::uint32_t;

    /** Runtime-validated identity of a function in a loaded module. */
    class LUX_FUNCTION_PUBLIC ScriptFunctionHandle final
    {
    public:
        ScriptFunctionHandle() = default;

        [[nodiscard]] ModuleHandle module() const noexcept { return module_; }
        [[nodiscard]] std::string_view name() const noexcept { return function_name_; }

    private:
        friend class ScriptRuntime;

        ScriptFunctionHandle(ModuleHandle module, std::string function_name)
            : module_(module), function_name_(std::move(function_name))
        {
        }

        ModuleHandle module_ = 0;
        std::string  function_name_;
    };

    class ScriptRuntimeState;

    class LUX_FUNCTION_PUBLIC ScriptRuntime final
    {
    public:
        ScriptRuntime();
        ~ScriptRuntime();

        ScriptRuntime(const ScriptRuntime&)            = delete;
        ScriptRuntime& operator=(const ScriptRuntime&) = delete;

        [[nodiscard]] ScriptResult<void> registerBackend(
            std::unique_ptr<IScriptBackend> backend
        );

        [[nodiscard]] ScriptResult<ModuleHandle> loadModule(
            const std::filesystem::path& path
        );

        [[nodiscard]] ScriptResult<ModuleHandle> loadModule(
            std::string_view backend_id,
            const std::filesystem::path& path
        );

        [[nodiscard]] ScriptResult<ModuleHandle> loadModuleFromMemory(
            std::string_view backend_id,
            std::span<const std::byte> payload,
            std::string_view module_name
        );

        [[nodiscard]] ScriptResult<void> unloadModule(ModuleHandle handle);

        [[nodiscard]] ScriptResult<ScriptFunctionHandle> findFunction(
            ModuleHandle handle,
            std::string_view function_name
        ) const;

        [[nodiscard]] ScriptResult<ScriptFunctionHandle> findFunction(
            std::string_view module_name,
            std::string_view function_name
        ) const;

        [[nodiscard]] ScriptResult<FunctionSignature> functionSignature(
            const ScriptFunctionHandle& function
        ) const;

        [[nodiscard]] ScriptResult<void> invoke(
            const ScriptFunctionHandle& function,
            CallFrame& frame
        ) const;

    private:
        std::unique_ptr<ScriptRuntimeState> state_;
    };
}
