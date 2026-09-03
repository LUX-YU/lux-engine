#include <lux/engine/flowforge/Compiler.hpp>
#include <lux/engine/flowforge/graph/FlowGraph.hpp>
#include <lux/engine/flowforge/graph/ControlNode.hpp>
#include <lux/engine/flowforge/graph/FunctionalNode.hpp>
#include <lux/engine/flowforge/script/ScriptAbilityNode.hpp>
#include <lux/engine/flowforge/script/ScriptEventAwaitNode.hpp>
#include <lux/engine/function/script/native/NativeModule.hpp>

#include <algorithm>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

namespace
{
    static constexpr auto kI32 = lux::script::makeScriptAbilityValue<std::int32_t>(
        lux::script::EScriptAbilityValueLifetime::OWNED_VALUE
    );
    static constexpr auto kBorrowedI32 = lux::script::makeScriptAbilityValue<const std::int32_t&>(
        lux::script::EScriptAbilityValueLifetime::BORROWED_STEP
    );
    static constexpr auto kAwaitI32 = lux::script::makeScriptAbilityValue<std::int32_t>(
        lux::script::EScriptAbilityValueLifetime::AWAITABLE
    );
    static constexpr std::array kOneI32Parameter{
        lux::script::ScriptAbilityParameterDescription{"value", kI32}
    };
    static constexpr std::array kOneI32Result{kI32};
    static constexpr std::array kAbilityNodes{
        lux::flowforge::ScriptAbilityNodeDescription{
            lux::script::ScriptApiContractIdView{"lux.test.flowforge.value"},
            lux::script::ScriptApiMethodIdView{"lux.test.flowforge.value.read"},
            "Value",
            "Read",
            1U,
            0x55112233ULL,
            lux::script::EScriptAbilityReceiverKind::PROVIDER_INSTANCE,
            lux::script::EScriptApiMethodKind::QUERY,
            kOneI32Parameter,
            kOneI32Result
        },
        lux::flowforge::ScriptAbilityNodeDescription{
            lux::script::ScriptApiContractIdView{"lux.test.flowforge.value"},
            lux::script::ScriptApiMethodIdView{"lux.test.flowforge.value.write"},
            "Value",
            "Write",
            1U,
            0x55112233ULL,
            lux::script::EScriptAbilityReceiverKind::PROVIDER_INSTANCE,
            lux::script::EScriptApiMethodKind::COMMAND,
            kOneI32Parameter,
            {}
        },
        lux::flowforge::ScriptAbilityNodeDescription{
            lux::script::ScriptApiContractIdView{"lux.test.flowforge.value"},
            lux::script::ScriptApiMethodIdView{"lux.test.flowforge.value.next"},
            "Value",
            "Next",
            1U,
            0x55112233ULL,
            lux::script::EScriptAbilityReceiverKind::PROVIDER_INSTANCE,
            lux::script::EScriptApiMethodKind::ASYNC_OPERATION,
            {},
            {}
        },
        lux::flowforge::ScriptAbilityNodeDescription{
            lux::script::ScriptApiContractIdView{"lux.test.flowforge.value"},
            lux::script::ScriptApiMethodIdView{"lux.test.flowforge.value.borrow"},
            "Value",
            "Borrow",
            1U,
            0x55112233ULL,
            lux::script::EScriptAbilityReceiverKind::PROVIDER_INSTANCE,
            lux::script::EScriptApiMethodKind::QUERY,
            {},
            std::span{&kBorrowedI32, 1U}
        },
        lux::flowforge::ScriptAbilityNodeDescription{
            lux::script::ScriptApiContractIdView{"lux.test.flowforge.value"},
            lux::script::ScriptApiMethodIdView{"lux.test.flowforge.value.async_value"},
            "Value",
            "Async Value",
            1U,
            0x55112233ULL,
            lux::script::EScriptAbilityReceiverKind::PROVIDER_INSTANCE,
            lux::script::EScriptApiMethodKind::ASYNC_OPERATION,
            {},
            std::span{&kAwaitI32, 1U}
        }
    };
    const lux::script::ScriptEventSourceDescription kEventSource{
        "Gameplay",
        "damage",
        0x5101U,
        0x5102U,
        lux::script::EScriptEventRoute::SIMULATION_BROADCAST,
        {
            "lux.i32",
            lux::semantic::typeId("lux.i32"),
            LUX_SCRIPT_VK_INT32,
            sizeof(std::int32_t),
            alignof(std::int32_t)
        },
        lux::semantic::typeId("lux.i32"),
        1U
    };

    struct AsyncHost final
    {
        std::uint32_t starts{};
        std::uint32_t event_starts{};
        std::uint32_t expected_ordinal{};
        std::int32_t failure_status{};

        static int start(
            void* opaque,
            std::uint32_t ordinal,
            const lux_script_value_slot*,
            std::uint32_t argument_count,
            lux_script_async_token* waiting
        ) noexcept
        {
            auto& self = *static_cast<AsyncHost*>(opaque);
            if (self.failure_status != 0)
                return self.failure_status;
            if (ordinal != self.expected_ordinal || argument_count != 0U || waiting == nullptr)
                return 7;
            ++self.starts;
            *waiting = {self.starts, 1U};
            return 0;
        }

        static int waitEvent(
            void* opaque,
            std::uint32_t ordinal,
            lux_script_async_token* waiting
        ) noexcept
        {
            auto& self = *static_cast<AsyncHost*>(opaque);
            if (ordinal != 0U || waiting == nullptr)
                return 8;
            ++self.event_starts;
            *waiting = {self.event_starts, 2U};
            return 0;
        }
    };

    struct AbilityProvider final
    {
        std::int32_t value{};
        std::size_t calls{};

        static int invoke(
            void* opaque,
            std::uint32_t ordinal,
            const lux_script_value_slot* arguments,
            std::uint32_t argument_count,
            lux_script_value_slot* results,
            std::uint32_t result_count
        ) noexcept
        {
            auto& self = *static_cast<AbilityProvider*>(opaque);
            if (argument_count != 1U || arguments == nullptr || arguments[0].data == nullptr)
                return 1;
            ++self.calls;
            if (ordinal == 0U && result_count == 1U)
            {
                if (result_count != 1U || results == nullptr || results[0].data == nullptr)
                    return 2;
                *static_cast<std::int32_t*>(results[0].data) =
                    self.value + *static_cast<const std::int32_t*>(arguments[0].data);
                return 0;
            }
            if ((ordinal == 1U || ordinal == 0U) && result_count == 0U)
            {
                self.value = *static_cast<const std::int32_t*>(arguments[0].data);
                return 0;
            }
            return 3;
        }
    };

    struct BorrowProvider final
    {
        std::int32_t value{47};
        std::int32_t observed{};
        std::size_t calls{};

        static int invoke(
            void* opaque,
            std::uint32_t ordinal,
            const lux_script_value_slot* arguments,
            std::uint32_t argument_count,
            lux_script_value_slot* results,
            std::uint32_t result_count
        ) noexcept
        {
            auto& self = *static_cast<BorrowProvider*>(opaque);
            ++self.calls;
            if (ordinal == 0U && argument_count == 0U && result_count == 1U && results != nullptr &&
                results[0].data != nullptr)
            {
                *static_cast<std::int32_t*>(results[0].data) = self.value;
                return 0;
            }
            if (ordinal == 2U && argument_count == 1U && arguments != nullptr && arguments[0].data != nullptr &&
                result_count == 0U)
            {
                self.observed = *static_cast<const std::int32_t*>(arguments[0].data);
                return 0;
            }
            return 4;
        }
    };

