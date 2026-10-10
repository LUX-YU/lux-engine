#include <lux/engine/flowforge/FlowExecutionCompiler.hpp>
#include <lux/engine/flowforge/detail/FlowPinSchema.hpp>
#include <lux/engine/flowforge/detail/FlowSourceMetadata.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNodeStorage.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityPayload.hpp>
#include <lux/engine/flowforge/script/ScriptValueType.hpp>

namespace lux::flowforge
{
    namespace
    {
        FlowForgeResult<std::unique_ptr<ScriptAbilityPayload>> cloneAbility(const ScriptAbilityPayload& value) noexcept
        {
            return std::make_unique<ScriptAbilityPayload>(value);
        }
    } // namespace

    FlowNodeRegistration scriptAbilityRegistration(object::CodeLease code) noexcept
    {
        FlowNodeRegistration result;
        constexpr std::string_view name = "lux.flow.ability_call";
        result.identity = {graph::nodeTypeId(name), std::string(name), 1};
        result.payload_type = cxx::typeToken<ScriptAbilityPayload>();
        result.code = std::move(code);
        result.create = [](const object::CodeLease& lease) noexcept
        { return FlowNodePayload::make<ScriptAbilityPayload, cloneAbility>(lease, ScriptAbilityNodeDescription{}); };
        result.describe_pins = [](const FlowNodePayload& payload) noexcept
        { return payload.get<ScriptAbilityPayload>()->describePins(); };
        result.validate = [](const FlowNodePayload& payload) noexcept -> FlowForgeResult<void>
        {
            // Catalog/schema and suspension checks remain in FlowAnalysis with the real environment.
            auto schema = payload.get<ScriptAbilityPayload>()->describePins();
            if (!schema)
            {
                return cxx::unexpected(std::move(schema.error()));
            }
            return {};
        };
        result.compile_execution = [](const FlowNodePayload& payload,
                                      FlowExecutionCompiler& compiler) noexcept -> FlowForgeResult<void>
        {
            const auto& ability = *payload.get<ScriptAbilityPayload>();
            const auto arguments = detail::parameterSemantics(graph::EPinDirection::INPUT, ability.parameters().size());
            const auto results = detail::parameterSemantics(graph::EPinDirection::OUTPUT, ability.results().size());
            return compiler.abilityCall(
                ability,
                arguments,
                results,
                detail::pinSemantic(EFlowPinRole::EXECUTION, graph::EPinDirection::OUTPUT, 0)
            );
        };
        result.capture_source = [](const FlowNodePayload& payload) noexcept -> FlowSourceResult<VFlowSourceParameters>
        {
            const auto& ability = *payload.get<ScriptAbilityPayload>();
            return FlowSourceAbility{
                std::string(ability.contract().name()),
                std::string(ability.method().name()),
                ability.expectedSchemaVersion(),
                ability.expectedSchemaHash()
            };
        };
        result.restore_source = [](const FlowSourceNode& source,
                                   const FlowSourceEnvironment& environment,
                                   FlowReferenceView,
                                   const object::CodeLease& lease) noexcept -> FlowSourceResult<FlowNodePayload>
        {
            const auto* saved = std::get_if<FlowSourceAbility>(&source.parameters);
            if (!saved)
            {
                return detail::sourceFailure(EFlowSourceError::SCHEMA_MISMATCH, source.type, source.id);
            }
            const auto& ability = *saved;
            const auto* description = environment.abilities.find(
                lux::script::ScriptApiContractIdView{ability.contract},
                lux::script::ScriptApiMethodIdView{ability.method}
            );
            if (!description)
            {
                return detail::sourceFailure(EFlowSourceError::UNKNOWN_ABILITY, ability.method, source.id);
            }
            const bool is_schema_mismatch = description->schema_version != ability.schema_version ||
                                            description->schema_hash != ability.schema_hash;
            if (is_schema_mismatch)
            {
                return detail::sourceFailure(EFlowSourceError::SCHEMA_MISMATCH, ability.method, source.id);
            }
            return detail::sourcePayload<ScriptAbilityPayload, cloneAbility>(lease, *description);
        };
        return result;
    }

