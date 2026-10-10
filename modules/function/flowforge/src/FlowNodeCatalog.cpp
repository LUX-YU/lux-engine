#include <lux/engine/flowforge/ControlNodes.hpp>
#include <lux/engine/flowforge/FlowNodeCatalog.hpp>
#include <lux/engine/flowforge/FunctionNodes.hpp>
#include <lux/engine/flowforge/ObjectNodes.hpp>
#include <lux/engine/flowforge/ScalarNodes.hpp>
#include <lux/engine/flowforge/detail/FlowNodeIdentity.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityPayload.hpp>
#include <lux/engine/flowforge/script/ScriptEventPayload.hpp>

#include <lux/engine/flowforge/detail/FlowSourceMetadata.hpp>
#include <lux/engine/meta/Meta.hpp>

#include <algorithm>

namespace lux::flowforge
{
    namespace
    {
        FlowForgeFailure invalid(std::string message, std::uint32_t pin = ~std::uint32_t{0}) noexcept
        {
            return {EFlowForgeError::GRAPH_INVALID, std::move(message), {}, pin};
        }

        bool validRegistration(const FlowNodeRegistration& value) noexcept
        {
            const bool has_identity = value.identity.id.valid() && value.identity.version != 0 &&
                                      detail::canonicalNodeName(value.identity.canonical_name);
            const bool has_payload_type = value.payload_type.isValid();
            const bool has_code = value.code.valid();
            const bool has_one_compiler = (value.compile != nullptr) != (value.compile_execution != nullptr);
            const bool has_callbacks = value.create && value.describe_pins && value.validate && has_one_compiler;
            const bool has_codec_pair = (value.capture_source != nullptr) == (value.restore_source != nullptr);
            const bool has_evaluation =
                value.value_evaluation == EFlowValueEvaluation::PURE ||
                (value.value_evaluation == EFlowValueEvaluation::READS_STATE && value.compile != nullptr);
            const bool has_source_stage =
                value.source_stage == EFlowSourceStage::DECLARATION || value.source_stage == EFlowSourceStage::BODY;
            return has_identity && has_payload_type && has_code && has_callbacks && has_codec_pair && has_evaluation &&
                   has_source_stage;
        }

        const FlowNodeRegistration* builtinRegistration(std::string_view name) noexcept
        {
            static const auto scalars = scalarNodeRegistrations();
            static const auto controls = controlNodeRegistrations();
            static const auto functions = functionNodeRegistrations();
            static const auto objects = objectNodeRegistrations();
            static const std::vector calls{
                nativeCallRegistration(),
                scriptAbilityRegistration(),
                scriptEventRegistration()
            };
            for (const auto group :
                 {std::span{scalars}, std::span{controls}, std::span{functions}, std::span{objects}, std::span{calls}})
            {
                for (const auto& value : group)
                {
                    if (name == value.identity.canonical_name)
                    {
                        return &value;
                    }
                }
            }
            return nullptr;
        }

        bool matchesBuiltin(const FlowNodeRegistration& candidate, const FlowNodeRegistration& value) noexcept
        {
            // Published intrinsic names cannot be reassigned. A DLL provider may supply its own code pin.
            const bool has_identity =
                candidate.identity.version == value.identity.version && candidate.payload_type == value.payload_type;
            const bool has_factory = candidate.create == value.create && candidate.describe_pins == value.describe_pins;
            const bool has_behavior = candidate.validate == value.validate && candidate.compile == value.compile &&
                                      candidate.compile_execution == value.compile_execution &&
                                      candidate.validate_references == value.validate_references &&
                                      candidate.value_evaluation == value.value_evaluation;
            const bool has_codec = candidate.capture_source == value.capture_source &&
                                   candidate.restore_source == value.restore_source &&
                                   candidate.source_stage == value.source_stage;
            return has_identity && has_factory && has_behavior && has_codec;
        }

    } // namespace