    lux::flowforge::FlowGraph makeGraph(
        std::string_view display_name,
        lux::script::ScriptSymbolId symbol
    )
    {
        lux::flowforge::FlowGraph graph;
        const auto node_index = graph.addNodes(std::make_unique<lux::flowforge::OnEventNode>(display_name));
        const auto node_id = graph.getNode(node_index).node->id();
        const bool added = graph.addExport(lux::flowforge::ExportMethodNode{
            lux::flowforge::FlowForgeExportNodeId{1U},
            node_id,
            symbol
        });
        assert(added);
        return graph;
    }

    lux::flowforge::FlowGraph makeAbilityGraph(lux::script::ScriptSymbolId symbol)
    {
        lux::flowforge::FlowGraph graph;
        auto event = std::make_unique<lux::flowforge::OnEventNode>("ability_tick");
        auto write = std::make_unique<lux::flowforge::ScriptAbilityNode>(kAbilityNodes[1]);
        auto read = std::make_unique<lux::flowforge::ScriptAbilityNode>(kAbilityNodes[0]);
        assert(write->parameterPins().front()->setConstantData(lux::meta::RuntimeObject(std::int32_t{41})));
        assert(read->parameterPins().front()->setConstantData(lux::meta::RuntimeObject(std::int32_t{1})));
        auto* event_ptr = event.get();
        auto* write_ptr = write.get();
        auto* read_ptr = read.get();
        const auto event_index = graph.addNodes(std::move(event));
        graph.addNodes(std::move(write));
        graph.addNodes(std::move(read));
        lux::flowforge::LastLink previous;
        assert(
            event_ptr->execOutPin().linkTo(&write_ptr->execInPin(), previous) == lux::flowforge::ELinkError::SUCCESS
        );
        assert(write_ptr->execOutPin().linkTo(&read_ptr->execInPin(), previous) == lux::flowforge::ELinkError::SUCCESS);
        assert(graph.addExport({
            lux::flowforge::FlowForgeExportNodeId{2U},
            graph.getNode(event_index).node->id(),
            symbol
        }));
        return graph;
    }

    lux::flowforge::FlowGraph makeAsyncAbilityGraph(lux::script::ScriptSymbolId symbol)
    {
        lux::flowforge::FlowGraph graph;
        auto event = std::make_unique<lux::flowforge::OnEventNode>("async_tick");
        auto first = std::make_unique<lux::flowforge::ScriptAbilityNode>(kAbilityNodes[2]);
        auto second = std::make_unique<lux::flowforge::ScriptAbilityNode>(kAbilityNodes[2]);
        auto write = std::make_unique<lux::flowforge::ScriptAbilityNode>(kAbilityNodes[1]);
        assert(write->parameterPins().front()->setConstantData(lux::meta::RuntimeObject(std::int32_t{77})));
        auto* event_ptr = event.get();
        auto* first_ptr = first.get();
        auto* second_ptr = second.get();
        auto* write_ptr = write.get();
        const auto event_index = graph.addNodes(std::move(event));
        graph.addNodes(std::move(first));
        graph.addNodes(std::move(second));
        graph.addNodes(std::move(write));
        lux::flowforge::LastLink previous;
        assert(
            event_ptr->execOutPin().linkTo(&first_ptr->execInPin(), previous) == lux::flowforge::ELinkError::SUCCESS
        );
        assert(
            first_ptr->execOutPin().linkTo(&second_ptr->execInPin(), previous) == lux::flowforge::ELinkError::SUCCESS
        );
        assert(
            second_ptr->execOutPin().linkTo(&write_ptr->execInPin(), previous) == lux::flowforge::ELinkError::SUCCESS
        );
        assert(graph.addExport({
            lux::flowforge::FlowForgeExportNodeId{3U},
            graph.getNode(event_index).node->id(),
            symbol
        }));
        return graph;
    }

    lux::flowforge::FlowGraph makeBorrowedAcrossAwaitGraph(lux::script::ScriptSymbolId symbol)
    {
        lux::flowforge::FlowGraph graph;
        auto event = std::make_unique<lux::flowforge::OnEventNode>("borrowed_tick");
        auto borrow = std::make_unique<lux::flowforge::ScriptAbilityNode>(kAbilityNodes[3]);
        auto wait = std::make_unique<lux::flowforge::ScriptAbilityNode>(kAbilityNodes[2]);
        auto write = std::make_unique<lux::flowforge::ScriptAbilityNode>(kAbilityNodes[1]);
        auto* event_ptr = event.get();
        auto* borrow_ptr = borrow.get();
        auto* wait_ptr = wait.get();
        auto* write_ptr = write.get();
        const auto event_index = graph.addNodes(std::move(event));
        graph.addNodes(std::move(borrow));
        graph.addNodes(std::move(wait));
        graph.addNodes(std::move(write));
        lux::flowforge::LastLink previous;
        assert(
            event_ptr->execOutPin().linkTo(&borrow_ptr->execInPin(), previous) == lux::flowforge::ELinkError::SUCCESS
        );
        assert(
            borrow_ptr->execOutPin().linkTo(&wait_ptr->execInPin(), previous) == lux::flowforge::ELinkError::SUCCESS
        );
        assert(wait_ptr->execOutPin().linkTo(&write_ptr->execInPin(), previous) == lux::flowforge::ELinkError::SUCCESS);
        assert(borrow_ptr->resultPins().front()->linkTo(write_ptr->parameterPins().front().get(), previous) ==
            lux::flowforge::ELinkError::SUCCESS);
        assert(graph.addExport({
            lux::flowforge::FlowForgeExportNodeId{4U},
            graph.getNode(event_index).node->id(),
            symbol
        }));
        return graph;
    }

    lux::flowforge::FlowGraph makeBorrowedAcrossEventWaitGraph(lux::script::ScriptSymbolId symbol)
    {
        using namespace lux::flowforge;
        FlowGraph graph;
        auto event = std::make_unique<OnEventNode>("borrowed_event_wait");
        auto borrow = std::make_unique<ScriptAbilityNode>(kAbilityNodes[3]);
        auto wait = std::make_unique<ScriptEventAwaitNode>(kEventSource);
        auto write = std::make_unique<ScriptAbilityNode>(kAbilityNodes[1]);
        auto* event_pointer = event.get();
        auto* borrow_pointer = borrow.get();
        auto* wait_pointer = wait.get();
        auto* write_pointer = write.get();
        const auto event_index = graph.addNodes(std::move(event));
        graph.addNodes(std::move(borrow));
        graph.addNodes(std::move(wait));
        graph.addNodes(std::move(write));
        LastLink previous;
        assert(event_pointer->execOutPin().linkTo(&borrow_pointer->execInPin(), previous) == ELinkError::SUCCESS);
        assert(borrow_pointer->execOutPin().linkTo(&wait_pointer->execInPin(), previous) == ELinkError::SUCCESS);
        assert(wait_pointer->execOutPin().linkTo(&write_pointer->execInPin(), previous) == ELinkError::SUCCESS);
        assert(borrow_pointer->resultPins().front()->linkTo(
            write_pointer->parameterPins().front().get(),
            previous
        ) == ELinkError::SUCCESS);
        assert(graph.addExport({FlowForgeExportNodeId{41U}, graph.getNode(event_index).node->id(), symbol}));
        return graph;
    }

    lux::flowforge::FlowGraph makeEventWaitGraph(
        lux::script::ScriptSymbolId symbol,
        const lux::script::ScriptEventSourceDescription& source = kEventSource
    )
    {
        using namespace lux::flowforge;
        FlowGraph graph;
        auto event = std::make_unique<OnEventNode>("event_wait");
        auto wait = std::make_unique<ScriptEventAwaitNode>(source);
        auto write = std::make_unique<ScriptAbilityNode>(kAbilityNodes[1]);
        auto* event_pointer = event.get();
        auto* wait_pointer = wait.get();
        auto* write_pointer = write.get();
        const auto event_index = graph.addNodes(std::move(event));
        graph.addNodes(std::move(wait));
        graph.addNodes(std::move(write));
        LastLink previous;
        assert(event_pointer->execOutPin().linkTo(&wait_pointer->execInPin(), previous) == ELinkError::SUCCESS);
        assert(wait_pointer->execOutPin().linkTo(&write_pointer->execInPin(), previous) == ELinkError::SUCCESS);
        assert(const_cast<DataOutPin&>(wait_pointer->payloadPin()).linkTo(
            write_pointer->parameterPins().front().get(),
            previous
        ) == ELinkError::SUCCESS);
        assert(graph.addExport({FlowForgeExportNodeId{42U}, graph.getNode(event_index).node->id(), symbol}));
        return graph;
    }

