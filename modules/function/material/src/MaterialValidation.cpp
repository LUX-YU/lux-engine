#include <lux/engine/description/Material.hpp>
#include <lux/engine/material/detail/MaterialValidation.hpp>
#include <lux/engine/material/graph/MaterialGraph.hpp>
#include <lux/engine/material/graph/Nodes.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <utility>

namespace lux::material::detail
{
    namespace
    {
        [[nodiscard]] bool validValueType(EValueType type) noexcept
        {
            switch (type)
            {
            case EValueType::FLOAT:
            case EValueType::VEC2:
            case EValueType::VEC3:
            case EValueType::VEC4:
                return true;
            }
            return false;
        }

        [[nodiscard]] std::size_t valueArity(EValueType type) noexcept
        {
            switch (type)
            {
            case EValueType::FLOAT:
                return 1U;
            case EValueType::VEC2:
                return 2U;
            case EValueType::VEC3:
                return 3U;
            case EValueType::VEC4:
                return 4U;
            }
            return 0U;
        }

        [[nodiscard]] bool validMathOp(EMathOp op) noexcept
        {
            switch (op)
            {
            case EMathOp::MUL:
            case EMathOp::ADD:
            case EMathOp::SUB:
            case EMathOp::DIV:
            case EMathOp::DOT:
            case EMathOp::MIN:
            case EMathOp::MAX:
            case EMathOp::POW:
            case EMathOp::STEP:
            case EMathOp::MOD:
            case EMathOp::CROSS:
            case EMathOp::REFLECT:
            case EMathOp::LERP:
            case EMathOp::SATURATE:
            case EMathOp::ONE_MINUS:
            case EMathOp::ABS:
            case EMathOp::SQRT:
            case EMathOp::FLOOR:
            case EMathOp::FRACT:
            case EMathOp::SIN:
            case EMathOp::COS:
            case EMathOp::NORMALIZE:
            case EMathOp::LENGTH:
                return true;
            }
            return false;
        }

        [[nodiscard]] bool finiteValues(const float (&values)[4]) noexcept
        {
            return std::all_of(std::begin(values), std::end(values), [](float value) { return std::isfinite(value); });
        }

        [[nodiscard]] lux::cxx::expected<void, MaterialCompileFailure> invalidGraph(
            std::string message,
            NodeId node = {},
            std::uint32_t pin = invalid_pin
        )
        {
            return lux::cxx::unexpected(
                MaterialCompileFailure{EMaterialCompileError::INVALID_GRAPH, std::move(message), node, pin}
            );
        }

        [[nodiscard]] lux::cxx::expected<void, MaterialCompileFailure> validatePins(
            const MaterialGraph& graph,
            NodeId id,
            const Node& node
        )
        {
            const auto validate = [&](const std::vector<DataPin>& pins,
                                      EPinDirection expected) -> lux::cxx::expected<void, MaterialCompileFailure>
            {
                for (std::uint32_t index = 0U; index < pins.size(); ++index)
                {
                    const auto& pin = pins[index];
                    const auto* structural = graph.topology().findPin(pin.id);
                    const auto structural_direction = expected == EPinDirection::OUTPUT
                                                          ? lux::graph::EPinDirection::OUTPUT
                                                          : lux::graph::EPinDirection::INPUT;
                    const bool is_invalid_structure = structural == nullptr || structural->owner != id ||
                                                      structural->direction != structural_direction;
                    const bool is_invalid_value = !validValueType(pin.type) || !finiteValues(pin.constant);
                    const bool is_invalid_identity = !pin.id.valid();
                    const bool is_direction_mismatch = pin.direction != expected;
                    const bool is_invalid_pin =
                        is_invalid_value || is_invalid_identity || is_direction_mismatch || is_invalid_structure;
                    if (is_invalid_pin)
                    {
                        return invalidGraph("invalid material pin contract", id, index);
                    }
                }
                return {};
            };

            if (auto result = validate(node.inputs(), EPinDirection::INPUT); !result)
            {
                return result;
            }
            return validate(node.outputs(), EPinDirection::OUTPUT);
        }

        [[nodiscard]] bool hasShape(const Node& node, std::size_t input_count, std::size_t output_count) noexcept
        {
            return node.inputs().size() == input_count && node.outputs().size() == output_count;
        }

