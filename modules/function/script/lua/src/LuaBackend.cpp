#include <lux/engine/function/script/backends/LuaBackend.hpp>
#include <lux/engine/function/script/lua/Lua.hpp>

#include <cstddef>
#include <fstream>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace lux::script::lua_backend
{
    using lux::script::CallFrame;
    using lux::script::ConstValueView;
    using lux::script::FunctionSignature;
    using lux::script::IScriptBackend;
    using lux::script::IScriptModule;
    using lux::script::ScriptFunction;
    using lux::script::ScriptModulePtr;

    namespace
    {
        /**
         * Minimal ScriptFunction implementation backed by a Lua chunk reference.
         *
         * Phase-2 limitations: only treats the chunk as a "void()" entry. Real
         * argument / return marshalling is intentionally postponed to a later
         * phase that grows the ABI ↔ Lua bridge in lockstep with the FlowForge
         * compiler / native backend so they share the same call protocol.
         */
        class LuaChunkFunction final : public ScriptFunction
        {
        public:
            LuaChunkFunction(lux::script::lua::ScriptEngine& engine,
                             lux::script::lua::ScriptRef    ref,
                             FunctionSignature              sig)
                : engine_(engine), ref_(std::move(ref)), sig_(std::move(sig))
            {
            }

            const FunctionSignature& signature() const noexcept override { return sig_; }

            ScriptResult<void> invoke(CallFrame& /*frame*/) const override
            {
                std::string detail;
                engine_.setOnError(
                    [&detail](std::string_view message)
                    {
                        detail.assign(message);
                    }
                );
                const bool succeeded = engine_.runScript(ref_);
                engine_.setOnError({});
                if (!succeeded)
                {
                    return lux::cxx::unexpected(scriptFailure(
                        EScriptError::INVOKE_FAILED,
                        detail.empty() ? "Lua script invocation failed" : std::move(detail)
                    ));
                }
                return {};
            }

        private:
            lux::script::lua::ScriptEngine& engine_;
            mutable lux::script::lua::ScriptRef ref_;
            FunctionSignature sig_;
        };

        class LuaModule final : public IScriptModule
        {
        public:
            LuaModule(std::string name,
                      std::unique_ptr<lux::script::lua::ScriptEngine> engine)
                : name_(std::move(name)), engine_(std::move(engine))
            {
            }

            std::string_view name() const noexcept override { return name_; }

            const ScriptFunction* findFunction(std::string_view fn_name) const override
            {
                auto it = functions_.find(std::string(fn_name));
                return it == functions_.end() ? nullptr : it->second.get();
            }

            void registerEntry(std::string fn_name, std::unique_ptr<LuaChunkFunction> fn)
            {
                functions_.emplace(std::move(fn_name), std::move(fn));
            }

            lux::script::lua::ScriptEngine& engine() noexcept { return *engine_; }

        private:
            std::string name_;
            std::unique_ptr<lux::script::lua::ScriptEngine> engine_;
            std::unordered_map<std::string, std::unique_ptr<LuaChunkFunction>> functions_;
        };

        class LuaBackend final : public IScriptBackend
        {
        public:
            std::string_view backendId() const noexcept override { return "lua"; }

            std::vector<std::string> handledExtensions() const override
            {
                return {"lua"};
            }

            ScriptResult<ScriptModulePtr> loadModule(
                const std::filesystem::path& path
            ) override
            {
                std::ifstream input(path, std::ios::binary);
                if (!input)
                {
                    return lux::cxx::unexpected(scriptFailure(
                        EScriptError::IO_ERROR,
                        "cannot open Lua script '" + path.string() + "'"
                    ));
                }
                std::ostringstream stream;
                stream << input.rdbuf();
                std::string src = stream.str();
                return parseSource(src, path.stem().string());
            }

            ScriptResult<ScriptModulePtr> loadFromMemory(
                std::span<const std::byte> payload,
                std::string_view module_name
            ) override
            {
                if (payload.empty())
                {
                    return lux::cxx::unexpected(scriptFailure(
                        EScriptError::INVALID_ARGUMENT,
                        "Lua script memory payload cannot be empty"
                    ));
                }
                std::string src(reinterpret_cast<const char*>(payload.data()),
                                payload.size());
                std::string name = module_name.empty()
                    ? std::string("memory_module")
                    : std::string(module_name);
                return parseSource(src, std::move(name));
            }

        private:
            static ScriptResult<ScriptModulePtr> parseSource(
                const std::string& src,
                std::string module_name
            )
            {
                auto engine = std::make_unique<lux::script::lua::ScriptEngine>();
                std::string detail;
                engine->setOnError(
                    [&detail](std::string_view message)
                    {
                        detail.assign(message);
                    }
                );
                auto ref    = engine->parseScript(src);
                engine->setOnError({});
                if (!ref)
                {
                    return lux::cxx::unexpected(scriptFailure(
                        EScriptError::LOAD_FAILED,
                        detail.empty() ? "Lua script compilation failed" : std::move(detail)
                    ));
                }

                auto mod = std::make_unique<LuaModule>(module_name, std::move(engine));

                FunctionSignature sig;
                sig.name = module_name + ".main";
                auto fn  = std::make_unique<LuaChunkFunction>(mod->engine(),
                                                              std::move(*ref),
                                                              std::move(sig));
                mod->registerEntry("main", std::move(fn));
                return ScriptModulePtr(std::move(mod));
            }
        };
    }

    std::unique_ptr<IScriptBackend> create()
    {
        return std::make_unique<LuaBackend>();
    }
}