    FlowNodeType::FlowNodeType(FlowNodeRegistration registration) noexcept
        : payload_type_name_(registration.payload_type.name()), registration_(std::move(registration))
    {
        registration_.payload_type = cxx::TypeToken{registration_.payload_type.hash(), payload_type_name_};
    }

    FlowNodeType::~FlowNodeType() = default;

    const graph::GraphNodeTypeIdentity& FlowNodeType::identity() const noexcept
    {
        return registration_.identity;
    }

    bool FlowNodeType::accepts(const FlowNodePayload& payload) const noexcept
    {
        return payload.value_ && payload.type_ == registration_.payload_type &&
               payload.code_.sameOwner(registration_.code);
    }

    FlowForgeResult<FlowNodePayload> FlowNodeType::create() const noexcept
    {
        auto result = registration_.create(registration_.code);
        if (!result)
        {
            return cxx::unexpected(std::move(result.error()));
        }
        if (!accepts(*result))
        {
            return cxx::unexpected(invalid("node factory returned a mismatched payload or code lease"));
        }
        return result;
    }

    FlowForgeResult<void> FlowNodeType::validate(const FlowNodePayload& payload) const noexcept
    {
        if (!accepts(payload))
        {
            return cxx::unexpected(invalid("node payload does not belong to this definition"));
        }
        return registration_.validate(payload);
    }

    FlowForgeResult<void> FlowNodeType::validateReferences(const FlowNodePayload& payload, FlowReferenceView references)
        const noexcept
    {
        if (!accepts(payload))
        {
            return cxx::unexpected(invalid("node payload does not belong to this definition"));
        }
        if (registration_.validate_references)
        {
            return registration_.validate_references(payload, references);
        }
        return {};
    }

    EFlowSourceStage FlowNodeType::sourceStage() const noexcept
    {
        return registration_.source_stage;
    }

    FlowSourceResult<VFlowSourceParameters> FlowNodeType::captureSource(const FlowNodePayload& payload) const noexcept
    {
        auto schema = describePins(payload);
        if (!schema)
        {
            return detail::sourceCodecFailure(std::move(schema.error()), registration_.identity.canonical_name);
        }
        if (!registration_.capture_source)
        {
            return detail::sourceCodecFailure(
                invalid("node definition has no source codec"),
                registration_.identity.canonical_name
            );
        }
        return registration_.capture_source(payload);
    }

    FlowSourceResult<FlowNodePayload> FlowNodeType::restoreSource(
        const FlowSourceNode& source,
        const FlowSourceEnvironment& environment,
        FlowReferenceView references
    ) const noexcept
    {
        const bool is_identity_mismatch =
            source.type != registration_.identity.canonical_name || source.version != registration_.identity.version;
        if (is_identity_mismatch)
        {
            return detail::sourceFailure(EFlowSourceError::SCHEMA_MISMATCH, source.type, source.id);
        }
        if (!registration_.restore_source)
        {
            return detail::sourceCodecFailure(invalid("node definition has no source codec"), source.type, source.id);
        }
        auto payload = registration_.restore_source(source, environment, references, registration_.code);
        if (!payload)
        {
            auto error = std::move(payload.error());
            if (error.code == EFlowSourceError::NODE_CODEC_FAILURE)
            {
                error.field = source.type;
                error.node = source.id;
            }
            return cxx::unexpected(std::move(error));
        }
        auto schema = describePins(*payload);
        if (!schema)
        {
            return detail::sourceCodecFailure(std::move(schema.error()), source.type, source.id);
        }
        return payload;
    }

