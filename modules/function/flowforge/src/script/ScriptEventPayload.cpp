#include <lux/engine/flowforge/FlowExecutionCompiler.hpp>
#include <lux/engine/flowforge/detail/FlowPinSchema.hpp>
#include <lux/engine/flowforge/detail/FlowSourceMetadata.hpp>
#include <lux/engine/flowforge/script/ScriptEventPayload.hpp>
#include <lux/engine/flowforge/script/ScriptValueType.hpp>

namespace lux::flowforge
{
    namespace
    {
        FlowForgeResult<std::unique_ptr<ScriptEventPayload>> cloneEvent(const ScriptEventPayload& value) noexcept
        {
            return std::make_unique<ScriptEventPayload>(value);
        }
    } // namespace

    FlowNodeRegistration scriptEventRegistration(object::CodeLease code) noexcept
    {
        FlowNodeRegistration result;
        constexpr std::string_view name = "lux.flow.event_wait";
        result.identity = {graph::nodeTypeId(name), std::string(name), 1};
        result.payload_type = cxx::typeToken<ScriptEventPayload>();
        result.code = std::move(code);
        result.create = [](const object::CodeLease& lease) noexcept
        {
            return FlowNodePayload::make<ScriptEventPayload, cloneEvent>(lease, script::ScriptEventSourceDescription{});
        };
        result.describe_pins = [](const FlowNodePayload& payload) noexcept
        { return payload.get<ScriptEventPayload>()->describePins(); };
        result.validate = [](const FlowNodePayload& payload) noexcept -> FlowForgeResult<void>
        {
            auto schema = payload.get<ScriptEventPayload>()->describePins();
            if (!schema)
            {
                return cxx::unexpected(std::move(schema.error()));
            }
            return {};
        };
        result.compile_execution = [](const FlowNodePayload& payload,
                                      FlowExecutionCompiler& compiler) noexcept -> FlowForgeResult<void>
        {
            return compiler.eventWait(
                *payload.get<ScriptEventPayload>(),
                detail::pinSemantic(EFlowPinRole::DATA, graph::EPinDirection::OUTPUT, 1),
                detail::pinSemantic(EFlowPinRole::EXECUTION, graph::EPinDirection::OUTPUT, 0)
            );
        };
        result.capture_source = [](const FlowNodePayload& payload) noexcept -> FlowSourceResult<VFlowSourceParameters>
        { return payload.get<ScriptEventPayload>()->source(); };
        result.restore_source = [](const FlowSourceNode& source,
                                   const FlowSourceEnvironment& environment,
                                   FlowReferenceView,
                                   const object::CodeLease& lease) noexcept -> FlowSourceResult<FlowNodePayload>
        {
            const auto* saved = std::get_if<lux::script::ScriptEventSourceDescription>(&source.parameters);
            if (!saved)
            {
                return detail::sourceFailure(EFlowSourceError::SCHEMA_MISMATCH, source.type, source.id);
            }
            const auto& saved_event = *saved;
            for (const auto& event : environment.events)
            {
                const bool is_matching_event =
                    event.system_id == saved_event.system_id && event.event_id == saved_event.event_id;
                if (is_matching_event)
                {
                    if (event != saved_event)
                    {
                        return detail::sourceFailure(EFlowSourceError::SCHEMA_MISMATCH, "event", source.id);
                    }
                    return detail::sourcePayload<ScriptEventPayload, cloneEvent>(lease, event);
                }
            }
            return detail::sourceFailure(EFlowSourceError::SCHEMA_MISMATCH, "missing event", source.id);
        };
        return result;
    }

    ScriptEventPayload::ScriptEventPayload(const script::ScriptEventSourceDescription& source) noexcept
        : source_(source), type_(std::make_shared<const detail::ScriptValueType>(script::ScriptAbilityValueDescription{
                               source_.payload.type_id,
                               source_.payload.canonical_name,
                               semantic::EValuePass::VALUE,
                               source_.payload.abi_kind,
                               source_.payload.size,
                               source_.payload.alignment,
                               script::EScriptAbilityValueLifetime::OWNED_VALUE
                           }))
    {
    }

    ScriptEventPayload::~ScriptEventPayload() = default;

    ScriptEventPayload::ScriptEventPayload(ScriptEventPayload&&) noexcept = default;

    ScriptEventPayload& ScriptEventPayload::operator=(ScriptEventPayload&&) noexcept = default;

    std::size_t ScriptEventPayload::descriptionBytes() const noexcept
    {
        return sizeof(detail::ScriptValueType) + type_->name.capacity() + source_.system_name.capacity() +
               source_.event_name.capacity() + source_.payload.canonical_name.capacity() + 4;
    }

    FlowNodeRegistration::PinResult ScriptEventPayload::describePins() const noexcept
    {
        std::vector<FlowPinDeclaration> pins;
        detail::appendExecutionPin(pins, graph::EPinDirection::INPUT, "Execute");
        detail::appendExecutionPin(pins, graph::EPinDirection::OUTPUT, "Received");
        detail::appendDataPin(pins, graph::EPinDirection::OUTPUT, 1, "Payload", &type_->type);
        return pins;
    }
} // namespace lux::flowforge
