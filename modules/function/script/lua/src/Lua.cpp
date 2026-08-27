#include <lux/engine/function/script/lua/LuaImpl.hpp>

namespace lux::script::lua
{
    ScriptRef::ScriptRef(int ref, ScriptEngine* engine) : ref_(ref), engine_(engine)
    {
    }
    ScriptRef::~ScriptRef()
    {
        if (ref_ != LUA_NOREF)
        {
            luaL_unref(static_cast<lua_State*>(engine_->state()), LUA_REGISTRYINDEX, ref_);
        }
    }

    ScriptRef::ScriptRef(ScriptRef&& other) noexcept
    {
        ref_ = other.ref_;
        engine_ = other.engine_;
        other.ref_ = LUA_NOREF;  // Invalidate the moved-from reference
        other.engine_ = nullptr; // Invalidate the moved-from engine
    }

    ScriptRef& ScriptRef::operator=(ScriptRef&& other) noexcept
    {
        if (this != &other)
        {
            // Unreference the current ref_ if it's valid
            if (ref_ != LUA_NOREF)
            {
                luaL_unref(static_cast<lua_State*>(engine_->state()), LUA_REGISTRYINDEX, ref_);
            }
            ref_ = other.ref_;
            engine_ = other.engine_;
            other.ref_ = LUA_NOREF;  // Invalidate the moved-from reference
            other.engine_ = nullptr; // Invalidate the moved-from engine
        }
        return *this;
    }

    ScriptEngine::ScriptEngine() : impl_(std::make_unique<ScriptEngineImpl>())
    {
    }

    ScriptEngine::~ScriptEngine()
    {
    }

    std::optional<ScriptRef> ScriptEngine::parseScript(std::string_view script)
    {
        auto program_ref_opt = impl_->parseScript(script);
        if (!program_ref_opt)
        {
            return std::nullopt;
        }
        return ScriptRef(*program_ref_opt, this);
    }

    lua_State* ScriptEngine::state()
    {
        return impl_->state();
    }

    bool ScriptEngine::runScript(const ScriptRef& program)
    {
        return impl_->runScript(program);
    }

    void ScriptEngine::setOnError(ErrorHandler on_error)
    {
        impl_->setErrorCallback(std::move(on_error));
    }
}