        [[nodiscard]] lux::cxx::expected<void, MaterialCompileFailure> validateNode(
            const MaterialGraph& graph,
            NodeId id,
            const Node& node
        )
        {
            if (auto pins = validatePins(graph, id, node); !pins)
            {
                return pins;
            }

            const auto requireShape = [&](std::size_t inputs,
                                          std::size_t outputs) -> lux::cxx::expected<void, MaterialCompileFailure>
            {
                return hasShape(node, inputs, outputs) ? lux::cxx::expected<void, MaterialCompileFailure>{}
                                                       : invalidGraph("invalid material node pin arity", id);
            };
            const auto requirePinType = [&](bool input, std::size_t index, EValueType type
                                        ) -> lux::cxx::expected<void, MaterialCompileFailure>
            {
                const auto& pins = input ? node.inputs() : node.outputs();
                return index < pins.size() && pins[index].type == type
                           ? lux::cxx::expected<void, MaterialCompileFailure>{}
                           : invalidGraph(
                                 "material node pin type does not match its payload",
                                 id,
                                 static_cast<std::uint32_t>(index)
                             );
            };

            switch (node.kind())
            {
            case EMatNodeKind::CONSTANT:
            {
                if (auto shape = requireShape(0U, 1U); !shape)
                {
                    return shape;
                }
                const auto& value = static_cast<const ConstantNode&>(node);
                const bool is_invalid_value = !validValueType(value.value_type) || !finiteValues(value.value);
                if (is_invalid_value)
                {
                    return invalidGraph("invalid Constant node payload", id);
                }
                return requirePinType(false, 0U, value.value_type);
            }
            case EMatNodeKind::INPUT:
            {
                if (auto shape = requireShape(0U, 1U); !shape)
                {
                    return shape;
                }
                const auto& input = static_cast<const InputNode&>(node);
                const auto* description = materialInputDescription(input.input);
                if (description == nullptr)
                {
                    return invalidGraph("invalid Material input enum", id);
                }
                return requirePinType(false, 0U, description->type);
            }
            case EMatNodeKind::SAMPLE_TEXTURE:
            {
                if (auto shape = requireShape(1U, 1U); !shape)
                {
                    return shape;
                }
                const auto& sample = static_cast<const SampleTextureNode&>(node);
                if (sample.texture_slot >= graph.texture_slots.size())
                {
                    return invalidGraph("SampleTexture references an undeclared texture slot", id);
                }
                if (auto input = requirePinType(true, 0U, EValueType::VEC2); !input)
                {
                    return input;
                }
                return requirePinType(false, 0U, EValueType::VEC4);
            }
            case EMatNodeKind::PARAM:
            {
                if (auto shape = requireShape(0U, 1U); !shape)
                {
                    return shape;
                }
                const auto& parameter = static_cast<const ParamNode&>(node);
                const bool has_slot = parameter.param_slot < graph.param_slots.size();
                const bool is_type_mismatch =
                    has_slot && graph.param_slots[parameter.param_slot].type != parameter.type;
                const bool is_invalid_parameter = !validValueType(parameter.type) || !has_slot || is_type_mismatch;
                if (is_invalid_parameter)
                {
                    return invalidGraph("invalid Param node payload", id);
                }
                return requirePinType(false, 0U, parameter.type);
            }
            case EMatNodeKind::MATH:
            {
                if (auto shape = requireShape(2U, 1U); !shape)
                {
                    return shape;
                }
                const auto& math = static_cast<const MathNode&>(node);
                const bool is_unsupported_operation = !validMathOp(math.op) || math.op == EMathOp::LERP;
                const bool is_invalid_operand = !validValueType(math.operand_type);
                const bool is_invalid_math = is_unsupported_operation || is_invalid_operand;
                if (is_invalid_math)
                {
                    return invalidGraph("invalid or unsupported Math node payload", id);
                }
                const bool requires_vector = math.op == EMathOp::DOT || math.op == EMathOp::CROSS;
                const bool is_scalar_mismatch = requires_vector && math.operand_type == EValueType::FLOAT;
                if (is_scalar_mismatch)
                {
                    return invalidGraph("Dot/Cross require vector operands", id);
                }
                const bool is_cross_mismatch = math.op == EMathOp::CROSS && math.operand_type != EValueType::VEC3;
                if (is_cross_mismatch)
                {
                    return invalidGraph("Cross requires Vec3 operands", id);
                }
                if (auto first = requirePinType(true, 0U, math.operand_type); !first)
                {
                    return first;
                }
                if (auto second = requirePinType(true, 1U, math.operand_type); !second)
                {
                    return second;
                }
                return requirePinType(false, 0U, math.operand_type);
            }
            case EMatNodeKind::SWIZZLE:
            {
                if (auto shape = requireShape(1U, 1U); !shape)
                {
                    return shape;
                }
                const auto& swizzle = static_cast<const SwizzleNode&>(node);
                const auto source_arity = valueArity(swizzle.source_type);
                const auto output_arity = valueArity(swizzle.out_type);
                const bool is_invalid_arity = source_arity == 0U || output_arity == 0U;
                if (is_invalid_arity)
                {
                    return invalidGraph("invalid Swizzle node value type", id);
                }
                for (std::size_t component = 0U; component < std::size(swizzle.components); ++component)
                {
                    const bool is_used = component < output_arity;
                    const bool is_invalid_component = swizzle.components[component] > 3U ||
                                                      (is_used && swizzle.components[component] >= source_arity);
                    if (is_invalid_component)
                    {
                        return invalidGraph("invalid Swizzle component", id, static_cast<std::uint32_t>(component));
                    }
                }
                if (auto input = requirePinType(true, 0U, swizzle.source_type); !input)
                {
                    return input;
                }
                return requirePinType(false, 0U, swizzle.out_type);
            }
            case EMatNodeKind::CONSTRUCT:
            {
                const auto& construct = static_cast<const ConstructNode&>(node);
                const auto arity = valueArity(construct.out_type);
                const bool is_invalid_shape = arity == 0U || !hasShape(node, arity, 1U);
                if (is_invalid_shape)
                {
                    return invalidGraph("invalid Construct node payload or arity", id);
                }
                for (std::size_t input = 0U; input < arity; ++input)
                {
                    if (auto type = requirePinType(true, input, EValueType::FLOAT); !type)
                    {
                        return type;
                    }
                }
                return requirePinType(false, 0U, construct.out_type);
            }
            case EMatNodeKind::DECODE_NORMAL:
            case EMatNodeKind::TBN_TRANSFORM:
                if (auto shape = requireShape(1U, 1U); !shape)
                {
                    return shape;
                }
                if (auto input = requirePinType(true, 0U, EValueType::VEC3); !input)
                {
                    return input;
                }
                return requirePinType(false, 0U, EValueType::VEC3);
            case EMatNodeKind::OUTPUT_SURFACE:
                if (!hasShape(node, std::size(kMaterialAttributes), 0U))
                {
                    return invalidGraph("invalid OutputSurface pin arity", id);
                }
                for (std::size_t input = 0U; input < std::size(kMaterialAttributes); ++input)
                {
                    if (auto type = requirePinType(true, input, kMaterialAttributes[input].type); !type)
                    {
                        return type;
                    }
                }
                return {};
            case EMatNodeKind::INVALID:
            case EMatNodeKind::COUNT:
                return invalidGraph("invalid material node kind", id);
            }
            return invalidGraph("unknown material node kind", id);
        }

    } // namespace

