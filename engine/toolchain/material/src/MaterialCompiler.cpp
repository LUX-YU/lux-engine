#include <lux/engine/material/Compiler.hpp>

#include <lux/engine/material/compiler/Backend.hpp>
#include <lux/engine/material/compiler/Lowering.hpp>
#include <lux/engine/material/graph/Nodes.hpp>

#include <algorithm>
#include <cmath>
#include <iterator>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace lux::material
{
    namespace
    {
        [[nodiscard]] MaterialCompileFailure failure(
            EMaterialCompileError code,
            std::string message,
            NodeId node_id = {},
            std::uint32_t pin_index = invalid_pin
        ) noexcept
        {
            return MaterialCompileFailure{code, std::move(message), node_id, pin_index};
        }

        [[nodiscard]] bool validSpirv(const std::vector<std::uint32_t>& words) noexcept
        {
            return words.size() >= 5U && words.front() == 0x07230203U;
        }

        [[nodiscard]] EMaterialCompileError shaderFailureCode(const std::string& message) noexcept
        {
            const bool is_compile_failure = message.find("shaderc failed") != std::string::npos ||
                message.find("SPIR-V reflection failed") != std::string::npos;
            return is_compile_failure ? EMaterialCompileError::SHADER_COMPILATION_FAILURE
                                      : EMaterialCompileError::SHADER_EMISSION_FAILURE;
        }

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
            case EValueType::FLOAT: return 1U;
            case EValueType::VEC2: return 2U;
            case EValueType::VEC3: return 3U;
            case EValueType::VEC4: return 4U;
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
            return std::all_of(
                std::begin(values),
                std::end(values),
                [](float value) { return std::isfinite(value); }
            );
        }

        [[nodiscard]] lux::cxx::expected<void, MaterialCompileFailure> invalidGraph(
            std::string message,
            NodeId node = {},
            std::uint32_t pin = invalid_pin
        )
        {
            return lux::cxx::unexpected(failure(EMaterialCompileError::INVALID_GRAPH, std::move(message), node, pin));
        }

        [[nodiscard]] lux::cxx::expected<void, MaterialCompileFailure>
        validatePins(const MaterialGraph& graph, const Node& node)
        {
            const auto validate = [&](const std::vector<DataPin>& pins, EPinDirection expected)
                -> lux::cxx::expected<void, MaterialCompileFailure>
            {
                for (std::uint32_t index = 0U; index < pins.size(); ++index)
                {
                    const auto& pin = pins[index];
                    const auto* structural = graph.topology().findPin(pin.id);
                    const auto structural_direction = expected == EPinDirection::OUTPUT ?
                        lux::graph::EPinDirection::OUTPUT : lux::graph::EPinDirection::INPUT;
                    const bool is_invalid_structure = structural == nullptr || structural->owner != node.id() ||
                        structural->direction != structural_direction;
                    if (!validValueType(pin.type) || pin.direction != expected || !pin.id.valid() ||
                        is_invalid_structure || !finiteValues(pin.constant))
                        return invalidGraph("invalid material pin contract", node.id(), index);
                }
                return {};
            };

            if (auto result = validate(node.inputs(), EPinDirection::INPUT); !result)
                return result;
            return validate(node.outputs(), EPinDirection::OUTPUT);
        }

        [[nodiscard]] bool hasShape(
            const Node& node,
            std::size_t input_count,
            std::size_t output_count
        ) noexcept
        {
            return node.inputs().size() == input_count && node.outputs().size() == output_count;
        }

        [[nodiscard]] lux::cxx::expected<void, MaterialCompileFailure>
        validateNode(const MaterialGraph& graph, const Node& node)
        {
            if (auto pins = validatePins(graph, node); !pins)
                return pins;

            const auto requireShape = [&](std::size_t inputs, std::size_t outputs)
                -> lux::cxx::expected<void, MaterialCompileFailure>
            {
                return hasShape(node, inputs, outputs)
                    ? lux::cxx::expected<void, MaterialCompileFailure>{}
                    : invalidGraph("invalid material node pin arity", node.id());
            };
            const auto requirePinType = [&](bool input, std::size_t index, EValueType type)
                -> lux::cxx::expected<void, MaterialCompileFailure>
            {
                const auto& pins = input ? node.inputs() : node.outputs();
                return index < pins.size() && pins[index].type == type
                    ? lux::cxx::expected<void, MaterialCompileFailure>{}
                    : invalidGraph("material node pin type does not match its payload",
                                   node.id(), static_cast<std::uint32_t>(index));
            };

            switch (node.kind())
            {
            case EMatNodeKind::CONSTANT:
            {
                if (auto shape = requireShape(0U, 1U); !shape)
                    return shape;
                const auto& value = static_cast<const ConstantNode&>(node);
                if (!validValueType(value.value_type) || !finiteValues(value.value))
                    return invalidGraph("invalid Constant node payload", node.id());
                return requirePinType(false, 0U, value.value_type);
            }
            case EMatNodeKind::INPUT:
            {
                if (auto shape = requireShape(0U, 1U); !shape)
                    return shape;
                const auto& input = static_cast<const InputNode&>(node);
                const auto* description = materialInputDescription(input.input);
                if (description == nullptr)
                    return invalidGraph("invalid Material input enum", node.id());
                return requirePinType(false, 0U, description->type);
            }
            case EMatNodeKind::SAMPLE_TEXTURE:
            {
                if (auto shape = requireShape(1U, 1U); !shape)
                    return shape;
                const auto& sample = static_cast<const SampleTextureNode&>(node);
                if (sample.texture_slot >= graph.texture_slots.size())
                    return invalidGraph("SampleTexture references an undeclared texture slot", node.id());
                if (auto input = requirePinType(true, 0U, EValueType::VEC2); !input)
                    return input;
                return requirePinType(false, 0U, EValueType::VEC4);
            }
            case EMatNodeKind::PARAM:
            {
                if (auto shape = requireShape(0U, 1U); !shape)
                    return shape;
                const auto& parameter = static_cast<const ParamNode&>(node);
                if (!validValueType(parameter.type) || parameter.param_slot >= graph.param_slots.size() ||
                    graph.param_slots[parameter.param_slot].type != parameter.type)
                    return invalidGraph("invalid Param node payload", node.id());
                return requirePinType(false, 0U, parameter.type);
            }
            case EMatNodeKind::MATH:
            {
                if (auto shape = requireShape(2U, 1U); !shape)
                    return shape;
                const auto& math = static_cast<const MathNode&>(node);
                if (!validMathOp(math.op) || math.op == EMathOp::LERP || !validValueType(math.operand_type))
                    return invalidGraph("invalid or unsupported Math node payload", node.id());
                if ((math.op == EMathOp::DOT || math.op == EMathOp::CROSS) &&
                    math.operand_type == EValueType::FLOAT)
                    return invalidGraph("Dot/Cross require vector operands", node.id());
                if (math.op == EMathOp::CROSS && math.operand_type != EValueType::VEC3)
                    return invalidGraph("Cross requires Vec3 operands", node.id());
                if (auto first = requirePinType(true, 0U, math.operand_type); !first)
                    return first;
                if (auto second = requirePinType(true, 1U, math.operand_type); !second)
                    return second;
                return requirePinType(false, 0U, math.operand_type);
            }
            case EMatNodeKind::SWIZZLE:
            {
                if (auto shape = requireShape(1U, 1U); !shape)
                    return shape;
                const auto& swizzle = static_cast<const SwizzleNode&>(node);
                const auto source_arity = valueArity(swizzle.source_type);
                const auto output_arity = valueArity(swizzle.out_type);
                if (source_arity == 0U || output_arity == 0U)
                    return invalidGraph("invalid Swizzle node value type", node.id());
                for (std::size_t component = 0U; component < std::size(swizzle.components); ++component)
                {
                    const bool is_used = component < output_arity;
                    const bool is_invalid_component = swizzle.components[component] > 3U ||
                        (is_used && swizzle.components[component] >= source_arity);
                    if (is_invalid_component)
                        return invalidGraph("invalid Swizzle component", node.id(),
                                            static_cast<std::uint32_t>(component));
                }
                if (auto input = requirePinType(true, 0U, swizzle.source_type); !input)
                    return input;
                return requirePinType(false, 0U, swizzle.out_type);
            }
            case EMatNodeKind::CONSTRUCT:
            {
                const auto& construct = static_cast<const ConstructNode&>(node);
                const auto arity = valueArity(construct.out_type);
                if (arity == 0U || !hasShape(node, arity, 1U))
                    return invalidGraph("invalid Construct node payload or arity", node.id());
                for (std::size_t input = 0U; input < arity; ++input)
                    if (auto type = requirePinType(true, input, EValueType::FLOAT); !type)
                        return type;
                return requirePinType(false, 0U, construct.out_type);
            }
            case EMatNodeKind::DECODE_NORMAL:
            case EMatNodeKind::TBN_TRANSFORM:
                if (auto shape = requireShape(1U, 1U); !shape)
                    return shape;
                if (auto input = requirePinType(true, 0U, EValueType::VEC3); !input)
                    return input;
                return requirePinType(false, 0U, EValueType::VEC3);
            case EMatNodeKind::OUTPUT_SURFACE:
                if (!hasShape(node, std::size(kMaterialAttributes), 0U))
                    return invalidGraph("invalid OutputSurface pin arity", node.id());
                for (std::size_t input = 0U; input < std::size(kMaterialAttributes); ++input)
                    if (auto type = requirePinType(true, input, kMaterialAttributes[input].type); !type)
                        return type;
                return {};
            case EMatNodeKind::INVALID:
            case EMatNodeKind::COUNT:
                return invalidGraph("invalid material node kind", node.id());
            }
            return invalidGraph("unknown material node kind", node.id());
        }

        [[nodiscard]] lux::cxx::expected<void, MaterialCompileFailure>
        validateGraph(const MaterialGraph& graph)
        {
            if (graph.nodes().empty() || graph.param_slots.size() > rdesc::MaterialDescription::kMaxParams ||
                graph.texture_slots.size() > rdesc::MaterialDescription::kMaxTextures)
                return lux::cxx::unexpected(failure(EMaterialCompileError::INVALID_GRAPH,
                                                     "invalid material graph capacity"));

            const auto alpha_mode = static_cast<std::uint8_t>(graph.render_state.alpha_mode);
            const auto shading_model = static_cast<std::uint8_t>(graph.shading_model);
            if (alpha_mode > static_cast<std::uint8_t>(rdesc::EAlphaMode::Blend) ||
                shading_model > static_cast<std::uint8_t>(rdesc::ELightingTechnique::Graph) ||
                !std::isfinite(graph.render_state.alpha_cutoff))
                return lux::cxx::unexpected(failure(EMaterialCompileError::INVALID_GRAPH,
                                                     "invalid material render state"));

            for (const auto& texture : graph.texture_slots)
                if (texture.texture.isNull())
                    return lux::cxx::unexpected(failure(EMaterialCompileError::INVALID_GRAPH,
                                                         "material texture slot has a null AssetId"));
            for (const auto& parameter : graph.param_slots)
            {
                if (!validValueType(parameter.type))
                    return invalidGraph("material parameter has an invalid value type");
                for (const float value : parameter.dflt)
                    if (!std::isfinite(value))
                        return invalidGraph("material parameter default is not finite");
            }

            for (const auto& [id, node] : graph.nodes())
            {
                if (!node || !id.valid() || node->id() != id || graph.topology().findNode(id) == nullptr)
                    return invalidGraph("material graph contains an invalid node identity", id);
                if (auto validation = validateNode(graph, *node); !validation)
                    return validation;
            }

            for (const auto& [id, node] : graph.nodes())
            {
                for (std::uint32_t pin_index = 0U; pin_index < node->inputs().size(); ++pin_index)
                {
                    const auto source = graph.source(id, pin_index);
                    if (!source.valid())
                        continue;
                    const auto* source_node = graph.node(source.node);
                    if (source_node == nullptr || source.pin >= source_node->outputs().size())
                        return invalidGraph("material connection references an invalid output",
                                            source.node, source.pin);
                }
            }
            return {};
        }
    } // namespace

    lux::cxx::expected<rdesc::MaterialDescription, MaterialCompileFailure>
    compileMaterial(const MaterialGraph& graph) noexcept
    {
        try
        {
            if (auto validation = validateGraph(graph); !validation)
                return lux::cxx::unexpected(std::move(validation.error()));

            auto lowered = compiler::lowerMaterial(graph);
            if (!lowered)
                return lux::cxx::unexpected(std::move(lowered.error()));

            const auto compile_pass = [&](shadergen::glsl::EMaterialPass pass)
                -> lux::cxx::expected<shadergen::glsl::CompiledShader, MaterialCompileFailure>
            {
                shadergen::glsl::EmitParams parameters;
                parameters.pass = pass;
                parameters.shading_model = lowered->shading_model;
                parameters.alpha_mode = lowered->alpha_mode;
                parameters.alpha_cutoff = lowered->alpha_cutoff;
                auto compiled = shadergen::glsl::compileToSpirv(lowered->shader, parameters);
                if (!compiled)
                {
                    auto message = std::move(compiled.error());
                    const auto code = shaderFailureCode(message);
                    return lux::cxx::unexpected(failure(code, std::move(message)));
                }
                return std::move(*compiled);
            };

            auto gbuffer = compile_pass(shadergen::glsl::EMaterialPass::GBUFFER);
            if (!gbuffer)
                return lux::cxx::unexpected(std::move(gbuffer.error()));
            auto forward = compile_pass(shadergen::glsl::EMaterialPass::FORWARD);
            if (!forward)
                return lux::cxx::unexpected(std::move(forward.error()));

            rdesc::MaterialDescription description;
            description.parameter_count = static_cast<std::uint32_t>(graph.param_slots.size());
            for (std::uint32_t parameter = 0U; parameter < description.parameter_count; ++parameter)
                std::copy_n(graph.param_slots[parameter].dflt,
                            description.parameter_defaults[parameter].size(),
                            description.parameter_defaults[parameter].begin());
            description.alpha_mode = graph.render_state.alpha_mode;
            description.double_sided = graph.render_state.double_sided;
            description.gbuffer_spirv = std::move(gbuffer->spirv);
            description.gbuffer_info = std::move(gbuffer->info);
            description.forward_spirv = std::move(forward->spirv);
            description.forward_info = std::move(forward->info);
            for (std::uint32_t slot = 0U; slot < graph.texture_slots.size(); ++slot)
                description.texture_slot_ids[slot] = graph.texture_slots[slot].texture;

            if (!validSpirv(description.gbuffer_spirv) || !validSpirv(description.forward_spirv))
                return lux::cxx::unexpected(failure(EMaterialCompileError::INVALID_RESULT,
                                                     "compiler produced an invalid MaterialDescription"));
            return description;
        }
        catch (const std::bad_alloc&)
        {
            return lux::cxx::unexpected(failure(EMaterialCompileError::ALLOCATION_FAILURE,
                                                 "allocation failure"));
        }
        catch (...)
        {
            return lux::cxx::unexpected(failure(EMaterialCompileError::INVALID_RESULT,
                                                 "foreign material compiler failure"));
        }
    }
} // namespace lux::material
