#include <lux/engine/material/MaterialNodeCatalog.hpp>

#include <algorithm>

namespace lux::material
{
    namespace
    {
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
            const bool has_callbacks = value.create && value.describe_pins && value.validate && value.emit_slang;
            const bool has_role = value.role == EMaterialNodeRole::VALUE || value.role == EMaterialNodeRole::SURFACE;
            const bool has_codec_pair = (value.encode != nullptr) == (value.decode != nullptr);
            return has_identity && has_payload_type && has_code && has_callbacks && has_role && has_codec_pair;
        }
    } // namespace

    MaterialNodeType::MaterialNodeType(MaterialNodeRegistration registration) noexcept
        : payload_type_name_(registration.payload_type.name()), registration_(std::move(registration))
    {
        registration_.payload_type = cxx::basic_type_info{registration_.payload_type.hash(), payload_type_name_};
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

    EMaterialNodeRole MaterialNodeType::role() const noexcept
    {
        return registration_.role;
    }

    MaterialNodeResult<void>
    MaterialNodeType::validateBindings(const MaterialNodePayload& payload, const MaterialGraph& resources)
        const noexcept
    {
        auto accepted = validate(payload);
        if (!accepted)
        {
            return accepted;
        }
        return registration_.validate_bindings ? registration_.validate_bindings(payload, resources)
                                               : MaterialNodeResult<void>{};
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

    MaterialNodeResult<std::string> MaterialNodeType::encode(const MaterialNodePayload& payload) const noexcept
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

    MaterialNodeResult<MaterialNodePayload> MaterialNodeType::decode(std::string_view bytes) const noexcept
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

    MaterialNodeResult<std::vector<MaterialPinDeclaration>>
    MaterialNodeType::describePins(const MaterialNodePayload& payload) const noexcept
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
            const bool is_valid_type = pin.type >= EValueType::FLOAT && pin.type <= EValueType::VEC4;
            const bool is_valid_direction =
                pin.direction == graph::EPinDirection::INPUT || pin.direction == graph::EPinDirection::OUTPUT;
            const bool has_duplicate = std::any_of(
                pins->begin(),
                pins->begin() + i,
                [&](const auto& existing) noexcept { return existing.semantic == pin.semantic; }
            );
            const bool is_valid_use = pin.input_use == EMaterialInputUse::VALUE ||
                                      pin.input_use == EMaterialInputUse::CONNECTED_VALUE ||
                                      pin.input_use == EMaterialInputUse::UNUSED;
            const bool is_output_use_mismatch =
                pin.direction == graph::EPinDirection::OUTPUT && pin.input_use != EMaterialInputUse::VALUE;
            const bool is_invalid_pin = !has_identity || !is_valid_type || !is_valid_direction || has_duplicate ||
                                        !is_valid_use || is_output_use_mismatch;
            if (is_invalid_pin)
            {
                return
                cxx::unexpected(invalid("invalid or duplicate node pin declaration", static_cast<std::uint32_t>(i)));
            }
        }
        return pins;
    }

    MaterialNodeResult<void>
    MaterialNodeType::emitSlang(
        const MaterialNodePayload& payload,
        std::span<const std::string_view> inputs,
        std::span<const std::string_view> outputs,
        std::string& destination
    ) const noexcept
    {
        auto validation = validate(payload);
        if (!validation)
        {
            return validation;
        }
        auto pins = describePins(payload);
        if (!pins)
        {
            return cxx::unexpected(std::move(pins.error()));
        }
        std::size_t input_count = 0, output_count = 0;
        for (const auto& pin : *pins)
        {
            if (pin.direction == graph::EPinDirection::OUTPUT)
            {
                const bool is_invalid_output = output_count >= outputs.size() || outputs[output_count].empty();
                if (is_invalid_output)
                {
                    return cxx::unexpected(invalid("missing output identifier"));
                }
                ++output_count;
                continue;
            }
            if (input_count >= inputs.size())
            {
                return cxx::unexpected(invalid("missing node input"));
            }
            const bool is_absent = inputs[input_count].empty();
            const bool is_input_mismatch = (pin.input_use == EMaterialInputUse::VALUE && is_absent) ||
                                           (pin.input_use == EMaterialInputUse::UNUSED && !is_absent);
            if (is_input_mismatch)
            {
                return cxx::unexpected(invalid("node input demand mismatch", input_count));
            }
            ++input_count;
        }
        const bool is_count_mismatch = input_count != inputs.size() || output_count != outputs.size();
        if (is_count_mismatch)
        {
            return cxx::unexpected(invalid("node expression count mismatch"));
        }
        return registration_.emit_slang(payload, inputs, outputs, destination);
    }

    MaterialNodeCatalog::~MaterialNodeCatalog() = default;

    cxx::expected<void, EMaterialNodeCatalogError>
    MaterialNodeCatalog::add(std::span<const MaterialNodeRegistration> registrations, std::uint32_t abi) noexcept
    {
        if (abi != kMaterialNodeAbi)
        {
            return cxx::unexpected(EMaterialNodeCatalogError::INVALID_REGISTRATION);
        }
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
                return
                cxx::unexpected(
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
