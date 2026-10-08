#pragma once
#include <lux/cxx/compile_time/expected.hpp>

#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/function/script/lua/LuaAllocation.hpp>
#include <lux/engine/function/visibility.h>
#include <memory>
#include <optional>
#include <string_view>

struct lua_State;

namespace lux::script::lua
{
    enum class ELuaEngineError : std::uint8_t
    {
        INVALID_CONFIGURATION,
        VM_CREATION_FAILURE,
        BOOTSTRAP_FAILURE
    };

    class ScriptEngine;

    class LUX_FUNCTION_PUBLIC ScriptRef
    {
    public:
        ScriptRef(int ref, ScriptEngine* engine);
        ~ScriptRef();

        ScriptRef(const ScriptRef&) = delete;
        ScriptRef& operator=(const ScriptRef&) = delete;

        ScriptRef(ScriptRef&& other) noexcept;
        ScriptRef& operator=(ScriptRef&& other) noexcept;

        int ref() const
        {
            return ref_;
        }

    private:
        int ref_;
        ScriptEngine* engine_;
    };

    class ScriptEngineImpl;

    class LUX_FUNCTION_PUBLIC ScriptEngine
    {
    public:
        using CreateResult = lux::cxx::expected<std::unique_ptr<ScriptEngine>, ELuaEngineError>;
        [[nodiscard]] static CreateResult create(LuaVmConfiguration configuration = {}) noexcept;
        ScriptEngine(const ScriptEngine&) = delete;
        ScriptEngine& operator=(const ScriptEngine&) = delete;
        ScriptEngine(ScriptEngine&&) = delete;
        ScriptEngine& operator=(ScriptEngine&&) = delete;
        [[nodiscard]] LuaAllocationStats allocationStats() const noexcept;
        ~ScriptEngine() noexcept;
        [[nodiscard]] lua_State* state() const noexcept;
        std::optional<ScriptRef> parseScript(std::string_view script);
        [[nodiscard]] bool runScript(const ScriptRef& program);

        using ErrorHandler = lux::cxx::move_only_function<void(std::string_view)>;

        void setOnError(ErrorHandler on_error);

    private:
        explicit ScriptEngine(std::unique_ptr<ScriptEngineImpl> impl) noexcept;
        std::unique_ptr<ScriptEngineImpl> impl_;
    };
} // namespace lux::script::lua