    FlowNodeRegistration::PinResult FlowNodeType::describePins(const FlowNodePayload& payload) const noexcept
    {
        if (!accepts(payload))
        {
            return cxx::unexpected(invalid("node payload does not belong to this definition"));
        }
        auto pins = registration_.describe_pins(payload);
        if (!pins)
        {
            return cxx::unexpected(std::move(pins.error()));
        }
        for (std::size_t i = 0; i != pins->size(); ++i)
        {
            const auto& pin = (*pins)[i];
            const bool has_identity = pin.semantic.valid() && !pin.name.empty();
            const bool is_data = pin.role == EFlowPinRole::DATA;
            const bool is_execution = pin.role == EFlowPinRole::EXECUTION;
            const bool is_valid_type = is_data ? pin.type != nullptr : is_execution && pin.type == nullptr;
            const bool has_invalid_default = is_execution && (pin.allow_default || pin.necessary);
            const bool can_initialize = is_data && pin.direction == graph::EPinDirection::INPUT;
            const bool has_invalid_initializer = pin.initial_value != nullptr && !can_initialize;
            const bool is_valid_direction =
                pin.direction == graph::EPinDirection::INPUT || pin.direction == graph::EPinDirection::OUTPUT;
            const bool has_duplicate = std::any_of(
                pins->begin(),
                pins->begin() + i,
                [&](const auto& existing) noexcept { return existing.semantic == pin.semantic; }
            );
            const bool is_invalid_pin = !has_identity || !is_valid_type || !is_valid_direction || has_duplicate ||
                                        has_invalid_default || has_invalid_initializer;
            if (is_invalid_pin)
            {
                return cxx::unexpected(
                    invalid("invalid or duplicate node pin declaration", static_cast<std::uint32_t>(i))
                );
            }
        }
        return pins;
    }

    FlowNodeRegistration::ValueResult FlowNodeType::compile(
        const FlowNodePayload& payload,
        std::span<const FlowValue> inputs,
        FlowValueCompiler& compiler
    ) const noexcept
    {
        if (!registration_.compile)
        {
            return cxx::unexpected(invalid("node does not have a value compiler"));
        }
        auto accepted = validate(payload);
        if (!accepted)
        {
            return cxx::unexpected(std::move(accepted.error()));
        }
        auto pins = describePins(payload);
        if (!pins)
        {
            return cxx::unexpected(std::move(pins.error()));
        }
        std::size_t input_count{};
        std::size_t output_count{};
        for (const auto& pin : *pins)
        {
            if (pin.role == EFlowPinRole::EXECUTION)
            {
                continue;
            }
            if (pin.direction == graph::EPinDirection::OUTPUT)
            {
                ++output_count;
                continue;
            }
            const auto* actual = input_count < inputs.size() ? compiler.type(inputs[input_count]) : nullptr;
            const bool is_matching = actual && *actual == *pin.type;
            if (!is_matching)
            {
                return cxx::unexpected(invalid("node input count or type differs from its declaration"));
            }
            ++input_count;
        }
        if (input_count != inputs.size())
        {
            return cxx::unexpected(invalid("node input count differs from its declaration"));
        }
        auto result = registration_.compile(payload, inputs, compiler);
        if (!result)
        {
            return result;
        }
        if (result->size() != output_count)
        {
            return cxx::unexpected(invalid("node compiler returned the wrong output count"));
        }
        std::size_t output_index{};
        for (const auto& pin : *pins)
        {
            const bool is_data_output = pin.role == EFlowPinRole::DATA && pin.direction == graph::EPinDirection::OUTPUT;
            if (!is_data_output)
            {
                continue;
            }
            const auto* actual = compiler.type((*result)[output_index++]);
            const bool is_matching = actual && *actual == *pin.type;
            if (!is_matching)
            {
                return cxx::unexpected(invalid("node compiler returned an invalid output value"));
            }
        }
        return result;
    }

    FlowNodeCatalog::~FlowNodeCatalog() = default;

    bool FlowNodeType::hasExecutionCompiler() const noexcept
    {
        return registration_.compile_execution != nullptr;
    }

    EFlowValueEvaluation FlowNodeType::valueEvaluation() const noexcept
    {
        return registration_.value_evaluation;
    }