    lux::flowforge::FlowGraph makeBorrowedAcrossAsyncFunctionGraph(lux::script::ScriptSymbolId symbol)
    {
        using namespace lux::flowforge;
        FlowGraph graph;
        auto event = std::make_unique<OnEventNode>(800U, "borrowed_function_tick");
        auto borrow = std::make_unique<ScriptAbilityNode>(801U, kAbilityNodes[3]);
        auto function = std::make_unique<FuncDefNode>(
            802U,
            "wait_for_borrowed",
            std::vector<FuncArgInfo>{},
            std::vector<FuncArgInfo>{}
        );
        auto call = std::make_unique<GraphFuncCallNode>(803U, *function);
        auto wait = std::make_unique<ScriptAbilityNode>(804U, kAbilityNodes[2]);
        auto function_return = std::make_unique<FuncReturnNode>(805U, *function);
        auto write = std::make_unique<ScriptAbilityNode>(806U, kAbilityNodes[1]);
        auto* event_ptr = event.get();
        auto* borrow_ptr = borrow.get();
        auto* function_ptr = function.get();
        auto* call_ptr = call.get();
        auto* wait_ptr = wait.get();
        auto* return_ptr = function_return.get();
        auto* write_ptr = write.get();
        const auto event_index = graph.addNodes(std::move(event));
        graph.addNodes(std::move(borrow));
        graph.addNodes(std::move(function));
        graph.addNodes(std::move(call));
        graph.addNodes(std::move(wait));
        graph.addNodes(std::move(function_return));
        graph.addNodes(std::move(write));
        LastLink previous;
        assert(event_ptr->execOutPin().linkTo(&borrow_ptr->execInPin(), previous) == ELinkError::SUCCESS);
        assert(borrow_ptr->execOutPin().linkTo(&call_ptr->execInPin(), previous) == ELinkError::SUCCESS);
        assert(call_ptr->execOutPin().linkTo(&write_ptr->execInPin(), previous) == ELinkError::SUCCESS);
        assert(function_ptr->execOutPin().linkTo(&wait_ptr->execInPin(), previous) == ELinkError::SUCCESS);
        assert(wait_ptr->execOutPin().linkTo(&return_ptr->execInPin(), previous) == ELinkError::SUCCESS);
        assert(borrow_ptr->resultPins().front()->linkTo(write_ptr->parameterPins().front().get(), previous) ==
            ELinkError::SUCCESS);
        assert(graph.addExport({FlowForgeExportNodeId{8U}, graph.getNode(event_index).node->id(), symbol}));
        return graph;
    }

    lux::flowforge::FlowGraph makeBorrowedFanInGraph(
        lux::script::ScriptSymbolId symbol,
        bool delayed_path_first
    )
    {
        using namespace lux::flowforge;
        FlowGraph graph;
        auto event = std::make_unique<OnEventNode>(900U, "borrowed_fan_in_tick");
        auto borrow = std::make_unique<ScriptAbilityNode>(901U, kAbilityNodes[3]);
        auto branch = std::make_unique<BranchNode>(902U);
        auto wait = std::make_unique<ScriptAbilityNode>(903U, kAbilityNodes[2]);
        auto write = std::make_unique<ScriptAbilityNode>(904U, kAbilityNodes[1]);
        assert(const_cast<DataInPin&>(branch->dataInPin()).setConstantData(lux::meta::RuntimeObject(true)));
        auto* event_ptr = event.get();
        auto* borrow_ptr = borrow.get();
        auto* branch_ptr = branch.get();
        auto* wait_ptr = wait.get();
        auto* write_ptr = write.get();
        const auto event_index = graph.addNodes(std::move(event));
        graph.addNodes(std::move(borrow));
        graph.addNodes(std::move(branch));
        graph.addNodes(std::move(wait));
        graph.addNodes(std::move(write));
        LastLink previous;
        assert(event_ptr->execOutPin().linkTo(&borrow_ptr->execInPin(), previous) == ELinkError::SUCCESS);
        assert(borrow_ptr->execOutPin().linkTo(&branch_ptr->execInPin(), previous) == ELinkError::SUCCESS);
        assert(const_cast<ExecOutPin&>(branch_ptr->execOutPinDown()).linkTo(&wait_ptr->execInPin(), previous) ==
            ELinkError::SUCCESS);
        const auto link_direct = [&] {
            return const_cast<ExecOutPin&>(branch_ptr->execOutPinUp()).linkTo(&write_ptr->execInPin(), previous);
        };
        const auto link_delayed = [&] {
            return wait_ptr->execOutPin().linkTo(&write_ptr->execInPin(), previous);
        };
        if (delayed_path_first)
        {
            assert(link_delayed() == ELinkError::SUCCESS);
            assert(link_direct() == ELinkError::SUCCESS);
        }
        else
        {
            assert(link_direct() == ELinkError::SUCCESS);
            assert(link_delayed() == ELinkError::SUCCESS);
        }
        assert(borrow_ptr->resultPins().front()->linkTo(write_ptr->parameterPins().front().get(), previous) ==
            ELinkError::SUCCESS);
        assert(graph.addExport({FlowForgeExportNodeId{9U}, graph.getNode(event_index).node->id(), symbol}));
        return graph;
    }

    lux::flowforge::FlowGraph makeBorrowedSameStepGraph(lux::script::ScriptSymbolId symbol)
    {
        using namespace lux::flowforge;
        FlowGraph graph;
        auto event = std::make_unique<OnEventNode>(1000U, "borrowed_same_step_tick");
        auto borrow = std::make_unique<ScriptAbilityNode>(1001U, kAbilityNodes[3]);
        auto write = std::make_unique<ScriptAbilityNode>(1002U, kAbilityNodes[1]);
        auto wait = std::make_unique<ScriptAbilityNode>(1003U, kAbilityNodes[2]);
        auto* event_ptr = event.get();
        auto* borrow_ptr = borrow.get();
        auto* write_ptr = write.get();
        auto* wait_ptr = wait.get();
        const auto event_index = graph.addNodes(std::move(event));
        graph.addNodes(std::move(borrow));
        graph.addNodes(std::move(write));
        graph.addNodes(std::move(wait));
        LastLink previous;
        assert(event_ptr->execOutPin().linkTo(&borrow_ptr->execInPin(), previous) == ELinkError::SUCCESS);
        assert(borrow_ptr->execOutPin().linkTo(&write_ptr->execInPin(), previous) == ELinkError::SUCCESS);
        assert(write_ptr->execOutPin().linkTo(&wait_ptr->execInPin(), previous) == ELinkError::SUCCESS);
        assert(borrow_ptr->resultPins().front()->linkTo(write_ptr->parameterPins().front().get(), previous) ==
            ELinkError::SUCCESS);
        assert(graph.addExport({FlowForgeExportNodeId{10U}, graph.getNode(event_index).node->id(), symbol}));
        return graph;
    }

