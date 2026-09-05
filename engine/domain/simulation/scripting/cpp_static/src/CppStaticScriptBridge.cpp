#include <lux/engine/simulation/scripting/cpp_static/CppStaticScriptBridge.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp>

#include <algorithm>
#include <cstring>
#include <limits>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lux::simulation::script
{
    lux::script::ScriptAbilityCoroutine<DelayAbility, ScriptCoroutineContext>
    ScriptCoroutineContext::delay() noexcept
    {
        const auto slot = findAbility(lux::script::ScriptAbilityTraits<DelayAbility>::Description.id.hash());
        return {
            *this,
            slot.value_or((std::numeric_limits<std::uint32_t>::max)())
        };
    }

    namespace
    {
        struct OwnedReference final
        {
            std::size_t parameter{};
            std::size_t offset{};
            std::size_t size{};
        };

        struct Callable final
        {
            lux::script::ScriptSymbolId symbol{};
            const lux::meta::RefMethod* method{};
            const lux::meta::RefFunction* function{};
            CppStaticCoroutineExport::InvokeFn coroutine_invoke{};
            CppStaticMethodExport::InvokeFn typed_invoke{};
            const lux::rdesc::ScriptFunction* signature{};
            std::vector<OwnedReference> references;
            std::size_t reference_bytes{};
            std::size_t reference_alignment{1U};
        };

        [[nodiscard]] const lux::semantic::Layout* builtin(
            const lux::meta::RefType& type,
            std::uint64_t value_type_hash
        ) noexcept
        {
            struct Mapping final
            {
                lux::meta::EBaseType base;
                std::uint64_t type_hash{};
                lux::semantic::Layout layout;
            };
#define LUX_META_BASE_BOOL Bool
#define LUX_META_BASE_I32 Int32
#define LUX_META_BASE_U32 Uint32
#define LUX_META_BASE_I64 Int64
#define LUX_META_BASE_U64 Uint64
#define LUX_META_BASE_F32 Float
#define LUX_META_BASE_F64 Double
#define LUX_META_BASE_VALUE(tag) LUX_META_BASE_##tag
#define LUX_SEMANTIC_BUILTIN(tag, cpp_type, canonical, abi_kind_value)         \
            Mapping{                                                          \
                lux::meta::EBaseType::LUX_META_BASE_VALUE(tag),                \
                lux::cxx::type_hash<cpp_type>(),                               \
                lux::semantic::Layout{                                         \
                    lux::semantic::typeId(canonical),                          \
                    canonical,                                                 \
                    abi_kind_value,                                            \
                    sizeof(cpp_type),                                          \
                    alignof(cpp_type)}},
            static const auto mappings = std::array{
#include <lux/engine/core/semantic/SemanticBuiltin.def>
            };
#undef LUX_SEMANTIC_BUILTIN
#undef LUX_META_BASE_VALUE
#undef LUX_META_BASE_F64
#undef LUX_META_BASE_F32
#undef LUX_META_BASE_U64
#undef LUX_META_BASE_I64
#undef LUX_META_BASE_U32
#undef LUX_META_BASE_I32
#undef LUX_META_BASE_BOOL
            const auto found = std::find_if(
                mappings.begin(),
                mappings.end(),
                [&type, value_type_hash](const Mapping& mapping) noexcept
                {
                    return mapping.base == static_cast<lux::meta::EBaseType>(
                        type.qtype.base
                    ) && mapping.type_hash == value_type_hash;
                }
            );
            return found == mappings.end() ? nullptr : std::addressof(found->layout);
        }

        [[nodiscard]] lux::cxx::expected<
            lux::rdesc::ScriptValueType,
            ECppStaticScriptBridgeError> projectType(
                const lux::meta::RefType& type,
                std::uint64_t value_type_hash,
                bool is_return,
                CppStaticRecordSemanticResolver resolver
            ) noexcept
        {
            const auto qualifier = static_cast<lux::meta::ETypeQual>(
                type.qtype.qual
            );
            auto pass = lux::semantic::EValuePass::VALUE;
            switch (qualifier)
            {
            case lux::meta::ETypeQual::Value:
                break;
            case lux::meta::ETypeQual::LRefToConst:
                if (is_return)
                {
                    return lux::cxx::unexpected(
                        ECppStaticScriptBridgeError::RETURN_NOT_SUPPORTED
                    );
                }
                pass = lux::semantic::EValuePass::CONST_REF;
                break;
            case lux::meta::ETypeQual::LRef:
                return lux::cxx::unexpected(
                    ECppStaticScriptBridgeError::MUTABLE_REFERENCE_NOT_SUPPORTED
                );
            case lux::meta::ETypeQual::RRef:
            case lux::meta::ETypeQual::RRefToConst:
                return lux::cxx::unexpected(
                    ECppStaticScriptBridgeError::RVALUE_REFERENCE_NOT_SUPPORTED
                );
            default:
                return lux::cxx::unexpected(
                    ECppStaticScriptBridgeError::POINTER_NOT_SUPPORTED
                );
            }

            if (const auto* layout = builtin(type, value_type_hash))
            {
                return lux::rdesc::ScriptValueType{
                    std::string{layout->canonical_name},
                    layout->type_id,
                    pass,
                    layout->abi_kind,
                    layout->size,
                    layout->alignment};
            }
            // Non-builtin records and enums have no portable Script identity
            // until the caller explicitly supplies one.  Lifecycle stop
            // reasons are an enum, so restricting this seam to Record would
            // make the canonical STOP signature impossible to project.
            if (!resolver.resolve)
            {
                return lux::cxx::unexpected(
                    ECppStaticScriptBridgeError::UNSUPPORTED_TYPE
                );
            }
            lux::semantic::Layout layout;
            if (!resolver.resolve(resolver.context, type, layout) ||
                layout.type_id == 0U || layout.canonical_name.empty() ||
                layout.type_id != lux::semantic::typeId(
                    layout.canonical_name) ||
                layout.size != type.size || layout.alignment != type.alignment)
            {
                return lux::cxx::unexpected(
                    ECppStaticScriptBridgeError::UNSUPPORTED_TYPE
                );
            }
            return lux::rdesc::ScriptValueType{
                std::string{layout.canonical_name},
                layout.type_id,
                pass,
                layout.abi_kind,
                layout.size,
                layout.alignment};
        }

        [[nodiscard]] lux::cxx::expected<
            lux::rdesc::ScriptFunction,
            ECppStaticScriptBridgeError> projectInvokable(
                const lux::meta::RefInvokable& invokable,
                lux::script::ScriptSymbolId assigned_symbol,
                bool is_noexcept,
                CppStaticRecordSemanticResolver resolver
            ) noexcept
        {
            if (assigned_symbol == lux::script::InvalidScriptSymbolId)
            {
                return lux::cxx::unexpected(
                    ECppStaticScriptBridgeError::INVALID_DESCRIPTOR
                );
            }
            if (!is_noexcept)
            {
                return lux::cxx::unexpected(
                    ECppStaticScriptBridgeError::METHOD_NOT_NOEXCEPT
                );
            }
            if (!invokable.invoker)
            {
                return lux::cxx::unexpected(
                    ECppStaticScriptBridgeError::MISSING_INVOKER
                );
            }
            if (invokable.is_variadic)
            {
                return lux::cxx::unexpected(
                    ECppStaticScriptBridgeError::VARIADIC_NOT_SUPPORTED
                );
            }
            try
            {
                lux::rdesc::ScriptFunction projected;
                projected.name = invokable.name;
                projected.args.reserve(invokable.parameters.size());
                for (const auto& parameter : invokable.parameters)
                {
                    auto type = projectType(
                        parameter.type,
                        parameter.value_type_hash,
                        false,
                        resolver
                    );
                    if (!type)
                        return lux::cxx::unexpected(type.error());
                    projected.args.push_back(std::move(*type));
                }
                const auto return_base = static_cast<lux::meta::EBaseType>(
                    invokable.return_type.qtype.base
                );
                if (return_base != lux::meta::EBaseType::Void)
                {
                    auto type = projectType(
                        invokable.return_type,
                        invokable.return_type.hash,
                        true,
                        resolver
                    );
                    if (!type)
                        return lux::cxx::unexpected(type.error());
                    projected.returns.push_back(std::move(*type));
                }
                projected.symbol_id = assigned_symbol;
                return projected;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(
                    ECppStaticScriptBridgeError::ALLOCATION_FAILURE
                );
            }
        }

        [[nodiscard]] lux::cxx::expected<
            lux::rdesc::ScriptFunction,
            ECppStaticScriptBridgeError> projectCoroutineInvokable(
                const CppStaticCoroutineExport& coroutine,
                bool is_noexcept,
                CppStaticRecordSemanticResolver resolver
            ) noexcept
        {
            const auto* invokable = coroutine.method != nullptr
                ? std::addressof(coroutine.method->invokable)
                : coroutine.function != nullptr
                    ? std::addressof(coroutine.function->invokable)
                    : nullptr;
            const bool is_invalid_signature = invokable == nullptr || coroutine.invoke == nullptr ||
                coroutine.symbol == lux::script::InvalidScriptSymbolId || !is_noexcept || invokable->is_variadic ||
                invokable->return_type.hash != lux::cxx::type_hash<ScriptCoroutine>() ||
                invokable->parameters.size() != coroutine.visible_parameter_count + 1U;
            if (is_invalid_signature)
                return lux::cxx::unexpected(ECppStaticScriptBridgeError::INVALID_DESCRIPTOR);

            const auto& hidden_context = invokable->parameters.front();
            const auto context_qualifier = static_cast<lux::meta::ETypeQual>(hidden_context.type.qtype.qual);
            const bool is_invalid_context = hidden_context.value_type_hash !=
                    lux::cxx::type_hash<ScriptCoroutineContext>() ||
                context_qualifier != lux::meta::ETypeQual::LRef;
            if (is_invalid_context)
                return lux::cxx::unexpected(ECppStaticScriptBridgeError::INVALID_DESCRIPTOR);

            try
            {
                lux::rdesc::ScriptFunction projected;
                projected.name = invokable->name;
                projected.symbol_id = coroutine.symbol;
                projected.args.reserve(coroutine.visible_parameter_count);
                for (std::size_t index{1U}; index < invokable->parameters.size(); ++index)
                {
                    const auto& parameter = invokable->parameters[index];
                    auto type = projectType(parameter.type, parameter.value_type_hash, false, resolver);
                    if (!type)
                        return lux::cxx::unexpected(type.error());
                    projected.args.push_back(std::move(*type));
                }
                return projected;
            }
            catch (const std::bad_alloc&)
            {
                return lux::cxx::unexpected(ECppStaticScriptBridgeError::ALLOCATION_FAILURE);
            }
        }
    }

    struct CppStaticScriptDescriptor::State final
    {
        lux::rdesc::Script description;
        std::string descriptor_key;
        const lux::meta::RefClass* reflected_class{};
        void (*attach)(void*, ScriptBehavior&) noexcept{};
        bool entity_scope{};
        std::vector<Callable> callables;
    };

    namespace
    {
        [[nodiscard]] lux::cxx::expected<void, ECppStaticScriptBridgeError> prepareCallPlans(
            CppStaticScriptDescriptor::State& state, std::span<const CppStaticMethodExport> typed_methods
        )
        {
            for (std::size_t index{}; index < state.callables.size(); ++index)
            {
                auto& call = state.callables[index];
                call.signature = &state.description.exports[index];
                if (call.signature->args.size() > 64U)
                    return lux::cxx::unexpected(ECppStaticScriptBridgeError::UNSUPPORTED_TYPE);
                if (call.coroutine_invoke == nullptr)
                    continue;
                for (std::size_t argument{}; argument < call.signature->args.size(); ++argument)
                {
                    const auto& type = call.signature->args[argument];
                    if (type.pass != lux::semantic::EValuePass::CONST_REF)
                        continue;
                    const auto alignment = static_cast<std::size_t>(type.alignment);
                    const auto maximum = (std::numeric_limits<std::size_t>::max)();
                    const bool is_invalid_alignment = alignment == 0U || (alignment & (alignment - 1U)) != 0U;
                    if (is_invalid_alignment || call.reference_bytes > maximum - (alignment - 1U))
                        return lux::cxx::unexpected(ECppStaticScriptBridgeError::UNSUPPORTED_TYPE);
                    const auto offset = (call.reference_bytes + alignment - 1U) & ~(alignment - 1U);
                    if (type.size > maximum - offset)
                        return lux::cxx::unexpected(ECppStaticScriptBridgeError::UNSUPPORTED_TYPE);
                    call.references.push_back({argument, offset, type.size});
                    call.reference_bytes = offset + type.size;
                    call.reference_alignment = (std::max)(call.reference_alignment, alignment);
                }
            }
            for (const auto& typed : typed_methods)
            {
                const auto found = std::ranges::find(state.callables, typed.symbol, &Callable::symbol);
                if (found == state.callables.end() || found->typed_invoke != nullptr ||
                    found->coroutine_invoke != nullptr || typed.invoke == nullptr || typed.matches == nullptr)
                    return lux::cxx::unexpected(ECppStaticScriptBridgeError::INVALID_DESCRIPTOR);
                const auto owner_hash = state.reflected_class != nullptr ? state.reflected_class->type.hash : 0U;
                if (typed.owner_type_hash != owner_hash || typed.method != found->method ||
                    typed.function != found->function)
                    return lux::cxx::unexpected(ECppStaticScriptBridgeError::INVALID_CLASS);
                const auto& reflection = found->method ? found->method->invokable : found->function->invokable;
                if (!typed.matches(reflection, *found->signature))
                    return lux::cxx::unexpected(ECppStaticScriptBridgeError::UNSUPPORTED_TYPE);
                found->typed_invoke = typed.invoke;
            }
            return {};
        }
    }

    CppStaticScriptDescriptor::CppStaticScriptDescriptor() noexcept = default;
    CppStaticScriptDescriptor::CppStaticScriptDescriptor(
        std::unique_ptr<State> state
    ) noexcept
        : state_(std::move(state))
    {
    }
    CppStaticScriptDescriptor::CppStaticScriptDescriptor(
        CppStaticScriptDescriptor&&
    ) noexcept = default;
    CppStaticScriptDescriptor& CppStaticScriptDescriptor::operator=(
        CppStaticScriptDescriptor&&
    ) noexcept = default;
    CppStaticScriptDescriptor::~CppStaticScriptDescriptor() = default;

    CppStaticScriptDescriptor::operator bool() const noexcept
    {
        return state_ != nullptr;
    }

    const lux::rdesc::Script& CppStaticScriptDescriptor::description() const
        noexcept
    {
        static const lux::rdesc::Script empty;
        return state_ ? state_->description : empty;
    }

    std::string_view CppStaticScriptDescriptor::key() const noexcept
    {
        return state_ ? std::string_view{state_->descriptor_key}
                      : std::string_view{};
    }

    lux::cxx::expected<
        CppStaticScriptDescriptor,
        ECppStaticScriptBridgeError> projectCppStaticEntityScript(
            std::string_view module_name,
            std::string_view descriptor_key,
            const lux::meta::RefClass& reflected_class,
            std::span<const lux::meta::RefMethod* const> methods,
            std::span<const lux::script::ScriptSymbolId> symbols,
            CppStaticRecordSemanticResolver record_types,
            void (*attach)(void*, ScriptBehavior&) noexcept,
            lux::rdesc::ScriptLifecycleRoles lifecycle,
            std::span<const CppStaticCoroutineExport> coroutines,
            std::span<const lux::rdesc::ScriptApiRequirement> abilities,
            std::span<const lux::script::ScriptEventSourceDescription> events,
            std::span<const CppStaticMethodExport> typed_methods
        ) noexcept
    {
        if (module_name.empty() || descriptor_key.empty() ||
            methods.size() != symbols.size() ||
            !reflected_class.construct || !reflected_class.destruct ||
            reflected_class.type.size == 0U ||
            reflected_class.type.alignment == 0U)
        {
            return lux::cxx::unexpected(
                ECppStaticScriptBridgeError::INVALID_DESCRIPTOR
            );
        }
        try
        {
            auto state = std::make_unique<CppStaticScriptDescriptor::State>();
            state->description.module_name = module_name;
            state->description.body = lux::rdesc::CppStaticScript{
                std::string{descriptor_key},
                {}
            };
            state->description.lifecycle = lifecycle;
            state->description.api_requirements.assign(abilities.begin(), abilities.end());
            state->description.event_requirements.assign(events.begin(), events.end());
            std::ranges::sort(
                state->description.api_requirements,
                {},
                [](const auto& requirement) noexcept
                {
                    return requirement.contract.hash();
                }
            );
            std::ranges::sort(
                state->description.event_requirements,
                lux::script::ScriptEventSourceLess{}
            );
            state->descriptor_key = descriptor_key;
            state->reflected_class = std::addressof(reflected_class);
            state->attach = attach;
            state->entity_scope = true;
            state->description.exports.reserve(methods.size() + coroutines.size());
            state->callables.reserve(methods.size() + coroutines.size());
            std::unordered_set<lux::script::ScriptSymbolId> symbols_seen;
            symbols_seen.reserve(methods.size() + coroutines.size());
            for (std::size_t method_index{}; method_index < methods.size();
                 ++method_index)
            {
                const auto* method = methods[method_index];
                if (!method || method->owner_class != &reflected_class ||
                    method->is_static)
                {
                    return lux::cxx::unexpected(
                        ECppStaticScriptBridgeError::INVALID_CLASS
                    );
                }
                if (method->visibility != lux::meta::EVisibility::Public)
                {
                    return lux::cxx::unexpected(
                        ECppStaticScriptBridgeError::METHOD_NOT_PUBLIC
                    );
                }
                auto projected = projectInvokable(
                    method->invokable,
                    symbols[method_index],
                    method->is_noexcept,
                    record_types
                );
                if (!projected)
                    return lux::cxx::unexpected(projected.error());
                const auto symbol = projected->symbol_id;
                if (!symbols_seen.emplace(symbol).second)
                {
                    return lux::cxx::unexpected(
                        ECppStaticScriptBridgeError::DUPLICATE_SYMBOL
                    );
                }
                state->description.exports.push_back(std::move(*projected));
                state->callables.push_back(Callable{symbol, method, nullptr, nullptr});
            }
            auto& body = std::get<lux::rdesc::CppStaticScript>(state->description.body);
            body.suspension_capable_exports.reserve(coroutines.size());
            for (const auto& coroutine : coroutines)
            {
                const auto* method = coroutine.method;
                const bool is_invalid_method = method == nullptr || coroutine.function != nullptr ||
                    method->owner_class != std::addressof(reflected_class) || method->is_static;
                if (is_invalid_method)
                    return lux::cxx::unexpected(ECppStaticScriptBridgeError::INVALID_CLASS);
                if (method->visibility != lux::meta::EVisibility::Public)
                    return lux::cxx::unexpected(ECppStaticScriptBridgeError::METHOD_NOT_PUBLIC);
                auto projected = projectCoroutineInvokable(coroutine, method->is_noexcept, record_types);
                if (!projected)
                    return lux::cxx::unexpected(projected.error());
                if (!symbols_seen.emplace(coroutine.symbol).second)
                    return lux::cxx::unexpected(ECppStaticScriptBridgeError::DUPLICATE_SYMBOL);
                state->description.exports.push_back(std::move(*projected));
                state->callables.push_back(Callable{
                    coroutine.symbol,
                    method,
                    nullptr,
                    coroutine.invoke
                });
                body.suspension_capable_exports.push_back(coroutine.symbol);
            }
            std::ranges::sort(body.suspension_capable_exports);
            const auto plans = prepareCallPlans(*state, typed_methods);
            if (!plans)
                return lux::cxx::unexpected(plans.error());
            if (!lux::rdesc::validScriptDescription(state->description))
                return lux::cxx::unexpected(ECppStaticScriptBridgeError::INVALID_DESCRIPTOR);
            return CppStaticScriptDescriptor{std::move(state)};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                ECppStaticScriptBridgeError::ALLOCATION_FAILURE
            );
        }
    }

    lux::cxx::expected<
        CppStaticScriptDescriptor,
        ECppStaticScriptBridgeError> projectCppStaticGlobalScript(
            std::string_view module_name,
            std::string_view descriptor_key,
            std::span<const lux::meta::RefFunction* const> functions,
            std::span<const lux::script::ScriptSymbolId> symbols,
            CppStaticRecordSemanticResolver record_types,
            lux::rdesc::ScriptLifecycleRoles lifecycle,
            std::span<const CppStaticCoroutineExport> coroutines,
            std::span<const lux::rdesc::ScriptApiRequirement> abilities,
            std::span<const lux::script::ScriptEventSourceDescription> events,
            std::span<const CppStaticMethodExport> typed_methods
        ) noexcept
    {
        if (module_name.empty() || descriptor_key.empty() ||
            functions.size() != symbols.size())
        {
            return lux::cxx::unexpected(
                ECppStaticScriptBridgeError::INVALID_DESCRIPTOR
            );
        }
        try
        {
            auto state = std::make_unique<CppStaticScriptDescriptor::State>();
            state->description.module_name = module_name;
            state->description.body = lux::rdesc::CppStaticScript{
                std::string{descriptor_key},
                {}
            };
            state->description.lifecycle = lifecycle;
            state->description.api_requirements.assign(abilities.begin(), abilities.end());
            state->description.event_requirements.assign(events.begin(), events.end());
            std::ranges::sort(
                state->description.api_requirements,
                {},
                [](const auto& requirement) noexcept
                {
                    return requirement.contract.hash();
                }
            );
            std::ranges::sort(
                state->description.event_requirements,
                lux::script::ScriptEventSourceLess{}
            );
            state->descriptor_key = descriptor_key;
            state->description.exports.reserve(functions.size() + coroutines.size());
            state->callables.reserve(functions.size() + coroutines.size());
            std::unordered_set<lux::script::ScriptSymbolId> symbols_seen;
            symbols_seen.reserve(functions.size() + coroutines.size());
            for (std::size_t function_index{};
                 function_index < functions.size(); ++function_index)
            {
                const auto* function = functions[function_index];
                if (!function)
                {
                    return lux::cxx::unexpected(
                        ECppStaticScriptBridgeError::INVALID_DESCRIPTOR
                    );
                }
                auto projected = projectInvokable(
                    function->invokable,
                    symbols[function_index],
                    function->is_noexcept,
                    record_types
                );
                if (!projected)
                {
                    if (projected.error() ==
                        ECppStaticScriptBridgeError::METHOD_NOT_NOEXCEPT)
                    {
                        return lux::cxx::unexpected(
                            ECppStaticScriptBridgeError::FUNCTION_NOT_NOEXCEPT
                        );
                    }
                    return lux::cxx::unexpected(projected.error());
                }
                const auto symbol = projected->symbol_id;
                if (!symbols_seen.emplace(symbol).second)
                {
                    return lux::cxx::unexpected(
                        ECppStaticScriptBridgeError::DUPLICATE_SYMBOL
                    );
                }
                state->description.exports.push_back(std::move(*projected));
                state->callables.push_back(Callable{symbol, nullptr, function, nullptr});
            }
            auto& body = std::get<lux::rdesc::CppStaticScript>(state->description.body);
            body.suspension_capable_exports.reserve(coroutines.size());
            for (const auto& coroutine : coroutines)
            {
                const auto* function = coroutine.function;
                if (function == nullptr || coroutine.method != nullptr)
                    return lux::cxx::unexpected(ECppStaticScriptBridgeError::INVALID_DESCRIPTOR);
                auto projected = projectCoroutineInvokable(coroutine, function->is_noexcept, record_types);
                if (!projected)
                    return lux::cxx::unexpected(projected.error());
                if (!symbols_seen.emplace(coroutine.symbol).second)
                    return lux::cxx::unexpected(ECppStaticScriptBridgeError::DUPLICATE_SYMBOL);
                state->description.exports.push_back(std::move(*projected));
                state->callables.push_back(Callable{
                    coroutine.symbol,
                    nullptr,
                    function,
                    coroutine.invoke
                });
                body.suspension_capable_exports.push_back(coroutine.symbol);
            }
            std::ranges::sort(body.suspension_capable_exports);
            const auto plans = prepareCallPlans(*state, typed_methods);
            if (!plans)
                return lux::cxx::unexpected(plans.error());
            if (!lux::rdesc::validScriptDescription(state->description))
                return lux::cxx::unexpected(ECppStaticScriptBridgeError::INVALID_DESCRIPTOR);
            return CppStaticScriptDescriptor{std::move(state)};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(
                ECppStaticScriptBridgeError::ALLOCATION_FAILURE
            );
        }
    }

    struct CppStaticScriptBackend::State final
    {
        struct ObjectSlab final
        {
            ObjectSlab() noexcept = default;
            ObjectSlab(const ObjectSlab&) = delete;
            ObjectSlab& operator=(const ObjectSlab&) = delete;

            ObjectSlab(ObjectSlab&& other) noexcept
                : data(std::exchange(other.data, nullptr)),
                  alignment(std::exchange(other.alignment, 0U))
            {
            }

            ObjectSlab& operator=(ObjectSlab&& other) noexcept
            {
                if (this == std::addressof(other))
                    return *this;
                reset();
                data = std::exchange(other.data, nullptr);
                alignment = std::exchange(other.alignment, 0U);
                return *this;
            }

            ~ObjectSlab()
            {
                reset();
            }

            void reset() noexcept
            {
                if (data)
                    ::operator delete(data, std::align_val_t{alignment});
                data = nullptr;
                alignment = 0U;
            }

            void* data{};
            std::size_t alignment{};
        };

        struct DescriptorIndex final
        {
            const CppStaticScriptDescriptor::State* descriptor{};
            std::unordered_map<
                lux::script::ScriptSymbolId,
                const Callable*> callables;
            ObjectSlab objects;
            std::vector<std::size_t> free_objects;
            detail::BoundedClassStorage coroutine_frames;
            std::size_t object_stride{};
            std::size_t instance_capacity{};
            std::size_t active_instances{};
            std::size_t coroutine_capacity{};
            std::size_t active_coroutines{};
            std::size_t coroutine_high_water{};
        };

        struct Instance final
        {
            State* owner{};
            DescriptorIndex* descriptor{};
            void* object{};
            std::size_t object_slot{(std::numeric_limits<std::size_t>::max)()};
            std::uint32_t slot{};
            std::span<const PreparedScriptApiCapability> capabilities;
            std::size_t active_coroutines{};
        };

        struct CoroutineContinuation final
        {
            State* owner{};
            DescriptorIndex* descriptor{};
            Instance* instance{};
            ScriptCoroutineContext context;
            detail::BoundedClassStorage::Allocation arguments;
            std::coroutine_handle<ScriptCoroutine::promise_type> handle;
            std::uint32_t slot{};
            bool active{};
        };

        struct PreparedCall final
        {
            Instance* instance{};
            const Callable* callable{};
            detail::BoundedClassStorage::ClassHandle argument_class;
            bool active{};

            static bool matchesSlots(const lux::rdesc::ScriptFunction& signature,
                const lux_script_call_frame& frame) noexcept
            {
                const bool is_invalid_shape = signature.args.size() != frame.arg_count ||
                    signature.returns.size() != frame.return_count ||
                    (frame.arg_count != 0U && frame.args == nullptr) ||
                    (frame.return_count != 0U && frame.returns == nullptr);
                if (is_invalid_shape)
                    return false;
                const auto matches = [](const auto& type, const auto& slot) noexcept {
                    return slot.data != nullptr && slot.type_id == type.type_id && slot.kind == type.abi_kind &&
                        slot.size == type.size && type.alignment != 0U &&
                        reinterpret_cast<std::uintptr_t>(slot.data) % type.alignment == 0U;
                };
                for (std::size_t index{}; index < frame.arg_count; ++index)
                    if (!matches(signature.args[index], frame.args[index]))
                        return false;
                for (std::size_t index{}; index < frame.return_count; ++index)
                    if (!matches(signature.returns[index], frame.returns[index]))
                        return false;
                return true;
            }

            static int invokeReflection(lux_script_call_frame* frame, std::span<void*> arguments) noexcept
            {
                if (frame == nullptr || frame->user_context == nullptr)
                    return -1;
                auto& self = *static_cast<PreparedCall*>(frame->user_context);
                if (!self.active || self.callable == nullptr || self.callable->coroutine_invoke != nullptr)
                    return -1;
                if (!matchesSlots(*self.callable->signature, *frame) || arguments.size() != frame->arg_count)
                    return -2;
                const auto& invokable = self.callable->method ? self.callable->method->invokable :
                    self.callable->function->invokable;
                for (std::size_t index{}; index < frame->arg_count; ++index)
                    arguments[index] = frame->args[index].data;
                invokable.invoker(self.callable->method ? self.instance->object : nullptr, arguments.data(),
                    frame->return_count == 0U ? nullptr : frame->returns[0].data);
                return 0;
            }

            template <std::size_t Count>
            static int invoke(lux_script_call_frame* frame) noexcept
            {
                if constexpr (Count == 0U)
                    return invokeReflection(frame, {});
                else
                {
                    std::array<void*, Count> arguments;
                    return invokeReflection(frame, arguments);
                }
            }

            template <std::size_t Count>
            static ScriptStepResult invokeStep(void* opaque, lux_script_call_frame& frame, ScriptStepContext& step,
                ScriptBackendContinuation& result) noexcept
            {
                if constexpr (Count == 0U)
                    return invokeStepImpl(opaque, frame, step, result, {});
                else
                {
                    std::array<lux_script_value_slot, Count> arguments;
                    return invokeStepImpl(opaque, frame, step, result, arguments);
                }
            }

            static ScriptStepResult invokeStepWithoutCopies(void* opaque, lux_script_call_frame& frame,
                ScriptStepContext& step, ScriptBackendContinuation& result) noexcept
            {
                return invokeStepImpl(opaque, frame, step, result, {});
            }

            static ScriptStepResult invokeStepImpl(
                void* opaque,
                lux_script_call_frame& frame,
                ScriptStepContext& step,
                ScriptBackendContinuation& result,
                std::span<lux_script_value_slot> arguments
            ) noexcept
            {
                auto& self = *static_cast<PreparedCall*>(opaque);
                const bool is_invalid_call = !self.active || self.instance == nullptr || self.callable == nullptr ||
                    self.callable->coroutine_invoke == nullptr;
                if (is_invalid_call)
                    return ScriptStepResult::failed(-1);
                auto* state = self.instance->owner;
                if (state == nullptr)
                    return ScriptStepResult::failed(-1);
                auto* continuation = state->acquireCoroutine(*self.instance);
                if (continuation == nullptr)
                    return ScriptStepResult::failed(-1);

                auto invocation_frame = frame;
                const bool is_invalid_shape = !matchesSlots(*self.callable->signature, frame) ||
                    (self.callable->reference_bytes != 0U && frame.arg_count != arguments.size());
                const bool copied = !is_invalid_shape &&
                    state->ownCoroutineReferences(*continuation, self, invocation_frame, arguments);
                if (!copied)
                {
                    state->releaseCoroutineSlot(*continuation);
                    return ScriptStepResult::failed(-1);
                }

                CppStaticCoroutineAccess::activate(continuation->context, step);
                auto coroutine = self.callable->coroutine_invoke(
                    self.instance->object,
                    continuation->context,
                    invocation_frame
                );
                CppStaticCoroutineAccess::deactivate(continuation->context);
                if (!coroutine)
                {
                    state->releaseCoroutineSlot(*continuation);
                    return ScriptStepResult::failed(-1);
                }
                continuation->handle = CppStaticCoroutineAccess::release(coroutine);
                const auto outcome = continuation->handle.promise().outcome;
                const bool is_suspended = !continuation->handle.done() &&
                    outcome.state == EScriptStepState::SUSPENDED && outcome.valid();
                if (is_suspended)
                {
                    result = {
                        continuation,
                        &State::resumeCoroutine,
                        &State::destroyCoroutineErased
                    };
                    return outcome;
                }
                const bool is_completed = continuation->handle.done() &&
                    outcome.state == EScriptStepState::COMPLETED && outcome.valid();
                state->destroyCoroutine(*continuation);
                return is_completed ? outcome : ScriptStepResult::failed(-1);
            }
        };

        explicit State(std::span<const CppStaticScriptPoolDescription> pools)
        {
            descriptor_indexes.reserve(pools.size());
            descriptor_by_key.reserve(pools.size());
            std::size_t prepared_capacity{};
            std::size_t coroutine_capacity{};
            for (const auto& pool : pools)
            {
                const auto* descriptor = pool.descriptor;
                if (!descriptor || !descriptor->state_ ||
                    descriptor->state_->descriptor_key.empty() ||
                    pool.instance_capacity == 0U)
                {
                    valid = false;
                    return;
                }
                const bool instance_capacity_overflow = instance_capacity >
                    (std::numeric_limits<std::size_t>::max)() - pool.instance_capacity;
                const bool prepared_capacity_overflow = pool.prepared_method_capacity == 0U ||
                    prepared_capacity > (std::numeric_limits<std::size_t>::max)() - pool.prepared_method_capacity;
                const bool coroutine_capacity_overflow = coroutine_capacity >
                    (std::numeric_limits<std::size_t>::max)() - pool.coroutine_capacity;
                if (instance_capacity_overflow || prepared_capacity_overflow || coroutine_capacity_overflow)
                {
                    valid = false;
                    return;
                }

                DescriptorIndex index;
                index.descriptor = descriptor->state_.get();
                index.instance_capacity = pool.instance_capacity;
                index.coroutine_capacity = pool.coroutine_capacity;
                const auto& cpp_body = std::get<lux::rdesc::CppStaticScript>(
                    descriptor->state_->description.body
                );
                if (!cpp_body.suspension_capable_exports.empty())
                {
                    if (pool.coroutine_capacity > (std::numeric_limits<std::size_t>::max)() / 2U)
                    {
                        valid = false;
                        return;
                    }
                    auto frames = detail::BoundedClassStorage::create(
                        pool.coroutine_frame_classes,
                        pool.coroutine_frame_storage_bytes,
                        pool.coroutine_capacity * 2U
                    );
                    if (!frames)
                    {
                        error = frames.error() == detail::EClassStorageError::ALLOCATION_FAILURE
                            ? ECppStaticScriptBridgeError::ALLOCATION_FAILURE
                            : ECppStaticScriptBridgeError::INVALID_DESCRIPTOR;
                        valid = false;
                        return;
                    }
                    index.coroutine_frames = std::move(*frames);
                }
                else if (pool.coroutine_capacity != 0U || pool.coroutine_frame_storage_bytes != 0U)
                {
                    valid = false;
                    return;
                }
                index.callables.reserve(index.descriptor->callables.size());
                for (const auto& callable : index.descriptor->callables)
                {
                    if (!index.callables.emplace(
                            callable.symbol,
                            std::addressof(callable)
                        ).second)
                    {
                        valid = false;
                        return;
                    }
                }
                if (index.descriptor->reflected_class)
                {
                    const auto& type = index.descriptor->reflected_class->type;
                    const bool valid_alignment = type.alignment != 0U &&
                        (type.alignment & (type.alignment - 1U)) == 0U;
                    const bool stride_overflow = valid_alignment &&
                        type.size > (std::numeric_limits<std::size_t>::max)() -
                            (type.alignment - 1U);
                    if (type.size == 0U || !valid_alignment || stride_overflow)
                    {
                        valid = false;
                        return;
                    }
                    index.object_stride = (type.size + type.alignment - 1U) &
                        ~(type.alignment - 1U);
                    if (pool.instance_capacity >
                        (std::numeric_limits<std::size_t>::max)() / index.object_stride)
                    {
                        valid = false;
                        return;
                    }
                    const auto slab_size = index.object_stride * pool.instance_capacity;
                    index.objects.data = ::operator new(
                        slab_size,
                        std::align_val_t{type.alignment},
                        std::nothrow
                    );
                    if (!index.objects.data)
                    {
                        error = ECppStaticScriptBridgeError::ALLOCATION_FAILURE;
                        valid = false;
                        return;
                    }
                    index.objects.alignment = type.alignment;
                    index.free_objects.reserve(pool.instance_capacity);
                    for (std::size_t slot = pool.instance_capacity; slot > 0U; --slot)
                        index.free_objects.push_back(slot - 1U);
                }
                descriptor_indexes.push_back(std::move(index));
                const auto descriptor_index = descriptor_indexes.size() - 1U;
                if (!descriptor_by_key.emplace(
                        descriptor->state_->descriptor_key,
                        descriptor_index
                    ).second)
                {
                    valid = false;
                    return;
                }
                instance_capacity += pool.instance_capacity;
                prepared_capacity += pool.prepared_method_capacity;
                coroutine_capacity += pool.coroutine_capacity;
            }
            instances.resize(instance_capacity);
            free_instances.reserve(instance_capacity);
            for (std::size_t index = instance_capacity; index > 0U; --index)
                free_instances.push_back(index - 1U);
            prepared_calls.resize(prepared_capacity);
            free_prepared_calls.reserve(prepared_capacity);
            for (std::size_t index = prepared_capacity; index > 0U; --index)
            {
                free_prepared_calls.push_back(index - 1U);
            }
            continuations.resize(coroutine_capacity);
            free_continuations.reserve(coroutine_capacity);
            for (std::size_t index = coroutine_capacity; index > 0U; --index)
                free_continuations.push_back(index - 1U);
        }

        ~State()
        {
            for (auto& continuation : continuations)
            {
                if (continuation.active && continuation.handle)
                    continuation.handle.destroy();
            }
            for (auto& instance : instances)
            {
                if (instance.object && instance.descriptor &&
                    instance.descriptor->descriptor->reflected_class)
                {
                    instance.descriptor->descriptor->reflected_class->destruct(instance.object);
                }
            }
        }

        [[nodiscard]] static bool findAbility(
            void* opaque,
            std::uint32_t instance_slot,
            std::uint64_t contract_hash,
            std::uint32_t& result
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            if (instance_slot >= self.instances.size())
                return false;
            const auto& instance = self.instances[instance_slot];
            if (instance.descriptor == nullptr)
                return false;
            for (std::size_t index{}; index < instance.capabilities.size(); ++index)
            {
                if (instance.capabilities[index].contract.hash() == contract_hash)
                {
                    result = static_cast<std::uint32_t>(index);
                    return true;
                }
            }
            return false;
        }

        [[nodiscard]] static bool resolveAbility(
            void* opaque,
            std::uint32_t instance_slot,
            std::uint32_t ability_slot,
            detail::ScriptCoroutineAbilityAccess& result
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            if (instance_slot >= self.instances.size())
                return false;
            const auto& instance = self.instances[instance_slot];
            if (instance.descriptor == nullptr || ability_slot >= instance.capabilities.size())
                return false;
            const auto& ability = instance.capabilities[ability_slot];
            if (ability.context == nullptr || ability.dispatch == nullptr)
                return false;
            result = {ability.context, ability.dispatch};
            return true;
        }

        [[nodiscard]] bool ownCoroutineReferences(
            CoroutineContinuation& continuation, PreparedCall& prepared, lux_script_call_frame& frame,
            std::span<lux_script_value_slot> arguments
        ) noexcept
        {
            const auto& plan = *prepared.callable;
            if (plan.reference_bytes == 0U)
                return true;
            auto storage = continuation.descriptor->coroutine_frames.acquire(
                prepared.argument_class, plan.reference_bytes);
            if (!storage)
                return false;
            continuation.arguments = *storage;
            std::copy_n(frame.args, frame.arg_count, arguments.data());
            for (const auto& reference : plan.references)
            {
                auto* destination = static_cast<std::byte*>(storage->data) + reference.offset;
                std::memcpy(destination, frame.args[reference.parameter].data, reference.size);
                arguments[reference.parameter].data = destination;
            }
            frame.args = arguments.data();
            return true;
        }

        void releaseCoroutineSlot(CoroutineContinuation& continuation) noexcept
        {
            auto* descriptor = continuation.descriptor;
            auto* instance = continuation.instance;
            const auto slot = continuation.slot;
            if (continuation.arguments && descriptor != nullptr)
                static_cast<void>(descriptor->coroutine_frames.release(continuation.arguments));
            continuation = {};
            if (descriptor != nullptr)
                --descriptor->active_coroutines;
            if (instance != nullptr)
                --instance->active_coroutines;
            free_continuations.push_back(slot);
        }

        void destroyCoroutine(CoroutineContinuation& continuation) noexcept
        {
            if (!continuation.active)
                return;
            if (continuation.handle)
                continuation.handle.destroy();
            releaseCoroutineSlot(continuation);
        }

        [[nodiscard]] CoroutineContinuation* acquireCoroutine(Instance& instance) noexcept
        {
            auto& descriptor = *instance.descriptor;
            if (free_continuations.empty() || descriptor.active_coroutines >= descriptor.coroutine_capacity)
                return nullptr;
            const auto slot = free_continuations.back();
            free_continuations.pop_back();
            auto& continuation = continuations[slot];
            continuation.owner = this;
            continuation.descriptor = std::addressof(descriptor);
            continuation.instance = std::addressof(instance);
            continuation.context = CppStaticCoroutineAccess::context(
                this,
                instance.slot,
                &State::findAbility,
                &State::resolveAbility,
                descriptor.coroutine_frames
            );
            continuation.slot = static_cast<std::uint32_t>(slot);
            continuation.active = true;
            ++descriptor.active_coroutines;
            descriptor.coroutine_high_water = (std::max)(
                descriptor.coroutine_high_water,
                descriptor.active_coroutines
            );
            ++instance.active_coroutines;
            return std::addressof(continuation);
        }

        [[nodiscard]] static ScriptStepResult resumeCoroutine(
            void* opaque,
            ScriptStepContext& step,
            const ScriptResumePacket& packet
        ) noexcept
        {
            auto& continuation = *static_cast<CoroutineContinuation*>(opaque);
            if (!continuation.active || !continuation.handle || continuation.instance == nullptr)
                return ScriptStepResult::failed(-1);
            auto& promise = continuation.handle.promise();
            if (!promise.prepareResume(packet))
                return promise.outcome;
            CppStaticCoroutineAccess::activate(continuation.context, step, std::addressof(packet));
            continuation.handle.resume();
            CppStaticCoroutineAccess::deactivate(continuation.context);
            promise.clearResume();
            const auto result = promise.outcome;
            const bool is_invalid_terminal = continuation.handle.done() &&
                result.state == EScriptStepState::SUSPENDED;
            const bool is_invalid_suspension = !continuation.handle.done() &&
                result.state != EScriptStepState::SUSPENDED;
            return is_invalid_terminal || is_invalid_suspension
                ? ScriptStepResult::failed(-1)
                : result;
        }

        static void destroyCoroutineErased(void* opaque) noexcept
        {
            auto* continuation = static_cast<CoroutineContinuation*>(opaque);
            if (continuation != nullptr && continuation->owner != nullptr)
                continuation->owner->destroyCoroutine(*continuation);
        }

        [[nodiscard]] DescriptorIndex* find(
            const lux::script::ScriptArtifact& artifact
        ) noexcept
        {
            const auto* body = std::get_if<lux::rdesc::CppStaticScript>(
                std::addressof(artifact.description().body)
            );
            if (!body)
                return nullptr;
            const auto found = descriptor_by_key.find(body->descriptor);
            return found == descriptor_by_key.end()
                ? nullptr
                : std::addressof(descriptor_indexes[found->second]);
        }

        [[nodiscard]] static bool executableContractMatches(
            const lux::script::ScriptArtifact& artifact,
            const CppStaticScriptDescriptor::State& descriptor
        ) noexcept
        {
            const auto* asset_body = std::get_if<lux::rdesc::CppStaticScript>(
                std::addressof(artifact.description().body)
            );
            const auto* descriptor_body =
                std::get_if<lux::rdesc::CppStaticScript>(
                    std::addressof(descriptor.description.body)
                );
            return asset_body && descriptor_body &&
                artifact.description().module_name ==
                    descriptor.description.module_name &&
                *asset_body == *descriptor_body &&
                artifact.description().exports == descriptor.description.exports &&
                artifact.description().lifecycle == descriptor.description.lifecycle &&
                artifact.description().api_requirements == descriptor.description.api_requirements &&
                artifact.description().event_requirements == descriptor.description.event_requirements;
        }

        static EScriptBackendResult createInstance(
            void* opaque,
            const ScriptInstanceCreateContext& context,
            const lux::script::ScriptArtifact& artifact,
            ScriptBackendInstance& result
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            auto* descriptor_index = self.find(artifact);
            if (!descriptor_index)
                return EScriptBackendResult::CONSTRUCTION_FAILURE;
            const auto& descriptor = *descriptor_index->descriptor;
            if (!executableContractMatches(artifact, descriptor))
            {
                return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
            }
            const bool entity_scope =
                std::holds_alternative<EntityScriptScope>(context.scope);
            if (descriptor.entity_scope != entity_scope)
                return EScriptBackendResult::EXECUTABLE_CONTRACT_MISMATCH;
            if (self.free_instances.empty() ||
                descriptor_index->active_instances >= descriptor_index->instance_capacity)
                return EScriptBackendResult::CAPACITY_EXCEEDED;
            if (descriptor.reflected_class && descriptor_index->free_objects.empty())
                return EScriptBackendResult::CAPACITY_EXCEEDED;
            const auto instance_slot = self.free_instances.back();
            self.free_instances.pop_back();
            auto* instance = std::addressof(self.instances[instance_slot]);
            instance->owner = std::addressof(self);
            instance->descriptor = descriptor_index;
            instance->object = nullptr;
            instance->slot = static_cast<std::uint32_t>(instance_slot);
            instance->capabilities = context.capabilities;
            if (descriptor.reflected_class)
            {
                instance->object_slot = descriptor_index->free_objects.back();
                descriptor_index->free_objects.pop_back();
                instance->object = static_cast<std::byte*>(descriptor_index->objects.data) +
                    instance->object_slot * descriptor_index->object_stride;
                try
                {
                    descriptor.reflected_class->construct(instance->object);
                    if (descriptor.attach && context.behavior)
                        descriptor.attach(instance->object, *context.behavior);
                }
                catch (...)
                {
                    descriptor_index->free_objects.push_back(instance->object_slot);
                    *instance = {};
                    self.free_instances.push_back(instance_slot);
                    return EScriptBackendResult::CONSTRUCTION_FAILURE;
                }
            }
            ++descriptor_index->active_instances;
            ++self.active_instances;
            result.value = instance;
            return EScriptBackendResult::SUCCESS;
        }

        static EScriptBackendResult prepareMethod(
            void* opaque,
            ScriptBackendInstance opaque_instance,
            const lux::rdesc::ScriptFunction& function,
            ScriptBackendPreparedMethod& result
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            auto* instance = static_cast<Instance*>(opaque_instance.value);
            if (!instance)
                return EScriptBackendResult::CONSTRUCTION_FAILURE;
            const auto found = instance->descriptor->callables.find(
                function.symbol_id
            );
            if (found == instance->descriptor->callables.end())
                return EScriptBackendResult::UNSUPPORTED_SIGNATURE;
            const auto& signature = *found->second->signature;
            if (function.args != signature.args || function.returns != signature.returns)
                return EScriptBackendResult::UNSUPPORTED_SIGNATURE;
            const auto argument_count = signature.args.size();
            if (argument_count > 64U)
                return EScriptBackendResult::UNSUPPORTED_SIGNATURE;
            const auto argument_class = found->second->reference_bytes != 0U ?
                instance->descriptor->coroutine_frames.select(found->second->reference_bytes,
                    found->second->reference_alignment) : detail::BoundedClassStorage::ClassHandle{};
            if (found->second->reference_bytes != 0U && !argument_class)
                return EScriptBackendResult::CAPACITY_EXCEEDED;
            if (self.free_prepared_calls.empty())
                return EScriptBackendResult::CAPACITY_EXCEEDED;
            const auto call_slot = self.free_prepared_calls.back();
            self.free_prepared_calls.pop_back();
            auto& call = self.prepared_calls[call_slot];
            call.instance = instance;
            call.callable = found->second;
            call.active = true;
            call.argument_class = argument_class;
            static constexpr auto sync_entries = []<std::size_t... Index>(std::index_sequence<Index...>) {
                return std::array{&PreparedCall::invoke<Index>...};
            }(std::make_index_sequence<65U>{});
            static constexpr auto step_entries = []<std::size_t... Index>(std::index_sequence<Index...>) {
                return std::array{&PreparedCall::invokeStep<Index>...};
            }(std::make_index_sequence<65U>{});
            const auto sync = call.callable->typed_invoke != nullptr ?
                lux::script::BoundScriptCall{call.callable->typed_invoke, instance->object} :
                lux::script::BoundScriptCall{sync_entries[argument_count], &call};
            result = {
                std::addressof(call),
                sync,
                call.callable->coroutine_invoke == nullptr
                    ? BoundScriptStepCall{}
                    : BoundScriptStepCall{std::addressof(call), call.callable->reference_bytes == 0U ?
                        &PreparedCall::invokeStepWithoutCopies : step_entries[argument_count]}
            };
            return EScriptBackendResult::SUCCESS;
        }

        static void releaseMethod(
            void* opaque,
            ScriptBackendInstance,
            ScriptBackendPreparedMethod method
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            auto* prepared = static_cast<PreparedCall*>(method.token);
            if (!prepared || !prepared->active)
                return;
            prepared->instance = nullptr;
            prepared->callable = nullptr;
            prepared->active = false;
            const auto index = static_cast<std::size_t>(
                prepared - self.prepared_calls.data()
            );
            self.free_prepared_calls.push_back(index);
        }

        static void destroyInstance(
            void* opaque,
            ScriptBackendInstance opaque_instance
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            auto* instance = static_cast<Instance*>(opaque_instance.value);
            if (!instance)
                return;
            if (instance->active_coroutines != 0U)
                std::terminate();
            if (instance->object)
            {
                instance->descriptor->descriptor->reflected_class->destruct(
                    instance->object
                );
                instance->descriptor->free_objects.push_back(instance->object_slot);
            }
            --instance->descriptor->active_instances;
            const auto index = static_cast<std::size_t>(
                instance - self.instances.data()
            );
            *instance = {};
            self.free_instances.push_back(index);
            --self.active_instances;
        }

        std::vector<DescriptorIndex> descriptor_indexes;
        std::unordered_map<std::string_view, std::size_t> descriptor_by_key;
        std::vector<Instance> instances;
        std::vector<std::size_t> free_instances;
        std::vector<PreparedCall> prepared_calls;
        std::vector<std::size_t> free_prepared_calls;
        std::vector<CoroutineContinuation> continuations;
        std::vector<std::size_t> free_continuations;
        std::size_t instance_capacity{};
        std::size_t active_instances{};
        ECppStaticScriptBridgeError error{ECppStaticScriptBridgeError::INVALID_DESCRIPTOR};
        bool valid{true};
    };

    lux::cxx::expected<CppStaticScriptBackend, ECppStaticScriptBridgeError>
    CppStaticScriptBackend::create(std::span<const CppStaticScriptPoolDescription> pools) noexcept
    {
        if (pools.empty())
            return lux::cxx::unexpected(ECppStaticScriptBridgeError::INVALID_DESCRIPTOR);
        try
        {
            auto state = std::make_unique<State>(pools);
            if (!state->valid)
                return lux::cxx::unexpected(state->error);
            return CppStaticScriptBackend{std::move(state)};
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(ECppStaticScriptBridgeError::ALLOCATION_FAILURE);
        }
    }

    CppStaticScriptBackend::CppStaticScriptBackend(std::unique_ptr<State> state) noexcept : state_(std::move(state))
    {
    }

    CppStaticScriptBackend::~CppStaticScriptBackend() = default;
    CppStaticScriptBackend::CppStaticScriptBackend(
        CppStaticScriptBackend&&
    ) noexcept = default;
    CppStaticScriptBackend&
    CppStaticScriptBackend::operator=(
        CppStaticScriptBackend&&
    ) noexcept = default;

    CppStaticScriptBackend::operator bool() const noexcept
    {
        return state_ != nullptr;
    }

    CppStaticScriptBackendStats CppStaticScriptBackend::stats() const noexcept
    {
        CppStaticScriptBackendStats result;
        if (!state_)
            return result;
        result.prepared_method_storage_bytes = state_->prepared_calls.capacity() * sizeof(State::PreparedCall) +
            state_->free_prepared_calls.capacity() * sizeof(std::size_t);
        result.active_prepared_methods = state_->prepared_calls.size() - state_->free_prepared_calls.size();
        for (const auto& descriptor : state_->descriptor_indexes)
        {
            const auto stats = descriptor.coroutine_frames.stats();
            result.frame_storage_bytes += stats.arena_bytes;
            result.active_frames += descriptor.active_coroutines;
            result.frame_high_water += descriptor.coroutine_high_water;
            result.frame_capacity_failures += stats.capacity_failures;
        }
        return result;
    }

    ScriptBackendDescriptor CppStaticScriptBackend::descriptor() noexcept
    {
        return state_
            ? ScriptBackendDescriptor{
                lux::rdesc::Script::Kind::CPP_STATIC,
                state_.get(),
                &State::createInstance,
                &State::prepareMethod,
                &State::releaseMethod,
                &State::destroyInstance
            }
            : ScriptBackendDescriptor{};
    }
}
