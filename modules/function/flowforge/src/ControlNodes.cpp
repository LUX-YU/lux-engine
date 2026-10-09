#include <lux/engine/flowforge/ControlNodes.hpp>
#include <lux/engine/flowforge/FlowExecutionCompiler.hpp>
#include <lux/engine/flowforge/detail/FlowPinSchema.hpp>
#include <lux/engine/meta/Meta.hpp>

#include <charconv>
#include <type_traits>

namespace lux::flowforge
{
    namespace
    {
        using graph::EPinDirection;

        bool sequenceFits(std::size_t count) noexcept
        {
            constexpr auto max_additional = (std::uint64_t{1} << 56U) - 2U;
            const bool fits_semantic = count <= max_additional;
            const bool fits_storage = count <= std::vector<FlowPinDeclaration>{}.max_size() - 2U;
            return fits_semantic && fits_storage;
        }

        FlowForgeFailure invalidControl() noexcept
        {
            return {EFlowForgeError::INVALID_DESCRIPTION, "invalid control node parameters"};
        }

        template <class T> FlowForgeResult<std::unique_ptr<T>> cloneControl(const T& payload) noexcept
        {
            return std::make_unique<T>(payload);
        }

        template <class T> FlowForgeResult<void> validateControl(const FlowNodePayload& payload) noexcept
        {
            if constexpr (std::is_same_v<T, SequencePayload>)
            {
                if (!sequenceFits(payload.get<T>()->additional_outputs))
                {
                    return cxx::unexpected(invalidControl());
                }
            }
            return {};
        }

        template <class T>
        FlowForgeResult<void> compileControl(const FlowNodePayload& payload, FlowExecutionCompiler& compiler) noexcept
        {
            const auto input = [](std::size_t ordinal) noexcept
            { return detail::pinSemantic(EFlowPinRole::DATA, EPinDirection::INPUT, ordinal); };
            const auto output = [](std::size_t ordinal) noexcept
            { return detail::pinSemantic(EFlowPinRole::EXECUTION, EPinDirection::OUTPUT, ordinal); };
            if constexpr (std::is_same_v<T, StartPayload>)
            {
                return cxx::unexpected(FlowForgeFailure{EFlowForgeError::GRAPH_INVALID, "entry node reached mid-chain"}
                );
            }
            else if constexpr (std::is_same_v<T, BranchPayload>)
            {
                return compiler.branch(input(1), output(0), output(1));
            }
            else if constexpr (std::is_same_v<T, ForLoopPayload>)
            {
                const auto index = detail::pinSemantic(EFlowPinRole::DATA, EPinDirection::OUTPUT, 2);
                return compiler.forLoop(input(1), input(2), index, output(0), output(1));
            }
            else if constexpr (std::is_same_v<T, WhileLoopPayload>)
            {
                return compiler.whileLoop(input(1), output(0), output(1));
            }
            else if constexpr (std::is_same_v<T, SequencePayload>)
            {
                const auto count = payload.get<T>()->additional_outputs + 1U;
                std::vector<graph::PinSemanticId> legs;
                legs.reserve(count);
                for (std::size_t i = 0; i != count; ++i)
                {
                    legs.push_back(output(i));
                }
                return compiler.sequence(legs);
            }
            else if constexpr (std::is_same_v<T, ReturnPayload>)
            {
                return compiler.returnValues({});
            }
            else
            {
                static_assert(std::is_same_v<T, BreakPayload>);
                return compiler.breakLoop();
            }
        }

        template <class T> FlowNodeRegistration registration(std::string name, const object::CodeLease& code) noexcept
        {
            FlowNodeRegistration result;
            result.identity = {graph::nodeTypeId(name), std::move(name), 1};
            result.payload_type = cxx::typeToken<T>();
            result.code = code;
            result.create = [](const object::CodeLease& lease) noexcept
            { return FlowNodePayload::make<T, cloneControl<T>>(lease); };
            result.describe_pins = [](const FlowNodePayload& payload) noexcept
            { return payload.get<T>()->describePins(); };
            result.validate = validateControl<T>;
            result.compile_execution = compileControl<T>;
            result.encode = [](const FlowNodePayload& payload) noexcept -> FlowForgeResult<std::string>
            {
                if constexpr (std::is_same_v<T, SequencePayload>)
                {
                    return std::to_string(payload.get<T>()->additional_outputs);
                }
                else
                {
                    return std::string{};
                }
            };
            result.decode = [](std::string_view bytes,
                               const object::CodeLease& lease) noexcept -> FlowForgeResult<FlowNodePayload>
            {
                T value;
                if constexpr (std::is_same_v<T, SequencePayload>)
                {
                    if (bytes.empty())
                    {
                        return cxx::unexpected(invalidControl());
                    }
                    const auto parsed =
                        std::from_chars(bytes.data(), bytes.data() + bytes.size(), value.additional_outputs);
                    const bool has_number = parsed.ec == std::errc{} && parsed.ptr == bytes.data() + bytes.size();
                    const bool is_invalid_count = !has_number || !sequenceFits(value.additional_outputs);
                    if (is_invalid_count)
                    {
                        return cxx::unexpected(invalidControl());
                    }
                }
                else if (!bytes.empty())
                {
                    return cxx::unexpected(invalidControl());
                }
                return FlowNodePayload::make<T, cloneControl<T>>(lease, value);
            };
            return result;
        }

