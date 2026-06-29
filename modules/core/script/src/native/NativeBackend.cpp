#include <lux/engine/script/backends/NativeBackend.hpp>
#include <lux/engine/dynamic_library/DynamicLibrary.hpp>

#include <lux/engine/script/abi/lux_script_abi.h>
#include <lux/engine/script/ScriptModule.hpp>
#include <lux/engine/script/ScriptCallFrame.hpp>
#include <lux/engine/script/ScriptSignature.hpp>

#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
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

            bool invoke(CallFrame& frame) const override
            {
                if (!invoke_ || !frame.raw()) return false;
                return invoke_(frame.raw()) == 0;
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
            std::string_view backendId() const noexcept override { return "native"; }

            std::vector<std::string> handledExtensions() const override
            {
                // Cross-platform shared-library extensions. FlowForge-MLIR's
                // hash-suffixed artefacts (Phase 5+) keep one of these.
                return {"dll", "so", "dylib"};
            }

            ScriptModulePtr loadModule(const std::filesystem::path& path) override
            {
                DynamicLibrary lib;
                if (!lib.load(path)) return nullptr;
                return finalize(std::move(lib));
            }

            ScriptModulePtr loadFromMemory(std::span<const std::byte> payload,
                                           std::string_view           module_name) override
            {
                if (payload.empty()) return nullptr;
                DynamicLibrary lib;
                if (!lib.load_from_memory(payload, module_name)) return nullptr;
                return finalize(std::move(lib));
            }

        private:
            static ScriptModulePtr finalize(DynamicLibrary lib)
            {
                using entry_fn = const lux_script_module_desc* (*)();
                auto entry = reinterpret_cast<entry_fn>(
                    lib.get_symbol(LUX_SCRIPT_MODULE_ENTRY));
                if (!entry) return nullptr;

                const lux_script_module_desc* desc = entry();
                if (!desc) return nullptr;
                if (desc->abi_version != LUX_SCRIPT_ABI_VERSION) return nullptr;

                return std::make_unique<NativeModule>(std::move(lib), desc);
            }
        };
    }

    std::unique_ptr<IScriptBackend> create()
    {
        return std::make_unique<NativeBackend>();
    }
}