    [[nodiscard]] lux::cxx::expected<void, MaterialCompileFailure> validateMaterialGraph(const MaterialGraph& graph
    ) noexcept
    {
        const bool has_nodes = !graph.nodes().empty();
        const bool exceeds_parameters = graph.param_slots.size() > rdesc::MaterialDescription::kMaxParams;
        const bool exceeds_textures = graph.texture_slots.size() > rdesc::MaterialDescription::kMaxTextures;
        const bool is_invalid_capacity = !has_nodes || exceeds_parameters || exceeds_textures;
        if (is_invalid_capacity)
        {
            return lux::cxx::unexpected(
                MaterialCompileFailure{EMaterialCompileError::INVALID_GRAPH, "invalid material graph capacity"}
            );
        }

        const auto alpha_mode = static_cast<std::uint8_t>(graph.render_state.alpha_mode);
        const auto shading_model = static_cast<std::uint8_t>(graph.shading_model);
        const bool is_invalid_alpha = alpha_mode > static_cast<std::uint8_t>(rdesc::EAlphaMode::BLEND);
        const bool is_invalid_shading = shading_model > static_cast<std::uint8_t>(rdesc::ELightingTechnique::GRAPH);
        const bool is_invalid_cutoff = !std::isfinite(graph.render_state.alpha_cutoff);
        const bool is_invalid_state = is_invalid_alpha || is_invalid_shading || is_invalid_cutoff;
        if (is_invalid_state)
        {
            return lux::cxx::unexpected(
                MaterialCompileFailure{EMaterialCompileError::INVALID_GRAPH, "invalid material render state"}
            );
        }

        for (const auto& texture : graph.texture_slots)
        {
            if (texture.texture.isNull())
            {
                return lux::cxx::unexpected(MaterialCompileFailure{
                    EMaterialCompileError::INVALID_GRAPH,
                    "material texture slot has a null AssetId"
                });
            }
        }
        for (const auto& parameter : graph.param_slots)
        {
            if (!validValueType(parameter.type))
            {
                return invalidGraph("material parameter has an invalid value type");
            }
            for (const float value : parameter.dflt)
            {
                if (!std::isfinite(value))
                {
                    return invalidGraph("material parameter default is not finite");
                }
            }
        }

        for (const auto& [id, node] : graph.nodes())
        {
            const bool has_payload = node != nullptr;
            const bool has_identity = id.valid() && graph.topology().findNode(id) != nullptr;
            const bool is_invalid_node = !has_payload || !has_identity;
            if (is_invalid_node)
            {
                return invalidGraph("material graph contains an invalid node identity", id);
            }
            if (auto validation = validateNode(graph, id, *node); !validation)
            {
                return validation;
            }
        }

        for (const auto& [id, node] : graph.nodes())
        {
            for (std::uint32_t pin_index = 0U; pin_index < node->inputs().size(); ++pin_index)
            {
                const auto source = graph.source(id, pin_index);
                if (!source.valid())
                {
                    continue;
                }
                const auto* source_node = graph.node(source.node);
                const bool has_source = source_node != nullptr;
                const bool has_pin = has_source && source.pin < source_node->outputs().size();
                if (!has_pin)
                {
                    return invalidGraph("material connection references an invalid output", source.node, source.pin);
                }
            }
        }
        return {};
    }
} // namespace lux::material::detail