        template <auto Value>
        FlowForgeResult<meta::RuntimeObject> initialValue(const FlowNodePayload&, const meta::RefType&) noexcept
        {
            return meta::RuntimeObject{Value};
        }

        std::vector<FlowPinDeclaration> executionInput() noexcept
        {
            std::vector<FlowPinDeclaration> pins;
            detail::appendExecutionPin(pins, EPinDirection::INPUT, "->");
            return pins;
        }

        void appendLoopOutputs(std::vector<FlowPinDeclaration>& pins) noexcept
        {
            detail::appendExecutionPin(pins, EPinDirection::OUTPUT, "loop body");
            detail::appendExecutionPin(pins, EPinDirection::OUTPUT, "Completed", 1);
        }
    } // namespace

    FlowNodeRegistration::PinResult StartPayload::describePins() const noexcept
    {
        std::vector<FlowPinDeclaration> pins;
        detail::appendExecutionPin(pins, EPinDirection::OUTPUT, "->");
        return pins;
    }

    FlowNodeRegistration::PinResult BranchPayload::describePins() const noexcept
    {
        auto pins = executionInput();
        detail::appendDataPin(pins, EPinDirection::INPUT, 1, "Condition", &meta::ref_type_of_v<bool>, true);
        pins.back().initial_value = initialValue<false>;
        detail::appendExecutionPin(pins, EPinDirection::OUTPUT, "True");
        detail::appendExecutionPin(pins, EPinDirection::OUTPUT, "False", 1);
        return pins;
    }

    FlowNodeRegistration::PinResult SequencePayload::describePins() const noexcept
    {
        // Semantic ordinals occupy the lower 56 bits; reserve zero and the primary output.
        std::vector<FlowPinDeclaration> pins;
        if (!sequenceFits(additional_outputs))
        {
            return cxx::unexpected(FlowForgeFailure{
                EFlowForgeError::INVALID_DESCRIPTION,
                "sequence output count exceeds pin schema capacity"
            });
        }
        pins.reserve(additional_outputs + 2U);
        detail::appendExecutionPin(pins, EPinDirection::INPUT, "->");
        for (std::size_t index = 0; index <= additional_outputs; ++index)
        {
            detail::appendExecutionPin(pins, EPinDirection::OUTPUT, "->", index);
        }
        return pins;
    }

    FlowNodeRegistration::PinResult ForLoopPayload::describePins() const noexcept
    {
        auto pins = executionInput();
        const auto* type = &meta::ref_type_of_v<std::int32_t>;
        detail::appendDataPin(pins, EPinDirection::INPUT, 1, "First Index", type, true);
        pins.back().initial_value = initialValue<std::int32_t{0}>;
        detail::appendDataPin(pins, EPinDirection::INPUT, 2, "Last Index", type, true);
        pins.back().initial_value = initialValue<std::int32_t{10}>;
        appendLoopOutputs(pins);
        detail::appendDataPin(pins, EPinDirection::OUTPUT, 2, "Index", type);
        return pins;
    }

    FlowNodeRegistration::PinResult WhileLoopPayload::describePins() const noexcept
    {
        auto pins = executionInput();
        detail::appendDataPin(pins, EPinDirection::INPUT, 1, "Condition", &meta::ref_type_of_v<bool>, true);
        pins.back().initial_value = initialValue<true>;
        appendLoopOutputs(pins);
        return pins;
    }

    FlowNodeRegistration::PinResult ReturnPayload::describePins() const noexcept
    {
        return executionInput();
    }

    FlowNodeRegistration::PinResult BreakPayload::describePins() const noexcept
    {
        return executionInput();
    }

    std::vector<FlowNodeRegistration> controlNodeRegistrations(object::CodeLease code) noexcept
    {
        return {
            registration<StartPayload>("lux.flow.start", code),
            registration<BranchPayload>("lux.flow.branch", code),
            registration<SequencePayload>("lux.flow.sequence", code),
            registration<ForLoopPayload>("lux.flow.for_loop", code),
            registration<WhileLoopPayload>("lux.flow.while_loop", code),
            registration<ReturnPayload>("lux.flow.return", code),
            registration<BreakPayload>("lux.flow.break", code)
        };
    }
} // namespace lux::flowforge
