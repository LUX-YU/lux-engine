#include <lux/engine/function/script/native/NativeModule.hpp>

#include <lux/engine/dynamic_library/DynamicLibrary.hpp>

#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace lux::script
{
    using lux::engine::platform::DynamicLibrary;

    struct NativeModule::State
    {
        DynamicLibrary library;
        const lux_script_module_desc* descriptor{nullptr};
        std::unordered_map<std::string_view, const lux_script_function_desc*>
            functions;
    };

    namespace
    {
        struct ResolverContext
        {
            HostSymbolResolver* resolver{nullptr};
        };

        void* resolveHostSymbol(void* context, const char* name)
        {
            auto* resolver = static_cast<ResolverContext*>(context);
            if (!resolver || !resolver->resolver || !*resolver->resolver || !name)
                return nullptr;
            return (*resolver->resolver)(std::string_view{name});
        }

        ScriptResult<NativeModule> finalizeModule(
            DynamicLibrary library,
            HostSymbolResolver resolver
        )
        {
            using EntryFn = const lux_script_module_desc* (*)();
            const auto entry = reinterpret_cast<EntryFn>(
                library.get_symbol(LUX_SCRIPT_MODULE_ENTRY)
            );
            if (!entry)
            {
                return lux::cxx::unexpected(scriptFailure(
                    EScriptError::INVALID_ENTRY_POINT,
                    "native script does not export " LUX_SCRIPT_MODULE_ENTRY
                ));
            }

            const auto* descriptor = entry();
            if (!descriptor)
            {
                return lux::cxx::unexpected(scriptFailure(
                    EScriptError::INVALID_MODULE,
                    "native script entry returned a null module descriptor"
                ));
            }
            if (descriptor->abi_version != LUX_SCRIPT_ABI_VERSION)
            {
                return lux::cxx::unexpected(scriptFailure(
                    EScriptError::ABI_MISMATCH,
                    "native script ABI version does not match the host"
                ));
            }
            if (!descriptor->module_name || descriptor->module_name[0] == '\0')
            {
                return lux::cxx::unexpected(scriptFailure(
                    EScriptError::INVALID_MODULE,
                    "native script module name is empty"
                ));
            }
            if (descriptor->function_count != 0 && !descriptor->functions)
            {
                return lux::cxx::unexpected(scriptFailure(
                    EScriptError::INVALID_MODULE,
                    "native script function table is null"
                ));
            }

            auto state = std::make_unique<NativeModule::State>();
            state->library = std::move(library);
            state->descriptor = descriptor;
            state->functions.reserve(descriptor->function_count);

            for (std::uint32_t i = 0; i < descriptor->function_count; ++i)
            {
                const auto& function = descriptor->functions[i];
                if (!function.name || function.name[0] == '\0' || !function.invoke)
                {
                    return lux::cxx::unexpected(scriptFailure(
                        EScriptError::INVALID_MODULE,
                        "native script function entry is incomplete"
                    ));
                }
                if ((function.arg_count != 0 && !function.args)
                    || (function.return_count != 0 && !function.returns))
                {
                    return lux::cxx::unexpected(scriptFailure(
                        EScriptError::INVALID_MODULE,
                        "native script function signature table is incomplete"
                    ));
                }
                if (!state->functions.emplace(function.name, &function).second)
                {
                    return lux::cxx::unexpected(scriptFailure(
                        EScriptError::INVALID_MODULE,
                        "native script exports a duplicate function name"
                    ));
                }
            }

            if (const auto bind = reinterpret_cast<lux_script_bind_host_fn>(
                    state->library.get_symbol(LUX_SCRIPT_BIND_HOST_ENTRY)
                ))
            {
                ResolverContext context{&resolver};
                if (bind(
                        &resolveHostSymbol,
                        &context,
                        LUX_SCRIPT_ABI_VERSION
                    ) != 0)
                {
                    return lux::cxx::unexpected(scriptFailure(
                        EScriptError::HOST_BIND_FAILED,
                        "native script host-symbol binding failed"
                    ));
                }
            }

            return NativeModule(std::move(state));
        }
    }

    NativeModule::NativeModule(std::unique_ptr<State> state) noexcept
        : state_(std::move(state))
    {}

    NativeModule::NativeModule(NativeModule&&) noexcept = default;
    NativeModule& NativeModule::operator=(NativeModule&&) noexcept = default;
    NativeModule::~NativeModule() = default;

    std::string_view NativeModule::name() const noexcept
    {
        return state_ && state_->descriptor
            ? std::string_view{state_->descriptor->module_name}
            : std::string_view{};
    }

    std::span<const lux_script_function_desc>
    NativeModule::functions() const noexcept
    {
        if (!state_ || !state_->descriptor || state_->descriptor->function_count == 0)
            return {};
        return {
            state_->descriptor->functions,
            state_->descriptor->function_count
        };
    }

    const lux_script_function_desc*
    NativeModule::findFunction(std::string_view name) const noexcept
    {
        if (!state_)
            return nullptr;
        const auto it = state_->functions.find(name);
        return it == state_->functions.end() ? nullptr : it->second;
    }

    ScriptResult<NativeModule> loadNativeModule(
        const std::filesystem::path& path,
        HostSymbolResolver resolver
    )
    {
        DynamicLibrary library;
        if (!library.load(path))
        {
            return lux::cxx::unexpected(scriptFailure(
                EScriptError::IO_ERROR,
                "cannot load native script '" + path.string()
                    + "': " + library.last_error()
            ));
        }
        return finalizeModule(std::move(library), std::move(resolver));
    }

    ScriptResult<NativeModule> loadNativeModule(
        std::span<const std::byte> image,
        std::string_view module_name,
        HostSymbolResolver resolver
    )
    {
        if (image.empty() || module_name.empty())
        {
            return lux::cxx::unexpected(scriptFailure(
                EScriptError::INVALID_ARGUMENT,
                "native script image and module name must be non-empty"
            ));
        }

        DynamicLibrary library;
        if (!library.load_from_memory(image, module_name))
        {
            return lux::cxx::unexpected(scriptFailure(
                EScriptError::LOAD_FAILED,
                "cannot load native script memory image '"
                    + std::string(module_name) + "': " + library.last_error()
            ));
        }
        return finalizeModule(std::move(library), std::move(resolver));
    }
}
