#include <lux/engine/flowforge/FlowNodeCatalog.hpp>
#include <lux/engine/flowforge/detail/FlowNodeIdentity.hpp>

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
            const bool has_callbacks = value.create && value.describe_pins && value.validate && value.compile;
            const bool has_codec_pair = (value.encode != nullptr) == (value.decode != nullptr);
            return has_identity && has_payload_type && has_code && has_callbacks && has_codec_pair;
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

    FlowForgeResult<std::string> FlowNodeType::encode(const FlowNodePayload& payload) const noexcept
    {
        auto schema = describePins(payload);
        if (!schema)
        {
            return cxx::unexpected(std::move(schema.error()));
        }
        if (!registration_.encode)
        {
            return cxx::unexpected(invalid("node definition has no source codec"));
        }
        return registration_.encode(payload);
    }

    FlowForgeResult<FlowNodePayload> FlowNodeType::decode(std::string_view bytes) const noexcept
    {
        if (!registration_.decode)
        {
            return cxx::unexpected(invalid("node definition has no source codec"));
        }
        auto payload = registration_.decode(bytes, registration_.code);
        if (!payload)
        {
            return cxx::unexpected(std::move(payload.error()));
        }
        auto schema = describePins(*payload);
        if (!schema)
        {
            return cxx::unexpected(std::move(schema.error()));
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
            const bool is_valid_type = pin.type != nullptr;
            const bool is_valid_direction =
                pin.direction == graph::EPinDirection::INPUT || pin.direction == graph::EPinDirection::OUTPUT;
            const bool has_duplicate = std::any_of(
                pins->begin(),
                pins->begin() + i,
                [&](const auto& existing) noexcept { return existing.semantic == pin.semantic; }
            );
            const bool is_invalid_pin = !has_identity || !is_valid_type || !is_valid_direction || has_duplicate;
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
            if (pin.direction != graph::EPinDirection::OUTPUT)
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

    cxx::expected<void, EFlowNodeCatalogError>
    FlowNodeCatalog::add(std::span<const FlowNodeRegistration> registrations) noexcept
    {
        for (std::size_t i = 0; i != registrations.size(); ++i)
        {
            const auto& value = registrations[i];
            const bool is_builtin_identity =
                detail::builtinNodeOperation(value.identity.canonical_name) != ENodeOperation::REGISTERED_VALUE;
            if (!validRegistration(value) || is_builtin_identity)
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
            for (const auto& builtin : detail::builtin_node_identities)
            {
                if (value.identity.id == graph::nodeTypeId(builtin.name))
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
            auto definition = std::shared_ptr<const FlowNodeType>(new FlowNodeType(value));
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
