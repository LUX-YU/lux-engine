#pragma once
#include <string_view>
#include <memory>
#include <optional>
#include <lux/cxx/core/move_only_function.hpp>
#include <lux/engine/function/visibility.h>

struct lua_State;

namespace lux::script::lua
{
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

		int ref() const { return ref_; }

    private:
        int ref_;
        ScriptEngine* engine_;
    };

    class ScriptEngineImpl;
    class LUX_FUNCTION_PUBLIC ScriptEngine
    {
    public:
        ScriptEngine();
        ~ScriptEngine();
        lua_State* state();
        std::optional<ScriptRef> parseScript(std::string_view script);
        void runScript(const ScriptRef& program);

        using ErrorHandler =
            lux::cxx::move_only_function<void(std::string_view)>;

        void setOnError(ErrorHandler on_error);
    private:

        std::unique_ptr<ScriptEngineImpl> impl_;
    };
}