    ScriptAbilityPayload::ScriptAbilityPayload(const ScriptAbilityNodeDescription& description) noexcept
        : description_(std::make_shared<const detail::ScriptAbilityNodeStorage>(description))
    {
        const auto& owned = description_->description();
        types_.reserve(owned.parameters.size() + owned.results.size());
        for (const auto& parameter : owned.parameters)
        {
            types_.push_back(std::make_shared<const detail::ScriptValueType>(parameter.value));
        }
        for (const auto& result : owned.results)
        {
            types_.push_back(std::make_shared<const detail::ScriptValueType>(result));
        }
    }

    ScriptAbilityPayload::~ScriptAbilityPayload() = default;

    ScriptAbilityPayload::ScriptAbilityPayload(ScriptAbilityPayload&&) noexcept = default;

    ScriptAbilityPayload& ScriptAbilityPayload::operator=(ScriptAbilityPayload&&) noexcept = default;

    const ScriptAbilityNodeDescription& ScriptAbilityPayload::description() const noexcept
    {
        return description_->description();
    }

    std::size_t ScriptAbilityPayload::descriptionBytes() const noexcept
    {
        std::size_t bytes = description_->bytes() + types_.capacity() * sizeof(decltype(types_)::value_type);
        for (const auto& type : types_)
        {
            bytes += sizeof(detail::ScriptValueType) + type->name.capacity() + 1;
        }
        return bytes;
    }

    FlowNodeRegistration::PinResult ScriptAbilityPayload::describePins() const noexcept
    {
        std::vector<FlowPinDeclaration> pins;
        detail::appendExecutionPin(pins, graph::EPinDirection::INPUT, "Execute");
        const auto& owned = description();
        for (std::size_t i = 0; i < owned.parameters.size(); ++i)
        {
            detail::appendDataPin(
                pins,
                graph::EPinDirection::INPUT,
                i + 1,
                std::string(owned.parameters[i].name),
                &types_[i]->type,
                true
            );
        }
        detail::appendExecutionPin(pins, graph::EPinDirection::OUTPUT, "Completed");
        for (std::size_t i = 0; i < owned.results.size(); ++i)
        {
            auto name = owned.results.size() == 1 ? "Result" : "Result " + std::to_string(i);
            detail::appendDataPin(
                pins,
                graph::EPinDirection::OUTPUT,
                i + 1,
                std::move(name),
                &types_[owned.parameters.size() + i]->type
            );
        }
        return pins;
    }

    lux::script::ScriptApiContractIdView ScriptAbilityPayload::contract() const noexcept
    {
        return description_->description().contract;
    }

    lux::script::ScriptApiMethodIdView ScriptAbilityPayload::method() const noexcept
    {
        return description_->description().method;
    }

    std::uint32_t ScriptAbilityPayload::expectedSchemaVersion() const noexcept
    {
        return description_->description().schema_version;
    }

    std::uint64_t ScriptAbilityPayload::expectedSchemaHash() const noexcept
    {
        return description_->description().schema_hash;
    }

    lux::script::EScriptApiMethodKind ScriptAbilityPayload::methodKind() const noexcept
    {
        return description_->description().kind;
    }

    lux::script::EScriptAbilityReceiverKind ScriptAbilityPayload::receiverKind() const noexcept
    {
        return description_->description().receiver;
    }

    std::span<const lux::script::ScriptAbilityParameterDescription> ScriptAbilityPayload::parameters() const noexcept
    {
        return description_->description().parameters;
    }

    std::span<const lux::script::ScriptAbilityValueDescription> ScriptAbilityPayload::results() const noexcept
    {
        return description_->description().results;
    }

} // namespace lux::flowforge
