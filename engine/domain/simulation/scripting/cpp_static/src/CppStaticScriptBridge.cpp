#include <lux/engine/simulation/scripting/cpp_static/CppStaticScriptBridge.hpp>

#include <algorithm>
#include <limits>
#include <new>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace lux::simulation::script
{
    namespace
    {
        struct Callable final
        {
            lux::script::ScriptSymbolId symbol{};
            const lux::meta::RefMethod* method{};
            const lux::meta::RefFunction* function{};
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
            void (*attach)(void*, ScriptBehavior&) noexcept
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
                std::string{descriptor_key}};
            state->descriptor_key = descriptor_key;
            state->reflected_class = std::addressof(reflected_class);
            state->attach = attach;
            state->entity_scope = true;
            state->description.exports.reserve(methods.size());
            state->callables.reserve(methods.size());
            std::unordered_set<lux::script::ScriptSymbolId> symbols_seen;
            symbols_seen.reserve(methods.size());
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
                state->callables.push_back(Callable{symbol, method, nullptr});
            }
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
            CppStaticRecordSemanticResolver record_types
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
                std::string{descriptor_key}};
            state->descriptor_key = descriptor_key;
            state->description.exports.reserve(functions.size());
            state->callables.reserve(functions.size());
            std::unordered_set<lux::script::ScriptSymbolId> symbols_seen;
            symbols_seen.reserve(functions.size());
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
                state->callables.push_back(Callable{symbol, nullptr, function});
            }
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
            std::size_t object_stride{};
            std::size_t instance_capacity{};
            std::size_t active_instances{};
        };

        struct Instance final
        {
            DescriptorIndex* descriptor{};
            void* object{};
            std::size_t object_slot{(std::numeric_limits<std::size_t>::max)()};
        };

        struct PreparedCall final
        {
            Instance* instance{};
            const Callable* callable{};
            std::vector<void*> arguments;
            bool active{};

            static int invoke(lux_script_call_frame* frame) noexcept
            {
                if (!frame || !frame->user_context)
                    return -1;
                auto& self = *static_cast<PreparedCall*>(frame->user_context);
                const auto& invokable = self.callable->method
                    ? self.callable->method->invokable
                    : self.callable->function->invokable;
                if (frame->arg_count != invokable.parameters.size() ||
                    frame->arg_count > self.arguments.size() ||
                    (frame->arg_count != 0U && !frame->args) ||
                    (frame->return_count != 0U && !frame->returns))
                {
                    return -2;
                }
                for (std::size_t index{}; index < frame->arg_count; ++index)
                    self.arguments[index] = frame->args[index].data;
                void* result = frame->return_count == 0U
                    ? nullptr
                    : frame->returns[0].data;
                invokable.invoker(
                    self.callable->method ? self.instance->object : nullptr,
                    self.arguments.data(),
                    result
                );
                return 0;
            }
        };

        explicit State(std::span<const CppStaticScriptPoolDescription> pools)
        {
            descriptor_indexes.reserve(pools.size());
            descriptor_by_key.reserve(pools.size());
            std::size_t maximum_parameters{};
            std::size_t prepared_capacity{};
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
                const bool prepared_capacity_overflow =
                    !descriptor->state_->callables.empty() &&
                    pool.instance_capacity >
                        ((std::numeric_limits<std::size_t>::max)() - prepared_capacity) /
                            descriptor->state_->callables.size();
                if (instance_capacity_overflow || prepared_capacity_overflow)
                {
                    valid = false;
                    return;
                }

                DescriptorIndex index;
                index.descriptor = descriptor->state_.get();
                index.instance_capacity = pool.instance_capacity;
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
                    const auto& invokable = callable.method
                        ? callable.method->invokable
                        : callable.function->invokable;
                    maximum_parameters = (std::max)(
                        maximum_parameters,
                        invokable.parameters.size()
                    );
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
                prepared_capacity += descriptor->state_->callables.size() * pool.instance_capacity;
            }
            instances.resize(instance_capacity);
            free_instances.reserve(instance_capacity);
            for (std::size_t index = instance_capacity; index > 0U; --index)
                free_instances.push_back(index - 1U);
            prepared_calls.resize(prepared_capacity);
            free_prepared_calls.reserve(prepared_capacity);
            for (std::size_t index = prepared_capacity; index > 0U; --index)
            {
                prepared_calls[index - 1U].arguments.resize(
                    maximum_parameters
                );
                free_prepared_calls.push_back(index - 1U);
            }
        }

        ~State()
        {
            for (auto& instance : instances)
            {
                if (instance.object && instance.descriptor &&
                    instance.descriptor->descriptor->reflected_class)
                {
                    instance.descriptor->descriptor->reflected_class->destruct(instance.object);
                }
            }
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
                asset_body->descriptor == descriptor_body->descriptor &&
                artifact.description().exports == descriptor.description.exports;
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
            instance->descriptor = descriptor_index;
            instance->object = nullptr;
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
            lux::script::BoundScriptCall& result
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
            if (self.free_prepared_calls.empty())
                return EScriptBackendResult::CAPACITY_EXCEEDED;
            const auto call_slot = self.free_prepared_calls.back();
            self.free_prepared_calls.pop_back();
            auto& call = self.prepared_calls[call_slot];
            call.instance = instance;
            call.callable = found->second;
            call.active = true;
            result = lux::script::BoundScriptCall{
                &PreparedCall::invoke,
                std::addressof(call)};
            return EScriptBackendResult::SUCCESS;
        }

        static void releaseMethod(
            void* opaque,
            ScriptBackendInstance,
            lux::script::BoundScriptCall call
        ) noexcept
        {
            auto& self = *static_cast<State*>(opaque);
            auto* prepared = static_cast<PreparedCall*>(call.context);
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

    ScriptBackendDescriptor CppStaticScriptBackend::descriptor() noexcept
    {
        return state_
            ? ScriptBackendDescriptor{
                lux::rdesc::Script::Kind::CPP_STATIC,
                state_.get(),
                &State::createInstance,
                &State::prepareMethod,
                &State::releaseMethod,
                &State::destroyInstance}
            : ScriptBackendDescriptor{};
    }
}
