#include <lux/engine/flowforge/FlowNodePayload.hpp>
#include <lux/engine/meta/RuntimeObject.hpp>

#include <cstdio>
#include <cstdlib>
#include <source_location>
#include <string>

namespace
{
    using namespace lux;
    using namespace lux::flowforge;

    void require(bool condition, std::source_location location = std::source_location::current()) noexcept
    {
        if (!condition)
        {
            std::fprintf(stderr, "%s:%u\n", location.file_name(), location.line());
            std::abort();
        }
    }

    struct Lifetime final
    {
        int constructed{};
        int destroyed{};
        bool code_released{};
    };

    struct Literal final
    {
        meta::RuntimeObject value;
        Lifetime* lifetime;
        bool reject_clone{};
        bool empty_clone{};

        Literal(meta::RuntimeObject input, Lifetime& state) noexcept : value(std::move(input)), lifetime(&state)
        {
            ++lifetime->constructed;
        }

        ~Literal()
        {
            require(!lifetime->code_released);
            ++lifetime->destroyed;
        }
    };

    FlowForgeResult<std::unique_ptr<Literal>> cloneLiteral(const Literal& input) noexcept
    {
        if (input.reject_clone)
        {
            return cxx::unexpected(
                FlowForgeFailure{EFlowForgeError::INVALID_DESCRIPTION, "literal clone rejected", 37, 41}
            );
        }
        if (input.empty_clone)
        {
            return std::unique_ptr<Literal>{};
        }
        auto value = input.value.clone();
        if (!value)
        {
            return cxx::unexpected(
                FlowForgeFailure{EFlowForgeError::INVALID_DESCRIPTION, "literal value cannot be cloned"}
            );
        }
        return std::make_unique<Literal>(std::move(*value), *input.lifetime);
    }

    object::CodeLease codeFor(Lifetime& state) noexcept
    {
        auto owner = std::shared_ptr<const void>(
            &state,
            [&state](const void*) noexcept
            {
                require(state.constructed == state.destroyed);
                state.code_released = true;
            }
        );
        return object::CodeLease::plugin(std::move(owner));
    }
} // namespace

int main()
{
    static_assert(!std::is_copy_constructible_v<FlowNodePayload>);
    static_assert(!std::is_copy_assignable_v<FlowNodePayload>);
    static_assert(std::is_nothrow_move_constructible_v<FlowNodePayload>);
    static_assert(std::is_nothrow_move_assignable_v<FlowNodePayload>);
    static_assert(static_cast<unsigned>(EFlowForgeError::FOREIGN_EXCEPTION) == 4);
    static_assert(static_cast<unsigned>(EFlowForgeError::UNSUPPORTED_COROUTINE_CONTROL_FLOW) == 24);

    FlowNodePayload empty;
    require(empty.get<Literal>() == nullptr);
    require(!empty.clone());
    Lifetime rejected;
    auto invalid =
        FlowNodePayload::make<Literal, &cloneLiteral>(object::CodeLease::plugin({}), meta::RuntimeObject{17}, rejected);
    require(!invalid && rejected.constructed == 0);

    meta::meta_module_init();
    {
        Lifetime first;
        Lifetime second;
        auto text = meta::RuntimeObject::create(std::string{"owned reflected value"});
        require(text.has_value());
        auto created = FlowNodePayload::make<Literal, &cloneLiteral>(codeFor(first), std::move(*text), first);
        require(created.has_value());
        auto payload = std::move(*created);
        require(!created->clone());
        require(payload.get<int>() == nullptr);
        const auto& observed = payload;
        require(observed.get<Literal>() != nullptr);
        auto copy = payload.clone();
        require(copy.has_value());
        auto* copied_value = static_cast<std::string*>(copy->get<Literal>()->value.data());
        *copied_value = "independent copy";
        require(*static_cast<const std::string*>(payload.get<Literal>()->value.data()) == "owned reflected value");

        payload.get<Literal>()->reject_clone = true;
        auto failure = payload.clone();
        require(!failure);
        require(failure.error().code == EFlowForgeError::INVALID_DESCRIPTION);
        require(failure.error().message == "literal clone rejected");
        require(failure.error().node_id == 37 && failure.error().pin_id == 41);
        require(first.constructed == 2 && first.destroyed == 0);
        payload.get<Literal>()->reject_clone = false;
        payload.get<Literal>()->empty_clone = true;
        require(!payload.clone());

        auto replacement =
            FlowNodePayload::make<Literal, &cloneLiteral>(codeFor(second), meta::RuntimeObject{23}, second);
        require(replacement.has_value());
        payload = std::move(*replacement);
        require(first.destroyed == 1 && !first.code_released);
        require(!replacement->clone());
        require(payload.get<Literal>()->value.get<int>() == 23);
        auto* self = &payload;
        payload = std::move(*self);
        require(second.destroyed == 0 && payload.get<Literal>()->value.get<int>() == 23);
        *copy = FlowNodePayload{};
        require(first.destroyed == 2 && first.code_released);
        payload = FlowNodePayload{};
        require(second.destroyed == 1 && second.code_released);
    }
    meta::meta_module_deinit();
    std::puts("PASS Flow payload: owning reflected literal, fallible deep clone, moves and code cleanup order");
}
