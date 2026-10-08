#include <lux/engine/input/ActionMapper.hpp>
#include <lux/engine/input/InputContext.hpp>
#include <lux/engine/input/InputContextStack.hpp>

#include <cassert>
#include <cstdio>
#include <optional>
#include <type_traits>
#include <utility>
#include <vector>

using namespace lux::input;

namespace
{
    template <class T>
    concept HasEnabled = requires(T& value) { value.setEnabled(true); };

    template <class T>
    concept HasPriority = requires(T& value) { value.setPriority(1); };

    template <class T>
    concept HasPush = requires(T& value, InputContext* context) { value.push(context); };

    void orderAndMoves()
    {
        static_assert(!std::is_copy_constructible_v<InputContextActivation>);
        static_assert(!std::is_copy_assignable_v<InputContextActivation>);
        static_assert(std::is_nothrow_move_constructible_v<InputContextActivation>);
        static_assert(std::is_nothrow_move_assignable_v<InputContextActivation>);
        static_assert(!std::is_move_constructible_v<InputContext>);
        static_assert(!std::is_copy_constructible_v<InputContextStack>);
        static_assert(!HasEnabled<InputContext> && !HasPriority<InputContext> && !HasPush<InputContextStack>);
        InputContext a{"a"}, b{"b"}, c{"c"};
        InputContextStack stack;
        assert(stack.empty() && !stack.top() && !stack.contains(nullptr));
        auto first = stack.activate(a, 1);
        auto second = stack.activate(b, 2);
        auto third = stack.activate(c, 2);
        assert(first && second && third);
        assert(stack.size() == 3 && stack.top() == &c);
        assert(&stack[0] == &a && &stack[1] == &b && &std::as_const(stack)[2] == &c);
        assert(std::as_const(stack).top() == &c);
        assert(stack.activate(b, 99).error() == EInputActivationError::DUPLICATE_CONTEXT);
        assert(stack.size() == 3 && stack.top() == &c);
        InputContextActivation moved{std::move(*second)};
        assert(moved && !*second && stack.contains(&b));
        moved = std::move(moved);
        assert(moved && stack.size() == 3);
        *first = std::move(moved);
        assert(!moved && stack.size() == 2 && !stack.contains(&a) && stack.contains(&b));
        *first = {};
        assert(stack.size() == 1 && stack.top() == &c);
        first = stack.activate(a, 3);
        assert(first && stack.top() == &a);
        InputContextStack other;
        auto other_first = other.activate(a, -2);
        auto other_third = other.activate(c, -1);
        assert(other_first && other_third && other.top() == &c && stack.top() == &a);
    }

    void endpointDeathAndReuse()
    {
        InputContextStack first, second;
        std::optional<InputContext> context{std::in_place, "temporary"};
        auto a = first.activate(*context);
        auto b = second.activate(*context);
        assert(a && b);
        auto* original_address = &*context;
        context.reset();
        assert(first.empty() && second.empty() && !*a && !*b);
        context.emplace("replacement");
        assert(&*context == original_address);
        auto replacement = first.activate(*context);
        assert(replacement);
        *a = {};
        *b = {};
        assert(first.size() == 1 && first.top() == &*context);

        std::optional<InputContextStack> stack{std::in_place};
        auto old = stack->activate(*context);
        assert(old);
        auto* stack_address = &*stack;
        stack.reset();
        assert(!*old && first.contains(&*context));
        stack.emplace();
        assert(&*stack == stack_address);
        auto current = stack->activate(*context);
        assert(current);
        *old = {};
        assert(*current && stack->size() == 1);
        context.reset();
        assert(!*current && !*replacement && stack->empty() && first.empty());

        InputContext shared{"shared"};
        std::vector<InputContextActivation> leases;
        std::vector<std::unique_ptr<InputContextStack>> stacks;
        for (int i = 0; i != 32; ++i)
        {
            stacks.push_back(std::make_unique<InputContextStack>());
            auto accepted = stacks.back()->activate(shared, i);
            assert(accepted);
            leases.push_back(std::move(*accepted));
        }
        for (std::size_t i = 1; i < stacks.size(); i += 2)
        {
            stacks[i].reset();
            assert(!leases[i]);
        }
        leases.clear();
        for (std::size_t i = 0; i < stacks.size(); i += 2)
        {
            assert(stacks[i]->empty());
        }
    }

    void actionConsumption()
    {
        ActionMapper mapper;
        const auto low = mapper.actionRegistry().registerAction({.name = "low"});
        const auto high = mapper.actionRegistry().registerAction({.name = "high"});
        InputContext game{"game"};
        InputContext modal{"modal", true};
        game.actionMap().bindKey(low, EKey::KEY_A);
        modal.actionMap().bindKey(high, EKey::KEY_A);
        InputContextStack stack;
        auto game_active = stack.activate(game, 0);
        auto modal_active = stack.activate(modal, 1);
        assert(game_active && modal_active);
        InputSnapshot snapshot;
        snapshot.keys_held.set(static_cast<std::size_t>(EKey::KEY_A));
        snapshot.keys_just_pressed = snapshot.keys_held;
        mapper.update(snapshot, stack, 0.016F);
        assert(mapper.active(high) && !mapper.active(low));
        mapper.update(snapshot, stack, 0.016F, false);
        assert(!mapper.active(high) && !mapper.active(low));
        *modal_active = {};
        mapper.update(snapshot, stack, 0.016F);
        assert(mapper.active(low) && !mapper.active(high));
        assert(mapper.getValue(low).v.x == 1.0F);
        *game_active = {};
        mapper.update(snapshot, stack, 0.016F);
        assert(!mapper.active(low) && !mapper.active(high));
    }
} // namespace

int main()
{
    orderAndMoves();
    endpointDeathAndReuse();
    actionConsumption();
    std::puts("PASS input activation moves, endpoint death/reuse, fixed ordering, duplicate rejection and real "
              "ActionMapper/UI capture");
}
