#pragma once
#include <lux/engine/function/script/lua/Lua.hpp>
#include <string_view>

#include <lua.hpp>
#include <lux/engine/function/script/lua/LuaBoundary.h>
#include <lux/engine/function/script/lua/LuaPageAllocator.hpp>

namespace lux::script::lua
{
    class ScriptEngineImpl
    {
    public:
        struct VmDeleter final
        {
            void operator()(lua_State* state) const noexcept
            {
                lua_close(state);
            }
        };

        using VmOwner = std::unique_ptr<lua_State, VmDeleter>;
        using CreateResult = lux::cxx::expected<std::unique_ptr<ScriptEngineImpl>, ELuaEngineError>;

        [[nodiscard]] static CreateResult create(LuaVmConfiguration config) noexcept
        {
            const bool is_invalid_mode =
                config.gc_mode != ELuaGcMode::INCREMENTAL && config.gc_mode != ELuaGcMode::GENERATIONAL;
            const bool has_invalid_parameter =
                std::ranges::any_of(config.gc_parameters, [](int value) { return value < -1; });
            if (is_invalid_mode || has_invalid_parameter)
            {
                return lux::cxx::unexpected(ELuaEngineError::INVALID_CONFIGURATION);
            }
            auto allocator = std::make_unique<LuaPageAllocator>(config);
            VmOwner vm(lua_newstate(allocator->callback(), allocator.get(), config.seed));
            if (!vm)
            {
                return lux::cxx::unexpected(ELuaEngineError::VM_CREATION_FAILURE);
            }
            lua_pushcfunction(vm.get(), &luxLuaBootstrap);
            if (lua_pcall(vm.get(), 0, 0, 0) != LUA_OK)
            {
                return lux::cxx::unexpected(ELuaEngineError::BOOTSTRAP_FAILURE);
            }
            lua_gc(vm.get(), config.gc_mode == ELuaGcMode::GENERATIONAL ? LUA_GCGEN : LUA_GCINC);
            for (int index{}; index < LUA_GCPN; ++index)
            {
                if (config.gc_parameters[index] != -1)
                {
                    lua_gc(vm.get(), LUA_GCPARAM, index, config.gc_parameters[index]);
                }
            }
            return std::unique_ptr<ScriptEngineImpl>(new ScriptEngineImpl(std::move(allocator), std::move(vm)));
        }

        [[nodiscard]] LuaAllocationStats allocationStats() const noexcept
        {
            auto result = allocator_->stats();
            if (result.enabled)
            {
                for (int i{}; i < LUA_GCPN; ++i)
                {
                    result.gc_parameters[i] = lua_gc(vm_.get(), LUA_GCPARAM, i, -1);
                }
            }
            return result;
        }

        ~ScriptEngineImpl() noexcept = default;

        [[nodiscard]] lua_State* state() const noexcept
        {
            return vm_.get();
        }

        void setErrorCallback(ScriptEngine::ErrorHandler cb)
        {
            on_error_ = std::move(cb);
        }

        /// Parses the source code and returns a registry ref
        std::optional<int> parseScript(std::string_view code)
        {
            if (luaL_loadbufferx(vm_.get(), code.data(), code.size(), "blueprint", "t") != LUA_OK)
            {
                reportAndPopError();
                return std::nullopt;
            }
            return luaL_ref(vm_.get(), LUA_REGISTRYINDEX);
        }

        /// Runs a registry ref
        bool runScript(const ScriptRef& program)
        {
            // push traceback(err) closure
            lua_pushlightuserdata(vm_.get(), this);
            lua_pushcclosure(vm_.get(), &luaTraceback, 1);
            int errfunc = lua_gettop(vm_.get());

            lua_rawgeti(vm_.get(), LUA_REGISTRYINDEX, program.ref()); // push func

            const bool succeeded = lua_pcall(vm_.get(), 0, 0, errfunc) == LUA_OK;
            if (!succeeded)
            {
                reportAndPopError(); // the error message is already on top of the stack
            }
            lua_remove(vm_.get(), errfunc); // pop the traceback closure
            return succeeded;
        }

        ScriptEngineImpl(const ScriptEngineImpl&) = delete;
        ScriptEngineImpl& operator=(const ScriptEngineImpl&) = delete;

    private:
        ScriptEngineImpl(std::unique_ptr<LuaPageAllocator> allocator, VmOwner vm) noexcept
            : allocator_(std::move(allocator)), vm_(std::move(vm))
        {
        }

        static int luaTraceback(lua_State* L)
        {
            const char* msg = lua_tostring(L, 1);
            auto* self = static_cast<ScriptEngineImpl*>(lua_touserdata(L, lua_upvalueindex(1)));
            if (self && self->on_error_)
            {
                self->on_error_(msg ? msg : "(no msg)");
            }
            luaL_traceback(L, L, msg, 1);
            return 1;
        }

        void reportAndPopError()
        {
            if (on_error_)
            {
                on_error_(lua_tostring(vm_.get(), -1));
            }
            lua_pop(vm_.get(), 1);
        }

        // lua_Alloc retains the allocator address. Move only the owning pointer; close VM first.
        std::unique_ptr<LuaPageAllocator> allocator_;
        ScriptEngine::ErrorHandler on_error_;
        VmOwner vm_;
    };
} // namespace lux::script::lua
