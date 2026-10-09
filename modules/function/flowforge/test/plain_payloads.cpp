#include <lux/engine/flowforge/FunctionNodes.hpp>
#include <lux/engine/flowforge/NativeCallDefinition.hpp>
#include <lux/engine/flowforge/graph/FlowNode.hpp>
#include <lux/engine/flowforge/script/ScriptEventPayload.hpp>

#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <type_traits>

namespace
{
    using namespace lux;
    using namespace lux::flowforge;

    void require(bool value, std::source_location at = std::source_location::current()) noexcept
    {
        if (!value)
        {
            std::fprintf(stderr, "plain payload contract failed at %u\n", at.line());
            std::exit(42);
        }
    }

    FlowForgeResult<std::unique_ptr<FunctionPayload>> cloneFunction(const FunctionPayload& value) noexcept
    {
        return std::make_unique<FunctionPayload>(value);
    }

    void functions() noexcept
    {
        static_assert(!std::is_polymorphic_v<FunctionPayload>);
        static_assert(!std::is_polymorphic_v<FunctionCallPayload>);
        const auto* integer = &meta::ref_type_of_v<std::int32_t>;
        FunctionPayload signature{{{integer, "argument"}}, {{integer, "result"}}};
        const auto pins = signature.describePins();
        require(pins.has_value() && pins->size() == 2);
        require((*pins)[0].role == EFlowPinRole::EXECUTION);
        require((*pins)[1].name == "argument" && (*pins)[1].type == integer);
        require((*pins)[1].semantic.value == (std::uint64_t{5} << 56 | 2));
        FunctionCallPayload call{{19}, signature.arguments, signature.results};
        FunctionReturnPayload returned{{19}, signature.results};
        auto renamed = signature;
        renamed.arguments[0].name = "different label";
        renamed.results[0].name = "renamed result";
        require(call.matchesSignature(renamed) && returned.matchesSignature(renamed));
        const auto call_pins = call.describePins();
        require(call_pins.has_value() && call_pins->size() == 4);
        require((*call_pins)[1].direction == graph::EPinDirection::INPUT && (*call_pins)[1].allow_default);
        require((*call_pins)[2].role == EFlowPinRole::EXECUTION);
        require((*call_pins)[3].name == "result" && !(*call_pins)[3].allow_default);
        const auto return_pins = returned.describePins();
        require(return_pins.has_value() && return_pins->size() == 2 && (*return_pins)[1].allow_default);

        FlowNode definition;
        auto payload = FlowNodePayload::make<FunctionPayload, cloneFunction>(
            object::CodeLease::builtin(),
            std::move(signature.arguments),
            std::move(signature.results)
        );
        require(payload.has_value());
        definition.payload = std::move(*payload);
        const FlowNode* current = &definition;
        auto lookup = [&](graph::NodeId id) noexcept { return id == graph::NodeId{19} ? current : nullptr; };
        auto variable = [](std::uint64_t) noexcept -> const meta::RefType* { return nullptr; };
        FlowReferenceView references{lookup, variable};
        require(call.validateReferences(references).has_value());
        require(returned.validateReferences(references).has_value());
        auto cloned = definition.payload.clone();
        require(cloned.has_value());
        FlowNode replacement;
        replacement.payload = std::move(*cloned);
        current = &replacement;
        definition.payload = {};
        require(call.validateReferences(references).has_value());
        require(returned.validateReferences(references).has_value());
        auto* changed = replacement.payload.get<FunctionPayload>();
        changed->arguments[0].type = &meta::ref_type_of_v<float>;
        const auto refused = call.validateReferences(references);
        require(!refused && refused.error().message == "graph function call signature differs from its definition");
        require(returned.validateReferences(references).has_value());
        changed->results[0].type = &meta::ref_type_of_v<float>;
        require(!returned.validateReferences(references));
        current = nullptr;
        require(!call.validateReferences(references) && !returned.validateReferences(references));
        EventEntryPayload event{{{integer, "elapsed"}}};
        require(event.describePins()->at(1).name == "elapsed");
        event.parameters[0].type = nullptr;
        require(!event.describePins());
    }

    void native() noexcept
    {
        static_assert(!std::is_polymorphic_v<NativeCallPayload>);
        NativeCallPayload payload;
        require(!payload.describePins());
        require(!payload.argumentSemantics());
        {
            meta::RefInvokable signature;
            std::string name{"external.compute"};
            std::string argument{"input"};
            signature.name = name;
            signature.full_name = name;
            signature.type_signature = "int(int)";
            signature.return_type = meta::ref_type_of_v<int>;
            const auto type = meta::ref_type_of_v<int>;
            signature.parameters.push_back({argument, type, type.name, type.hash, false});
            auto definition = NativeCallDefinition::create(signature, object::CodeLease::builtin(), &type);
            require(definition.has_value());
            payload.definition = std::move(*definition);
            name.assign(name.size(), '#');
            argument.assign(argument.size(), '#');
        }
        const auto pins = payload.describePins();
        require(pins.has_value() && pins->size() == 5);
        require((*pins)[1].name == "input" && (*pins)[1].allow_default);
        require((*pins)[2].name == "Self" && !(*pins)[2].allow_default);
        require((*pins)[3].role == EFlowPinRole::EXECUTION);
        require((*pins)[4].name == "Return" && (*pins)[4].type->size == sizeof(int));
        const auto arguments = payload.argumentSemantics();
        require(arguments.has_value() && arguments->size() == 2);
        require((*arguments)[0] == (*pins)[2].semantic && (*arguments)[1] == (*pins)[1].semantic);
        auto shared = payload;
        payload.definition.reset();
        require(shared.definition->signature().name == "external.compute");
        require(shared.describePins()->at(1).type->size == sizeof(int));
    }

    void events() noexcept
    {
        static_assert(!std::is_polymorphic_v<ScriptEventPayload>);
        script::ScriptEventSourceDescription description;
        description.system_name = "external.system";
        description.event_name = "arrived";
        description.payload =
            {"lux.i32", semantic::typeId("lux.i32"), static_cast<std::uint8_t>(semantic::EAbiKind::I32), 4, 4};
        auto payload = std::make_unique<ScriptEventPayload>(description);
        description.system_name = "changed";
        description.payload.canonical_name = "invalid";
        description.payload.size = 99;
        auto pins = payload->describePins();
        require(pins.has_value() && pins->size() == 3);
        require((*pins)[0].name == "Execute" && (*pins)[1].name == "Received");
        const auto* type = (*pins)[2].type;
        require(type->name == "lux.i32" && type->size == 4 && type->alignment == 4);
        ScriptEventPayload moved{std::move(*payload)};
        payload.reset();
        require(moved.source().system_name == "external.system");
        require(moved.describePins()->at(2).type == type);
        ScriptEventPayload replaced{description};
        replaced = std::move(moved);
        require(replaced.describePins()->at(2).type == type && type->name == "lux.i32");
    }
} // namespace

int main()
{
    functions();
    native();
    events();
    std::puts("PASS: plain function/reference/native/event semantics, owned metadata and stable pin schemas");
}