    lux::flowforge::FlowGraph makeControlAsyncGraph(lux::script::ScriptSymbolId symbol)
    {
        using namespace lux::flowforge;
        FlowGraph graph;
        auto event = std::make_unique<OnEventNode>("control_async_tick");
        auto before = std::make_unique<BranchNode>();
        auto loop = std::make_unique<ForLoopNode>();
        auto loop_wait = std::make_unique<ScriptAbilityNode>(kAbilityNodes[2]);
        auto final_wait = std::make_unique<ScriptAbilityNode>(kAbilityNodes[2]);
        auto write = std::make_unique<ScriptAbilityNode>(kAbilityNodes[1]);
        assert(const_cast<DataInPin&>(before->dataInPin()).setConstantData(lux::meta::RuntimeObject(true)));
        assert(const_cast<DataInPin&>(loop->first_index()).setConstantData(lux::meta::RuntimeObject(std::int32_t{0})));
        assert(const_cast<DataInPin&>(loop->lastIndex()).setConstantData(lux::meta::RuntimeObject(std::int32_t{2})));
        assert(write->parameterPins().front()->setConstantData(lux::meta::RuntimeObject(std::int32_t{99})));
        auto* event_ptr = event.get();
        auto* before_ptr = before.get();
        auto* loop_ptr = loop.get();
        auto* loop_wait_ptr = loop_wait.get();
        auto* final_wait_ptr = final_wait.get();
        auto* write_ptr = write.get();
        const auto event_index = graph.addNodes(std::move(event));
        graph.addNodes(std::move(before));
        graph.addNodes(std::move(loop));
        graph.addNodes(std::move(loop_wait));
        graph.addNodes(std::move(final_wait));
        graph.addNodes(std::move(write));
        LastLink previous;
        assert(event_ptr->execOutPin().linkTo(&before_ptr->execInPin(), previous) == ELinkError::SUCCESS);
        assert(const_cast<ExecOutPin&>(before_ptr->execOutPinUp()).linkTo(&loop_ptr->execInPin(), previous) ==
            ELinkError::SUCCESS);
        assert(const_cast<ExecOutPin&>(loop_ptr->loopBody()).linkTo(&loop_wait_ptr->execInPin(), previous) ==
            ELinkError::SUCCESS);
        assert(const_cast<ExecOutPin&>(loop_ptr->completed()).linkTo(&final_wait_ptr->execInPin(), previous) ==
            ELinkError::SUCCESS);
        assert(final_wait_ptr->execOutPin().linkTo(&write_ptr->execInPin(), previous) == ELinkError::SUCCESS);
        assert(graph.addExport({
            FlowForgeExportNodeId{5U},
            graph.getNode(event_index).node->id(),
            symbol
        }));
        return graph;
    }

    lux::flowforge::FlowGraph makeAsyncResultGraph(lux::script::ScriptSymbolId symbol)
    {
        using namespace lux::flowforge;
        FlowGraph graph;
        auto event = std::make_unique<OnEventNode>("async_result_tick");
        auto value = std::make_unique<ScriptAbilityNode>(kAbilityNodes[4]);
        auto write = std::make_unique<ScriptAbilityNode>(kAbilityNodes[1]);
        auto* event_ptr = event.get();
        auto* value_ptr = value.get();
        auto* write_ptr = write.get();
        const auto event_index = graph.addNodes(std::move(event));
        graph.addNodes(std::move(value));
        graph.addNodes(std::move(write));
        LastLink previous;
        assert(event_ptr->execOutPin().linkTo(&value_ptr->execInPin(), previous) == ELinkError::SUCCESS);
        assert(value_ptr->execOutPin().linkTo(&write_ptr->execInPin(), previous) == ELinkError::SUCCESS);
        assert(value_ptr->resultPins().front()->linkTo(write_ptr->parameterPins().front().get(), previous) ==
            ELinkError::SUCCESS);
        assert(graph.addExport({
            FlowForgeExportNodeId{6U},
            graph.getNode(event_index).node->id(),
            symbol
        }));
        return graph;
    }

    lux::flowforge::FlowGraph makeAsyncGraphFunctionGraph(lux::script::ScriptSymbolId symbol)
    {
        using namespace lux::flowforge;
        FlowGraph graph;
        auto event = std::make_unique<OnEventNode>("async_function_tick");
        auto function = std::make_unique<FuncDefNode>(
            "wait_once",
            std::vector<FuncArgInfo>{},
            std::vector<FuncArgInfo>{}
        );
        auto call = std::make_unique<GraphFuncCallNode>(*function);
        auto wait = std::make_unique<ScriptAbilityNode>(kAbilityNodes[2]);
        auto function_return = std::make_unique<FuncReturnNode>(*function);
        auto write = std::make_unique<ScriptAbilityNode>(kAbilityNodes[1]);
        assert(write->parameterPins().front()->setConstantData(lux::meta::RuntimeObject(std::int32_t{55})));
        auto* event_ptr = event.get();
        auto* function_ptr = function.get();
        auto* call_ptr = call.get();
        auto* wait_ptr = wait.get();
        auto* return_ptr = function_return.get();
        auto* write_ptr = write.get();
        const auto event_index = graph.addNodes(std::move(event));
        graph.addNodes(std::move(function));
        graph.addNodes(std::move(call));
        graph.addNodes(std::move(wait));
        graph.addNodes(std::move(function_return));
        graph.addNodes(std::move(write));
        LastLink previous;
        assert(event_ptr->execOutPin().linkTo(&call_ptr->execInPin(), previous) == ELinkError::SUCCESS);
        assert(call_ptr->execOutPin().linkTo(&write_ptr->execInPin(), previous) == ELinkError::SUCCESS);
        assert(function_ptr->execOutPin().linkTo(&wait_ptr->execInPin(), previous) == ELinkError::SUCCESS);
        assert(wait_ptr->execOutPin().linkTo(&return_ptr->execInPin(), previous) == ELinkError::SUCCESS);
        assert(graph.addExport({
            FlowForgeExportNodeId{7U},
            graph.getNode(event_index).node->id(),
            symbol
        }));
        return graph;
    }

    lux::flowforge::FlowGraph makeEventWaitFunctionGraph(lux::script::ScriptSymbolId symbol)
    {
        using namespace lux::flowforge;
        FlowGraph graph;
        auto event = std::make_unique<OnEventNode>("event_wait_function");
        auto function = std::make_unique<FuncDefNode>(
            "wait_event_once",
            std::vector<FuncArgInfo>{},
            std::vector<FuncArgInfo>{}
        );
        auto call = std::make_unique<GraphFuncCallNode>(*function);
        auto wait = std::make_unique<ScriptEventAwaitNode>(kEventSource);
        auto function_return = std::make_unique<FuncReturnNode>(*function);
        auto write = std::make_unique<ScriptAbilityNode>(kAbilityNodes[1]);
        assert(write->parameterPins().front()->setConstantData(lux::meta::RuntimeObject(std::int32_t{61})));
        auto* event_pointer = event.get();
        auto* function_pointer = function.get();
        auto* call_pointer = call.get();
        auto* wait_pointer = wait.get();
        auto* return_pointer = function_return.get();
        auto* write_pointer = write.get();
        const auto event_index = graph.addNodes(std::move(event));
        graph.addNodes(std::move(function));
        graph.addNodes(std::move(call));
        graph.addNodes(std::move(wait));
        graph.addNodes(std::move(function_return));
        graph.addNodes(std::move(write));
        LastLink previous;
        assert(event_pointer->execOutPin().linkTo(&call_pointer->execInPin(), previous) == ELinkError::SUCCESS);
        assert(call_pointer->execOutPin().linkTo(&write_pointer->execInPin(), previous) == ELinkError::SUCCESS);
        assert(function_pointer->execOutPin().linkTo(&wait_pointer->execInPin(), previous) == ELinkError::SUCCESS);
        assert(wait_pointer->execOutPin().linkTo(&return_pointer->execInPin(), previous) == ELinkError::SUCCESS);
        assert(graph.addExport({FlowForgeExportNodeId{43U}, graph.getNode(event_index).node->id(), symbol}));
        return graph;
    }
}

