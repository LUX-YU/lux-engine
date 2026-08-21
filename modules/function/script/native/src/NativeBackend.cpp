#include <lux/engine/function/script/backends/NativeBackend.hpp>
#include <lux/engine/dynamic_library/DynamicLibrary.hpp>

#include <lux/engine/function/script/abi/lux_script_abi.h>
#include <lux/engine/function/script/ScriptModule.hpp>
#include <lux/engine/function/script/ScriptCallFrame.hpp>
#include <lux/engine/function/script/ScriptSignature.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lux::script::native_backend
{
    using lux::engine::platform::DynamicLibrary;
    using lux::script::CallFrame;
    using lux::script::FunctionSignature;
    using lux::script::IScriptBackend;
    using lux::script::IScriptModule;
    using lux::script::ScriptFunction;
    using lux::script::ScriptModulePtr;

    namespace
    {
        /**
         * Wraps a single `lux_script_function_desc::invoke` pointer.
         *
         * The pointer remains owned by the loaded dynamic library; lifetime is
         * therefore tied to its owning @ref NativeModule.
         */
        class NativeFunction final : public ScriptFunction
        {
        public:
            NativeFunction(FunctionSignature sig, lux_script_invoke_fn invoke) noexcept
                : sig_(std::move(sig)), invoke_(invoke)
            {
            }

            const FunctionSignature& signature() const noexcept override { return sig_; }

            ScriptResult<void> invoke(CallFrame& frame) const override
            {
                if (!invoke_ || !frame.raw())
                {
                    return lux::cxx::unexpected(scriptFailure(
                        EScriptError::INVALID_ARGUMENT,
                        "native script invocation has no entry point or call frame"
                    ));
                }
                const int result = invoke_(frame.raw());
                if (result != 0)
                {
                    return lux::cxx::unexpected(scriptFailure(
                        EScriptError::INVOKE_FAILED,
                        "native script function returned error code "
                            + std::to_string(result)
                    ));
                }
                return {};
            }

        private:
            FunctionSignature    sig_;
            lux_script_invoke_fn invoke_ = nullptr;
        };

        class NativeModule final : public IScriptModule
        {
        public:
            NativeModule(DynamicLibrary lib,
                         const lux_script_module_desc* desc)
                : lib_(std::move(lib))
            {
                name_ = desc->module_name ? desc->module_name : "";

                functions_.reserve(desc->function_count);
                for (std::uint32_t i = 0; i < desc->function_count; ++i)
                {
                    const auto& f = desc->functions[i];
                    if (!f.name || !f.invoke) continue;

                    auto fn = std::make_unique<NativeFunction>(
                        FunctionSignature::from(f), f.invoke);
                    index_.emplace(std::string(f.name), fn.get());
                    functions_.emplace_back(std::move(fn));
                }
            }

            std::string_view name() const noexcept override { return name_; }

            const ScriptFunction* findFunction(std::string_view fn_name) const override
            {
                auto it = index_.find(std::string(fn_name));
                return it == index_.end() ? nullptr : it->second;
            }

        private:
            DynamicLibrary                                    lib_;
            std::string                                       name_;
            std::vector<std::unique_ptr<NativeFunction>>      functions_;
            std::unordered_map<std::string, NativeFunction*>  index_;
        };

        class NativeBackend final : public IScriptBackend
        {
        public:
            explicit NativeBackend(HostSymbolResolver resolver)
                : resolver_(std::move(resolver))
            {
            }

            std::string_view backendId() const noexcept override { return "native"; }

            std::vector<std::string> handledExtensions() const override
            {
                // Cross-platform shared-library extensions. FlowForge-MLIR's
                // hash-suffixed artefacts (Phase 5+) keep one of these.
                return {"dll", "so", "dylib"};
            }

            ScriptResult<ScriptModulePtr> loadModule(
                const std::filesystem::path& path
            ) override
            {
                DynamicLibrary lib;
                if (!lib.load(path))
                {
                    return lux::cxx::unexpected(scriptFailure(
                        EScriptError::IO_ERROR,
                        "cannot load native script '" + path.string()
                            + "': " + lib.last_error()
                    ));
                }
                return finalize(std::move(lib));
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
                        "native script memory payload cannot be empty"
                    ));
                }
                DynamicLibrary lib;
                if (!lib.load_from_memory(payload, module_name))
                {
                    return lux::cxx::unexpected(scriptFailure(
                        EScriptError::LOAD_FAILED,
                        "cannot load native script memory image '"
                            + std::string(module_name) + "': " + lib.last_error()
                    ));
                }
                return finalize(std::move(lib));
            }

        private:
            ScriptResult<ScriptModulePtr> finalize(DynamicLibrary lib)
            {
                using entry_fn = const lux_script_module_desc* (*)();
                auto entry = reinterpret_cast<entry_fn>(
                    lib.get_symbol(LUX_SCRIPT_MODULE_ENTRY));
                if (!entry)
                {
                    return lux::cxx::unexpected(scriptFailure(
                        EScriptError::INVALID_ENTRY_POINT,
                        "native script does not export " LUX_SCRIPT_MODULE_ENTRY
                    ));
                }

                const lux_script_module_desc* desc = entry();
                if (!desc)
                {
                    return lux::cxx::unexpected(scriptFailure(
                        EScriptError::INVALID_MODULE,
                        "native script entry returned a null module descriptor"
                    ));
                }
                if (desc->abi_version != LUX_SCRIPT_ABI_VERSION)
                {
                    return lux::cxx::unexpected(scriptFailure(
                        EScriptError::ABI_MISMATCH,
                        "native script ABI version " + std::to_string(desc->abi_version)
                            + " does not match host version "
                            + std::to_string(LUX_SCRIPT_ABI_VERSION)
                    ));
                }
                if (!desc->module_name)
                {
                    return lux::cxx::unexpected(scriptFailure(
                        EScriptError::INVALID_MODULE,
                        "native script module name is null"
                    ));
                }
                if (desc->function_count != 0 && !desc->functions)
                {
                    return lux::cxx::unexpected(scriptFailure(
                        EScriptError::INVALID_MODULE,
                        "native script function table is null"
                    ));
                }
                std::unordered_set<std::string_view> function_names;
                for (std::uint32_t i = 0; i < desc->function_count; ++i)
                {
                    const auto& function = desc->functions[i];
                    if (!function.name || !function.invoke)
                    {
                        return lux::cxx::unexpected(scriptFailure(
                            EScriptError::INVALID_MODULE,
                            "native script function entry is incomplete"
                        ));
                    }
                    if (function.arg_count != 0 && !function.args)
                    {
                        return lux::cxx::unexpected(scriptFailure(
                            EScriptError::INVALID_MODULE,
                            "native script argument type table is null"
                        ));
                    }
                    if (function.return_count != 0 && !function.returns)
                    {
                        return lux::cxx::unexpected(scriptFailure(
                            EScriptError::INVALID_MODULE,
                            "native script return type table is null"
                        ));
                    }
                    if (!function_names.emplace(function.name).second)
                    {
                        return lux::cxx::unexpected(scriptFailure(
                            EScriptError::INVALID_MODULE,
                            "native script exports a duplicate function name"
                        ));
                    }
                }

                // Optional host binding: a module that imports engine symbols
                // exports LUX_SCRIPT_BIND_HOST_ENTRY. Bind BEFORE exposing
                // any function, and reject the module when any import stays
                // unresolved — no invoke may ever run against a null import.
                if (auto bind = reinterpret_cast<lux_script_bind_host_fn>(
                        lib.get_symbol(LUX_SCRIPT_BIND_HOST_ENTRY)))
                {
                    if (bind(&NativeBackend::resolveThunk, this,
                             LUX_SCRIPT_ABI_VERSION) != 0)
                    {
                        return lux::cxx::unexpected(scriptFailure(
                            EScriptError::HOST_BIND_FAILED,
                            "native script host-symbol binding failed"
                        ));
                    }
                }

                return ScriptModulePtr(
                    std::make_unique<NativeModule>(std::move(lib), desc)
                );
            }

            // C-ABI thunk over the move-only resolver. Only alive for
            // the duration of the bind call; modules keep the ADDRESSES it
            // returns, never the resolver itself.
            static void* resolveThunk(void* host_ctx, const char* symbol_name)
            {
                auto* self = static_cast<NativeBackend*>(host_ctx);
                if (!self || !self->resolver_ || !symbol_name) return nullptr;
                return self->resolver_(std::string_view(symbol_name));
            }

            HostSymbolResolver resolver_;
        };
    }

    std::unique_ptr<IScriptBackend> create(HostSymbolResolver resolver)
    {
        return std::make_unique<NativeBackend>(std::move(resolver));
    }
}
