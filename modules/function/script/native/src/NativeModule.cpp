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
            functions_by_name;
        std::unordered_map<ScriptSymbolId, const lux_script_function_desc*>
            functions_by_symbol;
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
            if (descriptor->state_align == 0U ||
                (descriptor->state_align & (descriptor->state_align - 1U)) != 0U)
            {
                return lux::cxx::unexpected(scriptFailure(
                    EScriptError::INVALID_MODULE,
                    "native script state alignment is invalid"
                ));
            }

            auto state = std::make_unique<NativeModule::State>();
            state->library = std::move(library);
            state->descriptor = descriptor;
            state->functions_by_name.reserve(descriptor->function_count);
            state->functions_by_symbol.reserve(descriptor->function_count);

            for (std::uint32_t i = 0; i < descriptor->function_count; ++i)
            {
                const auto& function = descriptor->functions[i];
                if (!function.name || function.name[0] == '\0' ||
                    function.symbol_id == InvalidScriptSymbolId ||
                    !function.invoke)
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
                const auto valid_type = [](const lux_script_type_desc& type,
                                           bool is_return) noexcept
                {
                    if (!type.name || type.name[0] == '\0' ||
                        type.type_id == InvalidScriptSymbolId ||
                        type.type_id != scriptSemanticTypeId(type.name) ||
                        type.size == 0U || type.align == 0U ||
                        (type.align & (type.align - 1U)) != 0U ||
                        type.kind < LUX_SCRIPT_VK_BOOL ||
                        type.kind > LUX_SCRIPT_VK_STRUCT_REF ||
                        type.pass > LUX_SCRIPT_PASS_CONST_REF ||
                        (is_return && type.pass != LUX_SCRIPT_PASS_VALUE))
                    {
                        return false;
                    }
                    if (const auto* builtin = scriptBuiltinLayout(type.type_id))
                    {
                        return builtin->canonical_name == type.name &&
                            builtin->abi_kind == type.kind &&
                            builtin->size == type.size &&
                            builtin->alignment == type.align;
                    }
                    return type.kind == LUX_SCRIPT_VK_STRUCT_REF;
                };
                for (std::uint32_t argument{};
                     argument < function.arg_count; ++argument)
                {
                    if (!valid_type(function.args[argument], false))
                    {
                        return lux::cxx::unexpected(scriptFailure(
                            EScriptError::INVALID_MODULE,
                            "native script argument type is invalid"
                        ));
                    }
                }
                for (std::uint32_t result{};
                     result < function.return_count; ++result)
                {
                    if (!valid_type(function.returns[result], true))
                    {
                        return lux::cxx::unexpected(scriptFailure(
                            EScriptError::INVALID_MODULE,
                            "native script return type is invalid"
                        ));
                    }
                }
                const auto [name_iterator, inserted_name] =
                    state->functions_by_name.emplace(
                        function.name,
                        &function
                    );
                if (!inserted_name)
                {
                    // Names are diagnostic-only and overloads are legal. A
                    // diagnostic query must not pick an arbitrary overload.
                    name_iterator->second = nullptr;
                }
                if (!state->functions_by_symbol.emplace(
                        function.symbol_id,
                        &function
                    ).second)
                {
                    return lux::cxx::unexpected(scriptFailure(
                        EScriptError::INVALID_MODULE,
                        "native script exports a duplicate symbol id"
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
        const auto it = state_->functions_by_name.find(name);
        return it == state_->functions_by_name.end() ? nullptr : it->second;
    }

    const lux_script_function_desc* NativeModule::findFunction(
        ScriptSymbolId symbol
    ) const noexcept
    {
        if (!state_ || symbol == InvalidScriptSymbolId)
            return nullptr;
        const auto it = state_->functions_by_symbol.find(symbol);
        return it == state_->functions_by_symbol.end() ? nullptr : it->second;
    }

    std::uint32_t NativeModule::abiVersion() const noexcept
    {
        return state_ && state_->descriptor
            ? state_->descriptor->abi_version
            : 0U;
    }

    std::uint64_t NativeModule::stateLayoutHash() const noexcept
    {
        return state_ && state_->descriptor
            ? state_->descriptor->state_layout_hash
            : 0U;
    }

    std::uint32_t NativeModule::stateSize() const noexcept
    {
        return state_ && state_->descriptor
            ? state_->descriptor->state_size
            : 0U;
    }

    std::uint32_t NativeModule::stateAlignment() const noexcept
    {
        return state_ && state_->descriptor
            ? state_->descriptor->state_align
            : 0U;
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
