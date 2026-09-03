#include <lux/engine/function/script/native/NativeModule.hpp>

#include <lux/engine/dynamic_library/DynamicLibrary.hpp>
#include <lux/engine/function/script/ScriptApi.hpp>

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
        std::unordered_map<std::string_view, const lux_script_function_desc*> functions_by_name;
        std::unordered_map<ScriptSymbolId, const lux_script_function_desc*> functions_by_symbol;
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
            const bool is_missing_context = resolver == nullptr;
            const bool is_missing_resolver = !is_missing_context && resolver->resolver == nullptr;
            const bool is_empty_resolver = !is_missing_resolver && !*resolver->resolver;
            const bool is_missing_name = name == nullptr;
            const bool is_invalid_request = is_missing_context || is_missing_resolver || is_empty_resolver ||
                is_missing_name;
            if (is_invalid_request)
                return nullptr;
            return (*resolver->resolver)(std::string_view{name});
        }

        ScriptResult<NativeModule> finalizeModule(DynamicLibrary library, HostSymbolResolver resolver)
        {
            using EntryFn = const lux_script_module_desc* (*)();
            const auto entry = reinterpret_cast<EntryFn>(library.get_symbol(LUX_SCRIPT_MODULE_ENTRY));
            if (!entry)
            {
                return lux::cxx::unexpected(scriptFailure(
                    EScriptError::INVALID_ENTRY_POINT,
                    "native script does not export " LUX_SCRIPT_MODULE_ENTRY)
                );
            }

            const auto* descriptor = entry();
            if (!descriptor)
            {
                return lux::cxx::unexpected(scriptFailure(
                    EScriptError::INVALID_MODULE,
                    "native script entry returned a null module descriptor")
                );
            }
            if (descriptor->abi_version != LUX_SCRIPT_ABI_VERSION)
            {
                return lux::cxx::unexpected(
                    scriptFailure(EScriptError::ABI_MISMATCH, "native script ABI version does not match the host")
                );
            }
            if (!descriptor->module_name || descriptor->module_name[0] == '\0')
            {
                return lux::cxx::unexpected(
                    scriptFailure(EScriptError::INVALID_MODULE, "native script module name is empty")
                );
            }
            if (descriptor->function_count != 0 && !descriptor->functions)
            {
                return lux::cxx::unexpected(
                    scriptFailure(EScriptError::INVALID_MODULE, "native script function table is null")
                );
            }
            if (descriptor->ability_import_count != 0U && descriptor->ability_imports == nullptr)
            {
                return lux::cxx::unexpected(
                    scriptFailure(EScriptError::INVALID_MODULE, "native script Ability import table is null")
                );
            }
            if (descriptor->state_align == 0U || (descriptor->state_align & (descriptor->state_align - 1U)) != 0U)
            {
                return lux::cxx::unexpected(
                    scriptFailure(EScriptError::INVALID_MODULE, "native script state alignment is invalid")
                );
            }

            auto state = std::make_unique<NativeModule::State>();
            state->library = std::move(library);
            state->descriptor = descriptor;
            state->functions_by_name.reserve(descriptor->function_count);
            state->functions_by_symbol.reserve(descriptor->function_count);

            for (std::uint32_t i = 0; i < descriptor->function_count; ++i)
            {
                const auto& function = descriptor->functions[i];
                const bool is_missing_name = function.name == nullptr;
                const bool is_empty_name = !is_missing_name && function.name[0] == '\0';
                const bool is_invalid_symbol = function.symbol_id == InvalidScriptSymbolId;
                const bool is_missing_invoke = function.invoke == nullptr && function.step == nullptr;
                const bool is_invalid_function = is_missing_name || is_empty_name || is_invalid_symbol ||
                    is_missing_invoke;
                if (is_invalid_function)
                {
                    return lux::cxx::unexpected(
                        scriptFailure(EScriptError::INVALID_MODULE, "native script function entry is incomplete")
                    );
                }
                if (function.step != nullptr)
                {
                    const auto& step = *function.step;
                    const bool is_invalid_frame = step.frame_size == 0U || step.frame_align == 0U ||
                        (step.frame_align & (step.frame_align - 1U)) != 0U || step.frame_layout_hash == 0U;
                    const bool is_missing_step_function = step.start == nullptr || step.resume == nullptr ||
                        step.destroy == nullptr;
                    if (is_invalid_frame || is_missing_step_function)
                    {
                        return lux::cxx::unexpected(
                            scriptFailure(EScriptError::INVALID_MODULE, "native script step entry is invalid")
                        );
                    }
                }
                const bool is_missing_arguments = function.arg_count != 0 && function.args == nullptr;
                const bool is_missing_returns = function.return_count != 0 && function.returns == nullptr;
                const bool is_invalid_signature = is_missing_arguments || is_missing_returns;
                if (is_invalid_signature)
                {
                    return lux::cxx::unexpected(scriptFailure(
                        EScriptError::INVALID_MODULE,
                        "native script function signature table is incomplete")
                    );
                }
                const auto valid_type = [](const lux_script_type_desc& type, bool is_return) noexcept {
                    const bool has_valid_name = type.name && type.name[0] != '\0';
                    const bool is_invalid_identity = !has_valid_name || type.type_id == InvalidScriptSymbolId ||
                        type.type_id != lux::semantic::typeId(type.name);
                    const bool is_invalid_layout = type.size == 0U || type.align == 0U ||
                        (type.align & (type.align - 1U)) != 0U;
                    const bool is_invalid_abi_kind = type.kind < LUX_SCRIPT_VK_BOOL ||
                        type.kind > LUX_SCRIPT_VK_STRUCT_REF;
                    const bool is_invalid_pass = type.pass > LUX_SCRIPT_PASS_CONST_REF ||
                        (is_return && type.pass != LUX_SCRIPT_PASS_VALUE);
                    const bool is_invalid_type = is_invalid_identity || is_invalid_layout ||
                        is_invalid_abi_kind || is_invalid_pass;
                    if (is_invalid_type)
                    {
                        return false;
                    }
                    if (const auto* builtin = lux::semantic::builtinLayout(type.type_id))
                    {
                        const bool is_invalid_builtin = builtin->canonical_name != type.name ||
                            builtin->abi_kind != type.kind || builtin->size != type.size ||
                            builtin->alignment != type.align;
                        return !is_invalid_builtin;
                    }
                    const bool is_portable_custom_scalar = type.kind >= LUX_SCRIPT_VK_BOOL &&
                        type.kind <= LUX_SCRIPT_VK_DOUBLE;
                    return is_portable_custom_scalar || type.kind == LUX_SCRIPT_VK_STRUCT_REF;
                };
                for (std::uint32_t argument{}; argument < function.arg_count; ++argument)
                {
                    if (!valid_type(function.args[argument], false))
                    {
                        return lux::cxx::unexpected(
                            scriptFailure(EScriptError::INVALID_MODULE, "native script argument type is invalid")
                        );
                    }
                }
                for (std::uint32_t result{}; result < function.return_count; ++result)
                {
                    if (!valid_type(function.returns[result], true))
                    {
                        return lux::cxx::unexpected(
                            scriptFailure(EScriptError::INVALID_MODULE, "native script return type is invalid")
                        );
                    }
                }
                const auto [name_iterator, inserted_name] = state->functions_by_name.emplace(function.name, &function);
                if (!inserted_name)
                {
                    // Names are diagnostic-only and overloads are legal. A
                    // diagnostic query must not pick an arbitrary overload.
                    name_iterator->second = nullptr;
                }
                if (!state->functions_by_symbol.emplace(function.symbol_id, &function).second)
                {
                    return lux::cxx::unexpected(
                        scriptFailure(EScriptError::INVALID_MODULE, "native script exports a duplicate symbol id")
                    );
                }
            }

            std::unordered_set<std::uint64_t> import_methods;
            import_methods.reserve(descriptor->ability_import_count);
            for (std::uint32_t index{}; index < descriptor->ability_import_count; ++index)
            {
                const auto& import = descriptor->ability_imports[index];
                const bool is_invalid_identity = import.contract_name == nullptr || import.method_name == nullptr ||
                    import.contract_name[0] == '\0' || import.method_name[0] == '\0' || import.contract_id == 0U ||
                    import.method_id == 0U || import.contract_id != lux::semantic::typeId(import.contract_name) ||
                    import.method_id != lux::semantic::typeId(import.method_name);
                const bool is_invalid_schema = import.schema_hash == 0U || import.schema_version == 0U ||
                    import.method_kind > 2U;
                const bool is_command_with_results =
                    import.method_kind == static_cast<std::uint8_t>(EScriptApiMethodKind::COMMAND) &&
                    import.result_count != 0U;
                const bool is_missing_signature = (import.arg_count != 0U && import.args == nullptr) ||
                    (import.result_count != 0U && import.results == nullptr);
                if (is_invalid_identity || is_invalid_schema || is_command_with_results || is_missing_signature ||
                    !import_methods.insert(import.method_id).second)
                {
                    return lux::cxx::unexpected(
                        scriptFailure(EScriptError::INVALID_MODULE, "native script Ability import is invalid")
                    );
                }
                const auto valid_import_type = [](const lux_script_type_desc& type) noexcept {
                    const bool is_invalid_identity = type.name == nullptr || type.name[0] == '\0' ||
                        type.type_id == 0U || type.type_id != lux::semantic::typeId(type.name);
                    const bool is_invalid_layout = type.size == 0U || type.align == 0U ||
                        (type.align & (type.align - 1U)) != 0U;
                    const bool is_invalid_abi = type.kind < LUX_SCRIPT_VK_BOOL ||
                        type.kind > LUX_SCRIPT_VK_STRUCT_REF || type.pass > LUX_SCRIPT_PASS_CONST_REF;
                    return !is_invalid_identity && !is_invalid_layout && !is_invalid_abi;
                };
                for (std::uint32_t argument{}; argument < import.arg_count; ++argument)
                {
                    if (!valid_import_type(import.args[argument]))
                    {
                        return lux::cxx::unexpected(
                            scriptFailure(EScriptError::INVALID_MODULE, "native Script Ability argument is invalid")
                        );
                    }
                }
                for (std::uint32_t output{}; output < import.result_count; ++output)
                {
                    const bool is_value = import.results[output].pass == LUX_SCRIPT_PASS_VALUE;
                    const bool is_borrowed_query =
                        import.method_kind == static_cast<std::uint8_t>(EScriptApiMethodKind::QUERY) &&
                        import.results[output].pass == LUX_SCRIPT_PASS_CONST_REF;
                    if (!valid_import_type(import.results[output]) || (!is_value && !is_borrowed_query))
                    {
                        return lux::cxx::unexpected(
                            scriptFailure(EScriptError::INVALID_MODULE, "native Script Ability result is invalid")
                        );
                    }
                }
            }

            if (const auto bind =
                    reinterpret_cast<lux_script_bind_host_fn>(state->library.get_symbol(LUX_SCRIPT_BIND_HOST_ENTRY)))
            {
                ResolverContext context{&resolver};
                if (bind(&resolveHostSymbol, &context, LUX_SCRIPT_ABI_VERSION) != 0)
                {
                    return lux::cxx::unexpected(
                        scriptFailure(EScriptError::HOST_BIND_FAILED, "native script host-symbol binding failed")
                    );
                }
            }

            return NativeModule(std::move(state));
        }
    }

    NativeModule::NativeModule(std::unique_ptr<State> state) noexcept : state_(std::move(state))
    {
    }

    NativeModule::NativeModule(NativeModule&&) noexcept = default;
    NativeModule& NativeModule::operator=(NativeModule&&) noexcept = default;
    NativeModule::~NativeModule() = default;

    std::string_view NativeModule::name() const noexcept
    {
        return state_ && state_->descriptor ? std::string_view{state_->descriptor->module_name} : std::string_view{};
    }

    std::span<const lux_script_function_desc> NativeModule::functions() const noexcept
    {
        if (!state_ || !state_->descriptor || state_->descriptor->function_count == 0)
            return {};
        return {state_->descriptor->functions, state_->descriptor->function_count};
    }

    std::span<const lux_script_ability_import_desc> NativeModule::abilityImports() const noexcept
    {
        if (!state_ || !state_->descriptor || state_->descriptor->ability_import_count == 0U)
            return {};
        return {state_->descriptor->ability_imports, state_->descriptor->ability_import_count};
    }

    const lux_script_function_desc* NativeModule::findFunction(std::string_view name) const noexcept
    {
        if (!state_)
            return nullptr;
        const auto it = state_->functions_by_name.find(name);
        return it == state_->functions_by_name.end() ? nullptr : it->second;
    }

    const lux_script_function_desc* NativeModule::findFunction(ScriptSymbolId symbol) const noexcept
    {
        if (!state_ || symbol == InvalidScriptSymbolId)
            return nullptr;
        const auto it = state_->functions_by_symbol.find(symbol);
        return it == state_->functions_by_symbol.end() ? nullptr : it->second;
    }

    std::uint32_t NativeModule::abiVersion() const noexcept
    {
        return state_ && state_->descriptor ? state_->descriptor->abi_version : 0U;
    }

    std::uint64_t NativeModule::stateLayoutHash() const noexcept
    {
        return state_ && state_->descriptor ? state_->descriptor->state_layout_hash : 0U;
    }

    std::uint32_t NativeModule::stateSize() const noexcept
    {
        return state_ && state_->descriptor ? state_->descriptor->state_size : 0U;
    }

    std::uint32_t NativeModule::stateAlignment() const noexcept
    {
        return state_ && state_->descriptor ? state_->descriptor->state_align : 0U;
    }

    ScriptResult<NativeModule> loadNativeModule(const std::filesystem::path& path, HostSymbolResolver resolver)
    {
        DynamicLibrary library;
        if (!library.load(path))
        {
            return lux::cxx::unexpected(scriptFailure(
                EScriptError::IO_ERROR,
                "cannot load native script '" + path.string() + "': " + library.last_error())
            );
        }
        return finalizeModule(std::move(library), std::move(resolver));
    }

    ScriptResult<NativeModule>
    loadNativeModule(std::span<const std::byte> image, std::string_view module_name, HostSymbolResolver resolver)
    {
        if (image.empty() || module_name.empty())
        {
            return lux::cxx::unexpected(
                scriptFailure(EScriptError::INVALID_ARGUMENT, "native script image and module name must be non-empty")
            );
        }

        DynamicLibrary library;
        if (!library.load_from_memory(image, module_name))
        {
            return lux::cxx::unexpected(scriptFailure(
                EScriptError::LOAD_FAILED,
                "cannot load native script memory image '" + std::string(module_name) + "': " + library.last_error())
            );
        }
        return finalizeModule(std::move(library), std::move(resolver));
    }
}
