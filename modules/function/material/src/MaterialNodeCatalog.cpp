#include <lux/engine/material/MaterialNodeCatalog.hpp>

#include <algorithm>

namespace lux::material
{
    namespace
    {
        // The two existing scalar/vector contracts are ordinal-compatible; changes must be explicit.
        static_assert(static_cast<shadergen::EValueType>(EValueType::FLOAT) == shadergen::EValueType::FLOAT);
        static_assert(static_cast<shadergen::EValueType>(EValueType::VEC2) == shadergen::EValueType::VEC2);
        static_assert(static_cast<shadergen::EValueType>(EValueType::VEC3) == shadergen::EValueType::VEC3);
        static_assert(static_cast<shadergen::EValueType>(EValueType::VEC4) == shadergen::EValueType::VEC4);

        MaterialCompileFailure invalid(std::string message, std::uint32_t pin = ~std::uint32_t{0}) noexcept
        {
            return {EMaterialCompileError::INVALID_GRAPH, std::move(message), {}, pin};
        }

        bool canonicalName(std::string_view name) noexcept
        {
            if (name.empty())
            {
                return false;
            }
            const auto initial = [](char c) noexcept
            { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
            if (!initial(name.front()))
            {
                return false;
            }
            return std::all_of(
                name.begin(),
                name.end(),
                [&](char c) noexcept { return initial(c) || (c >= '0' && c <= '9') || c == '.' || c == '-'; }
            );
        }

        bool validRegistration(const MaterialNodeRegistration& value) noexcept
        {
            const bool has_identity = value.identity.id.valid() && value.identity.version != 0 &&
                                      canonicalName(value.identity.canonical_name);
            const bool has_payload_type = value.payload_type.isValid();
            const bool has_code = value.code.valid();
            const bool has_callbacks = value.create && value.describe_pins && value.validate && value.compile;
            return has_identity && has_payload_type && has_code && has_callbacks;
        }
    } // namespace

    MaterialNodeType::MaterialNodeType(MaterialNodeRegistration registration) noexcept
        : payload_type_name_(registration.payload_type.name()), registration_(std::move(registration))
    {
        registration_.payload_type = cxx::TypeToken{registration_.payload_type.hash(), payload_type_name_};
    }

    MaterialNodeType::~MaterialNodeType() = default;

    const graph::GraphNodeTypeIdentity& MaterialNodeType::identity() const noexcept
    {
        return registration_.identity;
    }

    bool MaterialNodeType::accepts(const MaterialNodePayload& payload) const noexcept
    {
        return payload.value_ && payload.type_ == registration_.payload_type &&
               payload.code_.sameOwner(registration_.code);
    }

    MaterialNodeResult<MaterialNodePayload> MaterialNodeType::create() const noexcept
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

    MaterialNodeResult<void> MaterialNodeType::validate(const MaterialNodePayload& payload) const noexcept
    {
        if (!accepts(payload))
        {
            return cxx::unexpected(invalid("node payload does not belong to this definition"));
        }
        return registration_.validate(payload);
    }

    MaterialNodeResult<std::vector<MaterialPinDeclaration>> MaterialNodeType::describePins(
        const MaterialNodePayload& payload
    ) const noexcept
    {
        auto validation = validate(payload);
        if (!validation)
        {
            return cxx::unexpected(std::move(validation.error()));
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
            const bool is_valid_type = pin.type >= EValueType::FLOAT && pin.type <= EValueType::VEC4;
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

    MaterialNodeResult<std::vector<std::uint32_t>> MaterialNodeType::compile(
        const MaterialNodePayload& payload,
        std::span<const std::uint32_t> inputs,
        shadergen::ShaderIR& candidate
    ) const noexcept
    {
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
            const bool has_input = input_count < inputs.size();
            const bool has_value = has_input && inputs[input_count] < candidate.values.size();
            const bool has_matching_type =
                has_value && candidate.values[inputs[input_count]].type == static_cast<shadergen::EValueType>(pin.type);
            if (!has_matching_type)
            {
                return cxx::unexpected(invalid("node input does not match its pin declaration"));
            }
            ++input_count;
        }
        if (input_count != inputs.size())
        {
            return cxx::unexpected(invalid("node input count does not match its pin declarations"));
        }
        auto outputs = registration_.compile(payload, inputs, candidate);
        if (!outputs)
        {
            return cxx::unexpected(std::move(outputs.error()));
        }
        if (outputs->size() != output_count)
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
            const auto value = (*outputs)[output_index++];
            const bool has_value = value < candidate.values.size();
            const bool has_matching_type =
                has_value && candidate.values[value].type == static_cast<shadergen::EValueType>(pin.type);
            if (!has_matching_type)
            {
                return cxx::unexpected(invalid("node compiler returned an invalid output value"));
            }
        }
        return outputs;
    }

    MaterialNodeCatalog::~MaterialNodeCatalog() = default;

    cxx::expected<void, EMaterialNodeCatalogError> MaterialNodeCatalog::add(
        std::span<const MaterialNodeRegistration> registrations
    ) noexcept
    {
        for (std::size_t i = 0; i != registrations.size(); ++i)
        {
            const auto& value = registrations[i];
            if (!validRegistration(value))
            {
                return cxx::unexpected(EMaterialNodeCatalogError::INVALID_REGISTRATION);
            }
            const auto check = [&](const graph::GraphNodeTypeIdentity& previous
                               ) noexcept -> cxx::expected<void, EMaterialNodeCatalogError>
            {
                if (value.identity.id != previous.id)
                {
                    return {};
                }
                return cxx::unexpected(
                    value.identity.canonical_name == previous.canonical_name ? EMaterialNodeCatalogError::DUPLICATE_TYPE
                                                                             : EMaterialNodeCatalogError::HASH_COLLISION
                );
            };
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
                return cxx::unexpected(EMaterialNodeCatalogError::INVALID_REGISTRATION);
            }
        }
        std::vector<std::shared_ptr<const MaterialNodeType>> candidates;
        candidates.reserve(registrations.size());
        for (const auto& value : registrations)
        {
            candidates.emplace_back(new MaterialNodeType(value));
        }
        types_.reserve(types_.size() + candidates.size());
        for (auto& candidate : candidates)
        {
            types_.push_back(std::move(candidate));
        }
        return {};
    }

    std::shared_ptr<const MaterialNodeType> MaterialNodeCatalog::find(graph::NodeTypeId id) const noexcept
    {
        const auto found = std::find_if(
            types_.begin(),
            types_.end(),
            [&](const auto& type) noexcept { return type->identity().id == id; }
        );
        return found == types_.end() ? nullptr : *found;
    }
} // namespace lux::material