int main()
{
    constexpr lux::script::ScriptSymbolId Symbol = 0x1234U;
    auto graph = makeGraph("tick", Symbol);
    auto compiled = lux::flowforge::compileFlowForgeScript(
        graph,
        lux::flowforge::FlowForgeCompileOptions{
            .module_name = "gameplay.behavior",
            .lifecycle = {.begin_play = Symbol}
        }
    );
    if (!compiled)
        std::fprintf(stderr, "FlowForge compile failed: %u %s\n", static_cast<unsigned>(compiled.error().code),
                     compiled.error().message.c_str());
    assert(compiled);
    assert(!compiled->payload().empty());
    assert(compiled->findExport(Symbol) == &compiled->description().exports.front());
    assert(compiled->description().lifecycle.begin_play == Symbol);

    auto loaded = lux::script::loadNativeModule(compiled->payload(), "gameplay.behavior");
    assert(loaded);
    assert(loaded->findFunction(Symbol) != nullptr);

    lux_script_call_frame frame{};
    const lux_script_native_instance_context native_instance{};
    frame.native_instance = std::addressof(native_instance);
    assert(loaded->findFunction(Symbol)->invoke(&frame) == 0);

    constexpr lux::script::ScriptSymbolId AbilitySymbol = 0x2234U;
    auto ability_graph = makeAbilityGraph(AbilitySymbol);
    lux::flowforge::ScriptAbilityNodeCatalog ability_catalog;
    assert(ability_catalog.add({kAbilityNodes}));
    auto ability_artifact = lux::flowforge::compileFlowForgeScript(
        ability_graph,
        lux::flowforge::FlowForgeCompileOptions{
            .module_name = "gameplay.ability",
            .script_abilities = ability_catalog.view()
        }
    );
    if (!ability_artifact)
    {
        std::fprintf(
            stderr,
            "FlowForge Ability compile failed: %u %s\n",
            static_cast<unsigned>(ability_artifact.error().code),
            ability_artifact.error().message.c_str()
        );
    }
    assert(ability_artifact);
    assert(ability_artifact->description().api_requirements.size() == 1U);
    assert(ability_artifact->description().api_requirements.front().contract.name() == "lux.test.flowforge.value");
    auto ability_module = lux::script::loadNativeModule(ability_artifact->payload(), "gameplay.ability");
    assert(ability_module);
    assert(ability_module->abilityImports().size() == 2U);
    assert(std::string_view{ability_module->abilityImports()[0].method_name} == "lux.test.flowforge.value.read");
    assert(std::string_view{ability_module->abilityImports()[1].method_name} == "lux.test.flowforge.value.write");
    AbilityProvider ability_provider;
    const lux_script_ability_runtime ability_runtime{std::addressof(ability_provider), &AbilityProvider::invoke};
    const lux_script_native_instance_context ability_instance{nullptr, std::addressof(ability_runtime)};
    lux_script_call_frame ability_frame{};
    ability_frame.native_instance = std::addressof(ability_instance);
    assert(ability_module->findFunction(AbilitySymbol)->invoke(std::addressof(ability_frame)) == 0);
    assert(ability_provider.value == 41);
    assert(ability_provider.calls == 2U);

    constexpr lux::script::ScriptSymbolId EventWaitSymbol = 0x2A34U;
    auto event_wait_graph = makeEventWaitGraph(EventWaitSymbol);
    const auto missing_event = lux::flowforge::compileFlowForgeScript(
        event_wait_graph,
        lux::flowforge::FlowForgeCompileOptions{
            .module_name = "gameplay.event_wait_missing",
            .script_abilities = ability_catalog.view()
        }
    );
    assert(!missing_event &&
        missing_event.error().code == lux::flowforge::EFlowForgeError::UNKNOWN_SCRIPT_EVENT_SOURCE);
    auto mismatched_event_source = kEventSource;
    ++mismatched_event_source.payload_schema_version;
    const auto mismatched_event = lux::flowforge::compileFlowForgeScript(
        event_wait_graph,
        lux::flowforge::FlowForgeCompileOptions{
            .module_name = "gameplay.event_wait_mismatch",
            .script_abilities = ability_catalog.view(),
            .script_events = std::span{&mismatched_event_source, 1U}
        }
    );
    assert(!mismatched_event &&
        mismatched_event.error().code == lux::flowforge::EFlowForgeError::SCRIPT_EVENT_SCHEMA_MISMATCH);
    auto event_wait_artifact = lux::flowforge::compileFlowForgeScript(
        event_wait_graph,
        lux::flowforge::FlowForgeCompileOptions{
            .module_name = "gameplay.event_wait",
            .script_abilities = ability_catalog.view(),
            .script_events = std::span{&kEventSource, 1U}
        }
    );
    assert(event_wait_artifact);
    auto event_wait_module = lux::script::loadNativeModule(event_wait_artifact->payload(), "gameplay.event_wait");
    assert(event_wait_module && event_wait_module->eventWaitImports().size() == 1U);
    auto targeted_source = kEventSource;
    targeted_source.event_name = "targeted";
    ++targeted_source.event_id;
    targeted_source.route = lux::script::EScriptEventRoute::ENTITY_TARGETED;
    auto targeted_graph = makeEventWaitGraph(0x2C34U, targeted_source);
    auto targeted_artifact = lux::flowforge::compileFlowForgeScript(
        targeted_graph,
        lux::flowforge::FlowForgeCompileOptions{
            .module_name = "gameplay.targeted_event_wait",
            .script_abilities = ability_catalog.view(),
            .script_events = std::span{&targeted_source, 1U}
        }
    );
    assert(targeted_artifact);
    auto targeted_module = lux::script::loadNativeModule(
        targeted_artifact->payload(),
        "gameplay.targeted_event_wait"
    );
    assert(targeted_module && targeted_module->eventWaitImports().size() == 1U);
    assert(targeted_module->eventWaitImports().front().route == 1U);
    const auto* event_wait_function = event_wait_module->findFunction(EventWaitSymbol);
    assert(event_wait_function != nullptr && event_wait_function->step != nullptr);
    AbilityProvider event_wait_provider;
    const lux_script_ability_runtime event_wait_runtime{
        std::addressof(event_wait_provider),
        &AbilityProvider::invoke
    };
    const lux_script_native_instance_context event_wait_instance{nullptr, std::addressof(event_wait_runtime)};
    lux_script_call_frame event_wait_frame{};
    event_wait_frame.native_instance = std::addressof(event_wait_instance);
    AsyncHost event_wait_host;
    const lux_script_step_host event_wait_step_host{
        std::addressof(event_wait_host),
        &AsyncHost::start,
        &AsyncHost::waitEvent
    };
    const auto& event_wait_step = *event_wait_function->step;
    void* event_wait_continuation = ::operator new(event_wait_step.frame_size);
    std::memset(event_wait_continuation, 0, event_wait_step.frame_size);
    lux_script_step_outcome event_wait_outcome{};
    assert(event_wait_step.start(
        &event_wait_frame,
        &event_wait_step_host,
        event_wait_continuation,
        &event_wait_outcome
    ) == 0);
    assert(event_wait_outcome.state == LUX_SCRIPT_STEP_SUSPENDED && event_wait_host.event_starts == 1U);
    std::int32_t event_value{37};
    const lux_script_step_resume_packet event_ready{
        LUX_SCRIPT_RESUME_READY,
        1U,
        {},
        {
            LUX_SCRIPT_VK_INT32,
            {},
            sizeof(event_value),
            lux::semantic::typeId("lux.i32"),
            std::addressof(event_value)
        },
        0
    };
    assert(event_wait_step.resume(
        &event_wait_step_host,
        event_wait_continuation,
        &event_ready,
        &event_wait_outcome
    ) == 0);
    assert(event_wait_outcome.state == LUX_SCRIPT_STEP_COMPLETED && event_wait_provider.value == event_value);
    event_wait_step.destroy(event_wait_continuation);
    ::operator delete(event_wait_continuation);

    auto borrowed_event_graph = makeBorrowedAcrossEventWaitGraph(0x2B34U);
    const auto borrowed_event = lux::flowforge::compileFlowForgeScript(
        borrowed_event_graph,
        lux::flowforge::FlowForgeCompileOptions{
            .module_name = "gameplay.borrowed_event_wait_invalid",
            .script_abilities = ability_catalog.view(),
            .script_events = std::span{&kEventSource, 1U}
        }
    );
    assert(!borrowed_event &&
        borrowed_event.error().code == lux::flowforge::EFlowForgeError::BORROWED_VALUE_CROSSES_SUSPENSION);
    const auto lifecycle_event = lux::flowforge::compileFlowForgeScript(
        event_wait_graph,
        lux::flowforge::FlowForgeCompileOptions{
            .module_name = "gameplay.event_wait_lifecycle_invalid",
            .lifecycle = {.begin_play = EventWaitSymbol},
            .script_abilities = ability_catalog.view(),
            .script_events = std::span{&kEventSource, 1U}
        }
    );
    assert(!lifecycle_event &&
        lifecycle_event.error().code == lux::flowforge::EFlowForgeError::ASYNC_LIFECYCLE_NOT_SUPPORTED);
    constexpr lux::script::ScriptSymbolId EventFunctionSymbol = 0x2D34U;
    auto event_function_graph = makeEventWaitFunctionGraph(EventFunctionSymbol);
    const auto lifecycle_event_function = lux::flowforge::compileFlowForgeScript(
        event_function_graph,
        lux::flowforge::FlowForgeCompileOptions{
            .module_name = "gameplay.event_wait_function_lifecycle_invalid",
            .lifecycle = {.end_play = EventFunctionSymbol},
            .script_abilities = ability_catalog.view(),
            .script_events = std::span{&kEventSource, 1U}
        }
    );
    assert(!lifecycle_event_function &&
        lifecycle_event_function.error().code == lux::flowforge::EFlowForgeError::ASYNC_LIFECYCLE_NOT_SUPPORTED);

    constexpr lux::script::ScriptSymbolId AsyncSymbol = 0x3234U;
    auto async_graph = makeAsyncAbilityGraph(AsyncSymbol);
    auto async_artifact = lux::flowforge::compileFlowForgeScript(
        async_graph,
        lux::flowforge::FlowForgeCompileOptions{
            .module_name = "gameplay.async_ability",
            .script_abilities = ability_catalog.view()
        }
    );
    if (!async_artifact)
    {
        std::fprintf(
            stderr,
            "FlowForge async compile failed: %u %s\n",
            static_cast<unsigned>(async_artifact.error().code),
            async_artifact.error().message.c_str()
        );
    }
    assert(async_artifact);
    auto async_artifact_again = lux::flowforge::compileFlowForgeScript(
        async_graph,
        lux::flowforge::FlowForgeCompileOptions{
            .module_name = "gameplay.async_ability",
            .script_abilities = ability_catalog.view()
        }
    );
    assert(async_artifact_again);
    assert(std::ranges::equal(async_artifact_again->payload(), async_artifact->payload()));
    auto async_module = lux::script::loadNativeModule(async_artifact->payload(), "gameplay.async_ability");
    assert(async_module);
    const auto* async_function = async_module->findFunction(AsyncSymbol);
    assert(async_function != nullptr && async_function->step != nullptr);
    assert(async_module->abilityImports().size() == 2U);
    assert(std::string_view{async_module->abilityImports()[0].method_name} == "lux.test.flowforge.value.next");
    assert(std::string_view{async_module->abilityImports()[1].method_name} == "lux.test.flowforge.value.write");
    AbilityProvider async_provider;
    const lux_script_ability_runtime async_runtime{std::addressof(async_provider), &AbilityProvider::invoke};
    const lux_script_native_instance_context async_instance{nullptr, std::addressof(async_runtime)};
    lux_script_call_frame async_frame{};
    async_frame.native_instance = std::addressof(async_instance);
    AsyncHost async_host;
    const lux_script_step_host step_host{std::addressof(async_host), &AsyncHost::start};
    const auto& step = *async_function->step;
    const bool is_over_aligned = step.frame_align > __STDCPP_DEFAULT_NEW_ALIGNMENT__;
    void* continuation_frame = is_over_aligned
        ? ::operator new(step.frame_size, std::align_val_t{step.frame_align})
        : ::operator new(step.frame_size);
    std::memset(continuation_frame, 0, step.frame_size);
    lux_script_step_outcome outcome{};
    assert(step.start(&async_frame, &step_host, continuation_frame, &outcome) == 0);
    assert(outcome.state == LUX_SCRIPT_STEP_SUSPENDED);
    assert(outcome.waiting_on.slot == 1U);
    const lux_script_step_resume_packet ready{LUX_SCRIPT_RESUME_READY, 0U, {}, {}, 0};
    assert(step.resume(&step_host, continuation_frame, &ready, &outcome) == 0);
    assert(outcome.state == LUX_SCRIPT_STEP_SUSPENDED);
    assert(outcome.waiting_on.slot == 2U);
    assert(step.resume(&step_host, continuation_frame, &ready, &outcome) == 0);
    assert(outcome.state == LUX_SCRIPT_STEP_COMPLETED);
    assert(async_provider.value == 77);
    assert(async_host.starts == 2U);
    std::memset(continuation_frame, 0, step.frame_size);
    async_host.failure_status = 71;
    assert(step.start(&async_frame, &step_host, continuation_frame, &outcome) == 0);
    assert(outcome.state == LUX_SCRIPT_STEP_FAILED && outcome.status == 71);
    async_host.failure_status = 0;
    std::memset(continuation_frame, 0, step.frame_size);
    assert(step.start(&async_frame, &step_host, continuation_frame, &outcome) == 0);
    assert(outcome.state == LUX_SCRIPT_STEP_SUSPENDED);
    const lux_script_step_resume_packet failed_resume{LUX_SCRIPT_RESUME_FAILED, 0U, {}, {}, 72};
    assert(step.resume(&step_host, continuation_frame, &failed_resume, &outcome) == 0);
    assert(outcome.state == LUX_SCRIPT_STEP_FAILED && outcome.status == 72);
    step.destroy(continuation_frame);
    if (is_over_aligned)
        ::operator delete(continuation_frame, std::align_val_t{step.frame_align});
    else
        ::operator delete(continuation_frame);

    auto borrowed_graph = makeBorrowedAcrossAwaitGraph(0x4234U);
    const auto borrowed = lux::flowforge::compileFlowForgeScript(
        borrowed_graph,
        lux::flowforge::FlowForgeCompileOptions{
            .module_name = "gameplay.borrowed_invalid",
            .script_abilities = ability_catalog.view()
        }
    );
    assert(!borrowed);
    assert(borrowed.error().code == lux::flowforge::EFlowForgeError::BORROWED_VALUE_CROSSES_SUSPENSION);
    assert(borrowed.error().node_id != 0U);
    assert(borrowed.error().pin_id != 0U);

    auto borrowed_function_graph = makeBorrowedAcrossAsyncFunctionGraph(0x4334U);
    const auto borrowed_function = lux::flowforge::compileFlowForgeScript(
        borrowed_function_graph,
        lux::flowforge::FlowForgeCompileOptions{
            .module_name = "gameplay.borrowed_function_invalid",
            .script_abilities = ability_catalog.view()
        }
    );
    assert(!borrowed_function);
    assert(borrowed_function.error().code ==
        lux::flowforge::EFlowForgeError::BORROWED_VALUE_CROSSES_SUSPENSION);
    assert(borrowed_function.error().node_id != 0U);
    assert(borrowed_function.error().pin_id != 0U);

    for (const bool delayed_path_first : {false, true})
    {
        auto fan_in_graph = makeBorrowedFanInGraph(delayed_path_first ? 0x4434U : 0x4534U, delayed_path_first);
        const auto fan_in = lux::flowforge::compileFlowForgeScript(
            fan_in_graph,
            lux::flowforge::FlowForgeCompileOptions{
                .module_name = delayed_path_first
                    ? "gameplay.borrowed_fan_in_delayed_first"
                    : "gameplay.borrowed_fan_in_direct_first",
                .script_abilities = ability_catalog.view()
            }
        );
        assert(!fan_in);
        assert(fan_in.error().code == lux::flowforge::EFlowForgeError::BORROWED_VALUE_CROSSES_SUSPENSION);
        assert(fan_in.error().node_id != 0U);
        assert(fan_in.error().pin_id != 0U);
    }

    const auto async_lifecycle = lux::flowforge::compileFlowForgeScript(
        async_graph,
        lux::flowforge::FlowForgeCompileOptions{
            .module_name = "gameplay.async_lifecycle_invalid",
            .lifecycle = {.begin_play = AsyncSymbol},
            .script_abilities = ability_catalog.view()
        }
    );
    assert(!async_lifecycle);
    assert(async_lifecycle.error().code == lux::flowforge::EFlowForgeError::ASYNC_LIFECYCLE_NOT_SUPPORTED);

    constexpr lux::script::ScriptSymbolId ControlSymbol = 0x5234U;
    auto control_graph = makeControlAsyncGraph(ControlSymbol);
    auto control_artifact = lux::flowforge::compileFlowForgeScript(
        control_graph,
        lux::flowforge::FlowForgeCompileOptions{
            .module_name = "gameplay.control_async",
            .script_abilities = ability_catalog.view()
        }
    );
    if (!control_artifact)
    {
        std::fprintf(stderr, "FlowForge control async compile failed: %u %s\n",
                     static_cast<unsigned>(control_artifact.error().code),
                     control_artifact.error().message.c_str());
    }
    assert(control_artifact);
    auto control_module = lux::script::loadNativeModule(control_artifact->payload(), "gameplay.control_async");
    assert(control_module);
    const auto* control_function = control_module->findFunction(ControlSymbol);
    assert(control_function != nullptr && control_function->step != nullptr);
    AbilityProvider control_provider;
    const lux_script_ability_runtime control_runtime{std::addressof(control_provider), &AbilityProvider::invoke};
    const lux_script_native_instance_context control_instance{nullptr, std::addressof(control_runtime)};
    lux_script_call_frame control_frame{};
    control_frame.native_instance = std::addressof(control_instance);
    AsyncHost control_host;
    const lux_script_step_host control_step_host{std::addressof(control_host), &AsyncHost::start};
    const auto& control_step = *control_function->step;
    const bool control_over_aligned = control_step.frame_align > __STDCPP_DEFAULT_NEW_ALIGNMENT__;
    void* control_continuation = control_over_aligned
        ? ::operator new(control_step.frame_size, std::align_val_t{control_step.frame_align})
        : ::operator new(control_step.frame_size);
    std::memset(control_continuation, 0, control_step.frame_size);
    lux_script_step_outcome control_outcome{};
    assert(control_step.start(
        &control_frame,
        &control_step_host,
        control_continuation,
        &control_outcome
    ) == 0);
    std::size_t control_resumes{};
    while (control_outcome.state == LUX_SCRIPT_STEP_SUSPENDED && control_resumes < 32U)
    {
        assert(control_step.resume(
            &control_step_host,
            control_continuation,
            &ready,
            &control_outcome
        ) == 0);
        ++control_resumes;
    }
    assert(control_outcome.state == LUX_SCRIPT_STEP_COMPLETED);
    assert(control_resumes >= 3U);
    assert(control_provider.value == 99);
    control_step.destroy(control_continuation);
    if (control_over_aligned)
        ::operator delete(control_continuation, std::align_val_t{control_step.frame_align});
    else
        ::operator delete(control_continuation);

    constexpr lux::script::ScriptSymbolId AsyncResultSymbol = 0x6234U;
    auto result_graph = makeAsyncResultGraph(AsyncResultSymbol);
    auto result_artifact = lux::flowforge::compileFlowForgeScript(
        result_graph,
        lux::flowforge::FlowForgeCompileOptions{
            .module_name = "gameplay.async_result",
            .script_abilities = ability_catalog.view()
        }
    );
    assert(result_artifact);
    auto result_module = lux::script::loadNativeModule(result_artifact->payload(), "gameplay.async_result");
    assert(result_module);
    const auto* result_function = result_module->findFunction(AsyncResultSymbol);
    assert(result_function != nullptr && result_function->step != nullptr);
    AbilityProvider result_provider;
    const lux_script_ability_runtime result_runtime{std::addressof(result_provider), &AbilityProvider::invoke};
    const lux_script_native_instance_context result_instance{nullptr, std::addressof(result_runtime)};
    lux_script_call_frame result_frame{};
    result_frame.native_instance = std::addressof(result_instance);
    AsyncHost result_host;
    const lux_script_step_host result_step_host{std::addressof(result_host), &AsyncHost::start};
    const auto& result_step = *result_function->step;
    const bool result_over_aligned = result_step.frame_align > __STDCPP_DEFAULT_NEW_ALIGNMENT__;
    void* result_continuation = result_over_aligned
        ? ::operator new(result_step.frame_size, std::align_val_t{result_step.frame_align})
        : ::operator new(result_step.frame_size);
    std::memset(result_continuation, 0, result_step.frame_size);
    lux_script_step_outcome result_outcome{};
    assert(result_step.start(&result_frame, &result_step_host, result_continuation, &result_outcome) == 0);
    assert(result_outcome.state == LUX_SCRIPT_STEP_SUSPENDED);
    std::int32_t resumed_value{123};
    const lux_script_step_resume_packet value_ready{
        LUX_SCRIPT_RESUME_READY,
        1U,
        {},
        {
            LUX_SCRIPT_VK_INT32,
            {},
            sizeof(resumed_value),
            lux::semantic::typeId("lux.i32"),
            std::addressof(resumed_value)
        },
        0
    };
    assert(result_step.resume(
        &result_step_host,
        result_continuation,
        &value_ready,
        &result_outcome
    ) == 0);
    assert(result_outcome.state == LUX_SCRIPT_STEP_COMPLETED);
    assert(result_provider.value == resumed_value);
    result_step.destroy(result_continuation);
    if (result_over_aligned)
        ::operator delete(result_continuation, std::align_val_t{result_step.frame_align});
    else
        ::operator delete(result_continuation);

    constexpr lux::script::ScriptSymbolId FunctionSymbol = 0x7234U;
    auto function_graph = makeAsyncGraphFunctionGraph(FunctionSymbol);
    auto function_artifact = lux::flowforge::compileFlowForgeScript(
        function_graph,
        lux::flowforge::FlowForgeCompileOptions{
            .module_name = "gameplay.async_function",
            .script_abilities = ability_catalog.view()
        }
    );
    assert(function_artifact);
    auto function_module = lux::script::loadNativeModule(function_artifact->payload(), "gameplay.async_function");
    if (!function_module)
    {
        std::fprintf(
            stderr,
            "FlowForge async function load failed: %u %s\n",
            static_cast<unsigned>(function_module.error().code),
            function_module.error().detail.c_str()
        );
    }
    assert(function_module);
    const auto* function_export = function_module->findFunction(FunctionSymbol);
    assert(function_export != nullptr && function_export->step != nullptr);
    AbilityProvider function_provider;
    const lux_script_ability_runtime function_runtime{std::addressof(function_provider), &AbilityProvider::invoke};
    const lux_script_native_instance_context function_instance{nullptr, std::addressof(function_runtime)};
    lux_script_call_frame function_frame{};
    function_frame.native_instance = std::addressof(function_instance);
    AsyncHost function_host;
    const lux_script_step_host function_step_host{std::addressof(function_host), &AsyncHost::start};
    const auto& function_step = *function_export->step;
    const bool function_over_aligned = function_step.frame_align > __STDCPP_DEFAULT_NEW_ALIGNMENT__;
    void* function_continuation = function_over_aligned
        ? ::operator new(function_step.frame_size, std::align_val_t{function_step.frame_align})
        : ::operator new(function_step.frame_size);
    std::memset(function_continuation, 0, function_step.frame_size);
    lux_script_step_outcome function_outcome{};
    assert(function_step.start(
        &function_frame,
        &function_step_host,
        function_continuation,
        &function_outcome
    ) == 0);
    assert(function_outcome.state == LUX_SCRIPT_STEP_SUSPENDED);
    assert(function_step.resume(
        &function_step_host,
        function_continuation,
        &ready,
        &function_outcome
    ) == 0);
    assert(function_outcome.state == LUX_SCRIPT_STEP_COMPLETED);
    assert(function_provider.value == 55);
    function_step.destroy(function_continuation);
    if (function_over_aligned)
        ::operator delete(function_continuation, std::align_val_t{function_step.frame_align});
    else
        ::operator delete(function_continuation);

    for (const bool is_end_play : {false, true})
    {
        const auto lifecycle_function = lux::flowforge::compileFlowForgeScript(
            function_graph,
            lux::flowforge::FlowForgeCompileOptions{
                .module_name = is_end_play
                    ? "gameplay.async_function_end_play_invalid"
                    : "gameplay.async_function_begin_play_invalid",
                .lifecycle = is_end_play
                    ? lux::rdesc::ScriptLifecycleRoles{.end_play = FunctionSymbol}
                    : lux::rdesc::ScriptLifecycleRoles{.begin_play = FunctionSymbol},
                .script_abilities = ability_catalog.view()
            }
        );
        assert(!lifecycle_function);
        assert(lifecycle_function.error().code == lux::flowforge::EFlowForgeError::ASYNC_LIFECYCLE_NOT_SUPPORTED);
        assert(lifecycle_function.error().node_id != 0U);
    }

    constexpr lux::script::ScriptSymbolId BorrowedSameStepSymbol = 0x8234U;
    auto borrowed_same_step_graph = makeBorrowedSameStepGraph(BorrowedSameStepSymbol);
    auto borrowed_same_step_artifact = lux::flowforge::compileFlowForgeScript(
        borrowed_same_step_graph,
        lux::flowforge::FlowForgeCompileOptions{
            .module_name = "gameplay.borrowed_same_step",
            .script_abilities = ability_catalog.view()
        }
    );
    assert(borrowed_same_step_artifact);
    auto borrowed_same_step_module = lux::script::loadNativeModule(
        borrowed_same_step_artifact->payload(),
        "gameplay.borrowed_same_step"
    );
    assert(borrowed_same_step_module);
    const auto borrowed_import = std::ranges::find_if(
        borrowed_same_step_module->abilityImports(),
        [](const auto& value) {
            return std::string_view{value.method_name} == "lux.test.flowforge.value.borrow";
        }
    );
    assert(borrowed_import != borrowed_same_step_module->abilityImports().end());
    assert(borrowed_import->result_count == 1U);
    assert(borrowed_import->results[0].pass == LUX_SCRIPT_PASS_CONST_REF);
    const auto* borrowed_same_step_function =
        borrowed_same_step_module->findFunction(BorrowedSameStepSymbol);
    assert(borrowed_same_step_function != nullptr && borrowed_same_step_function->step != nullptr);
    BorrowProvider borrow_provider;
    const lux_script_ability_runtime borrow_runtime{std::addressof(borrow_provider), &BorrowProvider::invoke};
    const lux_script_native_instance_context borrow_instance{nullptr, std::addressof(borrow_runtime)};
    lux_script_call_frame borrow_frame{};
    borrow_frame.native_instance = std::addressof(borrow_instance);
    AsyncHost borrow_host;
    borrow_host.expected_ordinal = 1U;
    const lux_script_step_host borrow_step_host{std::addressof(borrow_host), &AsyncHost::start};
    const auto& borrow_step = *borrowed_same_step_function->step;
    const bool borrow_over_aligned = borrow_step.frame_align > __STDCPP_DEFAULT_NEW_ALIGNMENT__;
    void* borrow_continuation = borrow_over_aligned
        ? ::operator new(borrow_step.frame_size, std::align_val_t{borrow_step.frame_align})
        : ::operator new(borrow_step.frame_size);
    std::memset(borrow_continuation, 0, borrow_step.frame_size);
    lux_script_step_outcome borrow_outcome{};
    assert(borrow_step.start(&borrow_frame, &borrow_step_host, borrow_continuation, &borrow_outcome) == 0);
    assert(borrow_outcome.state == LUX_SCRIPT_STEP_SUSPENDED);
    assert(borrow_provider.calls == 2U);
    assert(borrow_provider.observed == borrow_provider.value);
    assert(borrow_step.resume(
        &borrow_step_host,
        borrow_continuation,
        &ready,
        &borrow_outcome
    ) == 0);
    assert(borrow_outcome.state == LUX_SCRIPT_STEP_COMPLETED);
    borrow_step.destroy(borrow_continuation);
    if (borrow_over_aligned)
        ::operator delete(borrow_continuation, std::align_val_t{borrow_step.frame_align});
    else
        ::operator delete(borrow_continuation);

    auto compiled_again = lux::flowforge::compileFlowForgeScript(
        graph,
        lux::flowforge::FlowForgeCompileOptions{
            .module_name = "gameplay.behavior",
            .lifecycle = {.begin_play = Symbol}
        }
    );
    assert(compiled_again);
    assert(compiled_again->description().exports == compiled->description().exports);
    assert(compiled_again->description().lifecycle == compiled->description().lifecycle);
    assert(std::ranges::equal(compiled_again->payload(), compiled->payload()));

    auto renamed_graph = makeGraph("renamed_tick", Symbol);
    auto renamed = lux::flowforge::compileFlowForgeScript(
        renamed_graph,
        lux::flowforge::FlowForgeCompileOptions{.module_name = "gameplay.behavior"}
    );
    assert(renamed);
    auto renamed_module = lux::script::loadNativeModule(renamed->payload(), "gameplay.behavior");
    assert(renamed_module);
    assert(renamed_module->findFunction(Symbol) != nullptr);

    auto duplicate = makeGraph("first", 1U);
    const auto duplicate_node = duplicate.addNodes(std::make_unique<lux::flowforge::OnEventNode>("second"));
    const auto duplicate_node_id = duplicate.getNode(duplicate_node).node->id();
    assert(duplicate.addExport(lux::flowforge::ExportMethodNode{
        lux::flowforge::FlowForgeExportNodeId{1U},
        duplicate_node_id,
        2U
    }));
    assert(!lux::flowforge::validFlowForgeExports(duplicate));

    lux::flowforge::FlowGraph dangling;
    assert(dangling.addExport(lux::flowforge::ExportMethodNode{
        lux::flowforge::FlowForgeExportNodeId{1U},
        42U,
        1U
    }));
    assert(!lux::flowforge::validFlowForgeExports(dangling));

    lux::flowforge::FlowGraph wrong_entry;
    auto function = std::make_unique<lux::flowforge::FuncDefNode>(
        "helper",
        std::vector<lux::flowforge::FuncArgInfo>{}
    );
    const auto function_node = wrong_entry.addNodes(std::move(function));
    const auto function_node_id = wrong_entry.getNode(function_node).node->id();
    assert(wrong_entry.addExport(lux::flowforge::ExportMethodNode{
        lux::flowforge::FlowForgeExportNodeId{1U},
        function_node_id,
        1U
    }));
    assert(!lux::flowforge::validFlowForgeExports(wrong_entry));
    return 0;
}