    FlowForgeResult<void> FlowNodeType::compileExecution(
        const FlowNodePayload& payload,
        FlowExecutionCompiler& compiler
    ) const noexcept
    {
        if (!registration_.compile_execution)
        {
            return cxx::unexpected(invalid("node does not have an execution compiler"));
        }
        auto accepted = validate(payload);
        if (!accepted)
        {
            return cxx::unexpected(std::move(accepted.error()));
        }
        auto schema = describePins(payload);
        if (!schema)
        {
            return cxx::unexpected(std::move(schema.error()));
        }
        return registration_.compile_execution(payload, compiler);
    }

    cxx::expected<void, EFlowNodeCatalogError> FlowNodeCatalog::add(std::span<const FlowNodeRegistration> registrations
    ) noexcept
    {
        for (std::size_t i = 0; i != registrations.size(); ++i)
        {
            const auto& value = registrations[i];
            const auto operation = detail::builtinSourceKind(value.identity.canonical_name);
            const auto* builtin = builtinRegistration(value.identity.canonical_name);
            const bool is_unmigrated_builtin = operation != detail::EBuiltinSourceKind::EXTENSION && builtin == nullptr;
            const bool is_reassigned_builtin = builtin && !matchesBuiltin(value, *builtin);
            const bool is_invalid_registration =
                !validRegistration(value) || is_unmigrated_builtin || is_reassigned_builtin;
            if (is_invalid_registration)
            {
                return cxx::unexpected(EFlowNodeCatalogError::INVALID_REGISTRATION);
            }
            using Result = cxx::expected<void, EFlowNodeCatalogError>;
            const auto check = [&](const graph::GraphNodeTypeIdentity& previous) noexcept -> Result
            {
                if (value.identity.id != previous.id)
                {
                    return {};
                }
                return cxx::unexpected(
                    value.identity.canonical_name == previous.canonical_name ? EFlowNodeCatalogError::DUPLICATE_TYPE
                                                                             : EFlowNodeCatalogError::HASH_COLLISION
                );
            };
            for (const auto& reserved : detail::builtin_node_identities)
            {
                const bool has_matching_name = value.identity.canonical_name == reserved.name;
                const bool is_reserved_collision =
                    !has_matching_name && value.identity.id == graph::nodeTypeId(reserved.name);
                if (is_reserved_collision)
                {
                    return cxx::unexpected(EFlowNodeCatalogError::HASH_COLLISION);
                }
            }
            for (const auto& type : types_)
            {
                auto result = check(type->identity());
                if (!result)
                {
                    return result;
                }
            }
            for (std::size_t previous = 0; previous != i; ++previous)
            {
                auto result = check(registrations[previous].identity);
                if (!result)
                {
                    return result;
                }
            }
            if (value.identity.id != graph::nodeTypeId(value.identity.canonical_name))
            {
                return cxx::unexpected(EFlowNodeCatalogError::INVALID_REGISTRATION);
            }
        }
        std::vector<std::shared_ptr<const FlowNodeType>> candidates;
        candidates.reserve(registrations.size());
        for (const auto& value : registrations)
        {
            // The catalog can itself live in an extension's static copy of this module.
            // Keep code outside that copy's destructor and shared control-block return path.
            auto definition = std::unique_ptr<const FlowNodeType>(new FlowNodeType(value));
            candidates.push_back(object::pinCodeOwner(value.code, std::move(definition)));
        }
        types_.reserve(types_.size() + candidates.size());
        for (auto& candidate : candidates)
        {
            types_.push_back(std::move(candidate));
        }
        return {};
    }

    std::shared_ptr<const FlowNodeType> FlowNodeCatalog::find(graph::NodeTypeId id) const noexcept
    {
        const auto found = std::find_if(
            types_.begin(),
            types_.end(),
            [&](const auto& type) noexcept { return type->identity().id == id; }
        );
        return found == types_.end() ? nullptr : *found;
    }
} // namespace lux::flowforge
