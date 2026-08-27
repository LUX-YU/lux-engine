/// @file input_system_test.cpp
/// Comprehensive headless test for the gameplay input system.
/// Builds synthetic InputSnapshot frames and exercises the full ActionMapper pipeline.
///
/// Covers:
///   - ActionId runtime allocation via InputActionRegistry (SparseSet-backed)try (SparseSet-backed)try
///   (SparseSet-backed)
///   - InputValue factories, accessors, accumulateValue (untyped + typed)
///   - InputModifier application chain
///   - TriggerEvaluator: Down, Pressed, Released, Hold, HoldAndRelease, Tap, Pulse, ChordAction
///   - TriggerGroup Explicit / Implicit / Blocker composition
///   - ActionMap builder API (bindKey, bindMouseButton, bindMouseAxis)
///   - BindingIdAllocator global uniqueness
///   - InputActionRegistry registration, collision guard
///   - InputContext enabled flag, priority ordering
///   - InputContextStack push dedup, priority sort, pop
///   - ActionMapper full pipeline: beginFrame → evaluateBindings → accumulateActions
///                                 → evaluateActionLayer → finalizeEvents
///   - ActionMapper query API: triggered, performed, ongoing, canceled, active, axis, getValue, state
///   - ActionMapper injection API: injectTriggered, injectValue
///   - IActionDispatcher / ActionMapperDispatcher
///   - Multi-frame edge detection: Started / Completed events
///   - Multi-binding accumulation (WASD → Axis2D)
///   - Context consume flags (keyboard / mouse)
///   - UI capture (keyboard_captured_by_ui / mouse_captured_by_ui)
///   - Action-level modifiers (Normalize2D) and action-level triggers (Hold)
///
/// Uses plain assert() — no external test framework required.

#include <lux/engine/input/ActionMapper.hpp>
#include <lux/engine/input/ActionDispatcher.hpp>
#include <lux/engine/input/BindingIdAllocator.hpp>
#include <lux/engine/input/Input.hpp>

#include <cassert>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

// Suppress deprecation warnings — this test intentionally exercises deprecated API
// (accumulateValue 2-arg and the old scalar ActionMapper API).
#if defined(_MSC_VER)
#pragma warning(disable : 4996)
#elif defined(__GNUC__) || defined(__clang__)
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#endif

using namespace lux::input;

static_assert(static_cast<int>(EKey::KEY_SPACE) == 32);
static_assert(static_cast<int>(EKey::KEY_MENU) == 348);
static_assert(static_cast<int>(EMouseButton::MOUSE_BUTTON_LEFT) == 0);
static_assert(static_cast<int>(EMouseButton::MOUSE_BUTTON_8) == 7);
static_assert(static_cast<int>(EMouseButton::UNKNOWN) == 8);
static_assert(static_cast<int>(EKeyModifier::KEY_MOD_SHIFT) == 0x0001);
static_assert(static_cast<int>(EKeyModifier::KEY_MOD_CONTROL) == 0x0002);
static_assert(static_cast<int>(EKeyModifier::KEY_MOD_ALT) == 0x0004);
static_assert(static_cast<int>(EKeyModifier::KEY_MOD_SUPER) == 0x0008);
static_assert(static_cast<int>(EKeyModifier::KEY_MOD_CAPS_LOCK) == 0x0010);
static_assert(static_cast<int>(EKeyModifier::KEY_MOD_NUM_LOCK) == 0x0020);
static_assert(static_cast<int>(EInputState::PRESS) == 0);
static_assert(static_cast<int>(EInputState::RELEASE) == 1);
static_assert(static_cast<int>(EInputState::REPEAT) == 2);
static_assert(static_cast<int>(EInputState::UNKNOWN) == 3);
static_assert(static_cast<int>(ETouchPhase::BEGAN) == 0);
static_assert(static_cast<int>(ETouchPhase::MOVED) == 1);
static_assert(static_cast<int>(ETouchPhase::STATIONARY) == 2);
static_assert(static_cast<int>(ETouchPhase::ENDED) == 3);
static_assert(static_cast<int>(ETouchPhase::CANCELED) == 4);

// ═══════════════════════════════════════════════════════════════════════════ //
//  Test action IDs                                                          //
// ═══════════════════════════════════════════════════════════════════════════ //

namespace Actions
{
    ActionId Jump = InvalidActionId;
    ActionId Move = InvalidActionId;
    ActionId Look = InvalidActionId;
    ActionId Fire = InvalidActionId;
    ActionId Aim = InvalidActionId;
    ActionId Sprint = InvalidActionId;
    ActionId Crouch = InvalidActionId;
    ActionId Interact = InvalidActionId;
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  Helpers                                                                  //
// ═══════════════════════════════════════════════════════════════════════════ //

static constexpr float kEps = 1e-5f;
static bool
near(float a, float b)
{
    return std::fabs(a - b) < kEps;
}

/// Create a blank InputSnapshot with no keys/mouse pressed.
static InputSnapshot
makeBlankSnapshot(float dt = 1.f / 60.f)
{
    InputSnapshot s{};
    s.sample_dt = dt;
    return s;
}

/// Press a key: just_pressed + held.
static void
pressKey(InputSnapshot& s, EKey k)
{
    auto i = static_cast<size_t>(static_cast<int>(k));
    s.keys_just_pressed.set(i);
    s.keys_held.set(i);
}

/// Hold a key (already pressed in previous frame): held only, no edge.
static void
holdKey(InputSnapshot& s, EKey k)
{
    auto i = static_cast<size_t>(static_cast<int>(k));
    s.keys_held.set(i);
}

/// Release a key: just_released, clear held.
static void
releaseKey(InputSnapshot& s, EKey k)
{
    auto i = static_cast<size_t>(static_cast<int>(k));
    s.keys_just_released.set(i);
    s.keys_held.reset(i);
}

/// Press a mouse button.
static void
pressMouse(InputSnapshot& s, EMouseButton btn)
{
    auto i = static_cast<int>(btn);
    s.mouse_just_pressed |= (1u << i);
    s.mouse_held |= (1u << i);
}

/// Hold a mouse button.
static void
holdMouse(InputSnapshot& s, EMouseButton btn)
{
    auto i = static_cast<int>(btn);
    s.mouse_held |= (1u << i);
}

/// Release a mouse button.
static void
releaseMouse(InputSnapshot& s, EMouseButton btn)
{
    auto i = static_cast<int>(btn);
    s.mouse_just_released |= (1u << i);
    s.mouse_held &= ~(1u << i);
}

static int tests_passed = 0;

#define TEST_SECTION(name)                                                                                             \
    do                                                                                                                 \
    {                                                                                                                  \
        std::printf("  [PASS] %s\n", name);                                                                            \
        ++tests_passed;                                                                                                \
    } while (0)

// ═══════════════════════════════════════════════════════════════════════════ //
//  1. ActionId                                                              //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_action_id()
{
    // ActionId is a runtime-allocated integer. InvalidActionId == 0.
    static_assert(InvalidActionId == 0, "InvalidActionId must be zero");
    static_assert(std::is_same_v<ActionId, uint32_t>, "ActionId is uint32_t");

    // Registry allocates sequential IDs starting from 1.
    InputActionRegistry reg;
    ActionId a = reg.registerAction({0, "jump", EInputValueType::BOOL});
    ActionId b = reg.registerAction({0, "fire", EInputValueType::BOOL});
    assert(a != InvalidActionId);
    assert(b != InvalidActionId);
    assert(a != b);

    // Registering the same name again returns the same id (idempotent).
    ActionId a2 = reg.registerAction({0, "jump", EInputValueType::BOOL});
    assert(a2 == a);

    // findByName works for registered actions.
    assert(reg.findByName("jump") == a);
    assert(reg.findByName("fire") == b);
    assert(reg.findByName("unknown") == InvalidActionId);

    TEST_SECTION("ActionId runtime allocation + registry basics");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  2. InputValue                                                            //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_input_value()
{
    // Bool factory + accessor
    {
        auto v = InputValue::makeBool(true);
        assert(v.type == EInputValueType::BOOL);
        assert(v.asBool() == true);
        assert(near(v.as1D(), 1.0f));

        auto v2 = InputValue::makeBool(false);
        assert(v2.asBool() == false);
        assert(v2.nearlyZero());
    }

    // Axis1D
    {
        auto v = InputValue::makeAxis1D(3.14f);
        assert(v.type == EInputValueType::AXIS_1D);
        assert(near(v.as1D(), 3.14f));
        assert(!v.nearlyZero());
    }

    // Axis2D
    {
        auto v = InputValue::makeAxis2D(1.0f, -2.0f);
        assert(v.type == EInputValueType::AXIS_2D);
        auto [x, y] = v.as2D();
        assert(near(x, 1.0f));
        assert(near(y, -2.0f));
    }

    // Axis3D
    {
        auto v = InputValue::makeAxis3D(1.f, 2.f, 3.f);
        assert(v.type == EInputValueType::AXIS_3D);
        auto r = v.as3D();
        assert(near(r.x, 1.f) && near(r.y, 2.f) && near(r.z, 3.f));
    }

    // zeroOf
    {
        auto z2 = InputValue::zeroOf(EInputValueType::AXIS_2D);
        assert(z2.type == EInputValueType::AXIS_2D);
        assert(z2.nearlyZero());
    }

    // accumulateValue — same type
    {
        auto a = InputValue::makeAxis2D(1.f, 0.f);
        auto b = InputValue::makeAxis2D(0.f, -1.f);
        auto c = accumulateValue(a, b, EInputValueType::AXIS_2D);
        assert(c.type == EInputValueType::AXIS_2D);
        assert(near(c.as2D().x, 1.f));
        assert(near(c.as2D().y, -1.f));
    }

    // accumulateValue — Bool zero adopts incoming type via target_type
    {
        auto acc = InputValue::makeBool(false);
        auto b = InputValue::makeAxis1D(5.f);
        auto c = accumulateValue(acc, b, EInputValueType::AXIS_1D);
        assert(c.type == EInputValueType::AXIS_1D);
        assert(near(c.as1D(), 5.f));
    }

    // accumulateValue — typed overload initializes to correct type
    {
        auto acc = InputValue{}; // default: Bool, zero
        auto b = InputValue::makeAxis2D(1.f, 0.f);
        auto c = accumulateValue(acc, b, EInputValueType::AXIS_2D);
        assert(c.type == EInputValueType::AXIS_2D);
        assert(near(c.as2D().x, 1.f));
        assert(near(c.as2D().y, 0.f));
    }

    // Bool accumulation — OR semantics
    {
        auto a = InputValue::makeBool(false);
        auto b = InputValue::makeBool(true);
        auto c = accumulateValue(a, b, EInputValueType::BOOL);
        assert(c.asBool() == true);
    }

    TEST_SECTION("InputValue factories + accumulateValue");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  3. InputModifier                                                         //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_input_modifier()
{
    // Scale
    {
        auto v = InputValue::makeAxis2D(2.f, 3.f);
        ModifierSpec ms{EModifierKind::SCALE, 0.5f, 2.f, 0.f};
        auto r = applyModifier(v, ms);
        assert(near(r.as2D().x, 1.f));
        assert(near(r.as2D().y, 6.f));
    }

    // NegateX / NegateY
    {
        auto v = InputValue::makeAxis2D(1.f, 2.f);
        auto r = applyModifier(v, {EModifierKind::NEGATE_X});
        assert(near(r.as2D().x, -1.f));
        assert(near(r.as2D().y, 2.f));

        r = applyModifier(v, {EModifierKind::NEGATE_Y});
        assert(near(r.as2D().x, 1.f));
        assert(near(r.as2D().y, -2.f));
    }

    // DeadZone — Axis1D
    {
        auto v = InputValue::makeAxis1D(0.05f);
        ModifierSpec dz{EModifierKind::DEAD_ZONE, 0.1f, 0.f, 0.f};
        auto r = applyModifier(v, dz);
        assert(near(r.as1D(), 0.f)); // below dead zone

        auto v2 = InputValue::makeAxis1D(0.5f);
        auto r2 = applyModifier(v2, dz);
        assert(near(r2.as1D(), 0.5f)); // above dead zone
    }

    // Normalize2D
    {
        auto v = InputValue::makeAxis2D(3.f, 4.f);
        auto r = applyModifier(v, {EModifierKind::NORMALIZE_2D});
        float len = std::sqrt(r.as2D().x * r.as2D().x + r.as2D().y * r.as2D().y);
        assert(near(len, 1.0f));
    }

    // applyModifiers chain
    {
        auto v = InputValue::makeAxis2D(3.f, 4.f);
        std::vector<ModifierSpec> chain = {
            {EModifierKind::NORMALIZE_2D},
            {EModifierKind::SCALE, 2.f, 2.f, 0.f},
        };
        auto r = applyModifiers(v, chain);
        float len = std::sqrt(r.as2D().x * r.as2D().x + r.as2D().y * r.as2D().y);
        assert(near(len, 2.0f));
    }

    TEST_SECTION("InputModifier apply chain");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  4. TriggerEvaluator — individual triggers                                //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_trigger_evaluator()
{
    const float dt = 1.f / 60.f;

    // Down — Triggered when value exceeds threshold
    {
        TriggerDesc desc{ETriggerKind::DOWN};
        desc.actuation_threshold = 0.5f;
        TriggerRuntimeState rt{};

        auto r1 = evaluateTrigger(desc, rt, InputValue::makeAxis1D(0.f), dt);
        assert(r1 == ETriggerState::NONE);

        auto r2 = evaluateTrigger(desc, rt, InputValue::makeAxis1D(1.f), dt);
        assert(r2 == ETriggerState::TRIGGERED);
    }

    // Pressed — fires only on rising edge
    {
        TriggerDesc desc{ETriggerKind::PRESSED};
        desc.actuation_threshold = 0.5f;
        TriggerRuntimeState rt{};

        auto r1 = evaluateTrigger(desc, rt, InputValue::makeAxis1D(0.f), dt);
        assert(r1 == ETriggerState::NONE);

        auto r2 = evaluateTrigger(desc, rt, InputValue::makeAxis1D(1.f), dt);
        assert(r2 == ETriggerState::TRIGGERED); // rising edge

        auto r3 = evaluateTrigger(desc, rt, InputValue::makeAxis1D(1.f), dt);
        assert(r3 == ETriggerState::NONE); // still held — no edge
    }

    // Released — fires only on falling edge
    {
        TriggerDesc desc{ETriggerKind::RELEASED};
        desc.actuation_threshold = 0.5f;
        TriggerRuntimeState rt{};

        evaluateTrigger(desc, rt, InputValue::makeAxis1D(1.f), dt); // actuate first
        auto r = evaluateTrigger(desc, rt, InputValue::makeAxis1D(0.f), dt);
        assert(r == ETriggerState::TRIGGERED);
    }

    // Hold — Ongoing until time, then Triggered
    {
        TriggerDesc desc{ETriggerKind::HOLD};
        desc.hold_time_seconds = 0.1f;
        desc.actuation_threshold = 0.5f;
        TriggerRuntimeState rt{};

        // 3 frames of holding (3 * 1/60 ≈ 0.05s) — still Ongoing
        for (int i = 0; i < 3; ++i)
        {
            auto r = evaluateTrigger(desc, rt, InputValue::makeAxis1D(1.f), dt);
            assert(r == ETriggerState::ONGOING);
        }

        // Hold for enough frames to exceed 0.1s
        for (int i = 0; i < 10; ++i)
        {
            evaluateTrigger(desc, rt, InputValue::makeAxis1D(1.f), dt);
        }
        // By now elapsed > 0.1s total
        auto r = evaluateTrigger(desc, rt, InputValue::makeAxis1D(1.f), dt);
        assert(r == ETriggerState::TRIGGERED);
    }

    // Tap — Ongoing while held briefly, Triggered on release within time
    {
        TriggerDesc desc{ETriggerKind::TAP};
        desc.tap_time_seconds = 0.2f;
        desc.actuation_threshold = 0.5f;
        TriggerRuntimeState rt{};

        // Press for 2 frames
        auto r1 = evaluateTrigger(desc, rt, InputValue::makeAxis1D(1.f), dt);
        assert(r1 == ETriggerState::ONGOING);
        auto r2 = evaluateTrigger(desc, rt, InputValue::makeAxis1D(1.f), dt);
        assert(r2 == ETriggerState::ONGOING);

        // Release — should fire Triggered
        auto r3 = evaluateTrigger(desc, rt, InputValue::makeAxis1D(0.f), dt);
        assert(r3 == ETriggerState::TRIGGERED);
    }

    // Pulse — fires repeatedly at interval while held
    {
        TriggerDesc desc{ETriggerKind::PULSE};
        desc.pulse_interval = 0.05f;
        desc.actuation_threshold = 0.5f;
        TriggerRuntimeState rt{};

        int trigger_count = 0;
        for (int i = 0; i < 10; ++i)
        {
            auto r = evaluateTrigger(desc, rt, InputValue::makeAxis1D(1.f), dt);
            if (r == ETriggerState::TRIGGERED)
                ++trigger_count;
        }
        // In 10 * 1/60 ≈ 0.167s with interval 0.05s, expect ~3 triggers
        assert(trigger_count >= 2 && trigger_count <= 5);
    }

    // ChordAction — requires another action to be active
    {
        TriggerDesc desc{ETriggerKind::CHORD_ACTION};
        desc.actuation_threshold = 0.5f;
        desc.chord_action = Actions::Sprint;
        TriggerRuntimeState rt{};

        // Sprint NOT active → Ongoing (value is actuated but chord not met)
        auto notActive = [](ActionId) -> bool { return false; };
        auto r1 = evaluateTrigger(desc, rt, InputValue::makeAxis1D(1.f), dt, notActive);
        assert(r1 == ETriggerState::ONGOING);

        // Sprint IS active → Triggered
        auto isActive = [](ActionId) -> bool { return true; };
        auto r2 = evaluateTrigger(desc, rt, InputValue::makeAxis1D(1.f), dt, isActive);
        assert(r2 == ETriggerState::TRIGGERED);
    }

    TEST_SECTION("TriggerEvaluator individual triggers");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  5. TriggerGroup — Explicit / Implicit / Blocker composition              //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_trigger_group()
{
    const float dt = 1.f / 60.f;
    auto val = InputValue::makeAxis1D(1.f);
    auto zero = InputValue::makeAxis1D(0.f);

    // Empty group → Triggered (pass-through)
    {
        std::vector<TriggerDesc> descs;
        std::vector<TriggerRuntimeState> rts;
        auto r = evaluateTriggerGroup(descs, rts, val, dt);
        assert(r == ETriggerState::TRIGGERED);
    }

    // Single Explicit Down → Triggered when actuated
    {
        std::vector<TriggerDesc> descs = {{ETriggerKind::DOWN, ETriggerLogicType::EXPLICIT, 0.5f}};
        std::vector<TriggerRuntimeState> rts;
        auto r = evaluateTriggerGroup(descs, rts, val, dt);
        assert(r == ETriggerState::TRIGGERED);
    }

    // Blocker blocks even when Explicit passes
    {
        std::vector<TriggerDesc> descs = {
            {ETriggerKind::DOWN, ETriggerLogicType::EXPLICIT, 0.5f},
            {ETriggerKind::DOWN, ETriggerLogicType::BLOCKER, 0.5f},
        };
        std::vector<TriggerRuntimeState> rts;
        auto r = evaluateTriggerGroup(descs, rts, val, dt);
        assert(r == ETriggerState::NONE);
    }

    // Two Implicit both must pass
    {
        std::vector<TriggerDesc> descs = {
            {ETriggerKind::DOWN, ETriggerLogicType::IMPLICIT, 0.5f},
            {ETriggerKind::DOWN, ETriggerLogicType::IMPLICIT, 0.5f},
        };
        std::vector<TriggerRuntimeState> rts;

        // Both actuated → Triggered
        auto r1 = evaluateTriggerGroup(descs, rts, val, dt);
        assert(r1 == ETriggerState::TRIGGERED);

        // Not actuated → None (both fail)
        rts.clear();
        auto r2 = evaluateTriggerGroup(descs, rts, zero, dt);
        assert(r2 == ETriggerState::NONE);
    }

    TEST_SECTION("TriggerGroup Explicit/Implicit/Blocker");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  6. BindingIdAllocator — global uniqueness                                //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_binding_id_allocator()
{
    BindingId a = BindingIdAllocator::next();
    BindingId b = BindingIdAllocator::next();
    BindingId c = BindingIdAllocator::next();
    assert(a != b);
    assert(b != c);
    assert(a != c);
    assert(a != InvalidBindingId || b != InvalidBindingId); // at least non-zero

    TEST_SECTION("BindingIdAllocator global uniqueness");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  7. ActionMap builder + cross-map BindingId uniqueness                    //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_action_map_builder()
{
    // Allocate valid action IDs for this test via a temporary registry.
    InputActionRegistry temp_reg;
    Actions::Jump = temp_reg.registerAction({0, "jump", EInputValueType::BOOL});
    Actions::Move = temp_reg.registerAction({0, "move", EInputValueType::AXIS_2D});
    Actions::Fire = temp_reg.registerAction({0, "fire", EInputValueType::BOOL});
    Actions::Look = temp_reg.registerAction({0, "look", EInputValueType::AXIS_2D});
    Actions::Crouch = temp_reg.registerAction({0, "crouch", EInputValueType::BOOL});

    ActionMap map;

    map.bindKey(Actions::Jump, EKey::KEY_SPACE, InputValue::makeBool(true))
        .bindKey(Actions::Move, EKey::KEY_W, InputValue::makeAxis2D(0.f, 1.f))
        .bindKey(Actions::Move, EKey::KEY_S, InputValue::makeAxis2D(0.f, -1.f))
        .bindKey(Actions::Move, EKey::KEY_A, InputValue::makeAxis2D(-1.f, 0.f))
        .bindKey(Actions::Move, EKey::KEY_D, InputValue::makeAxis2D(1.f, 0.f))
        .bindMouseButton(Actions::Fire, EMouseButton::MOUSE_BUTTON_LEFT)
        .bindMouseAxis(Actions::Look, MouseAxisInput::EAxis::DELTA_X, InputValue::makeAxis2D(1.f, 0.f))
        .bindMouseAxis(Actions::Look, MouseAxisInput::EAxis::DELTA_Y, InputValue::makeAxis2D(0.f, 1.f));

    assert(map.bindings().size() == 8);

    // All binding IDs should be unique.
    std::vector<BindingId> ids;
    for (auto& b : map.bindings())
    {
        assert(b.binding_id != InvalidBindingId);
        ids.push_back(b.binding_id);
    }
    for (size_t i = 0; i < ids.size(); ++i)
        for (size_t j = i + 1; j < ids.size(); ++j)
            assert(ids[i] != ids[j]);

    // IDs from a different ActionMap must also be unique vs the first map.
    ActionMap map2;
    map2.bindKey(Actions::Crouch, EKey::KEY_C, InputValue::makeBool(true));
    BindingId map2_id = map2.bindings()[0].binding_id;
    for (auto& id : ids)
        assert(id != map2_id);

    // unbindAll
    map.unbindAll(Actions::Move);
    int move_count = 0;
    for (auto& b : map.bindings())
        if (b.action == Actions::Move)
            ++move_count;
    assert(move_count == 0);
    assert(map.bindings().size() == 4); // Jump + Fire + Look×2

    // clear
    map.clear();
    assert(map.bindings().size() == 0);

    TEST_SECTION("ActionMap builder + cross-map BindingId uniqueness");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  8. InputActionRegistry                                                   //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_action_registry()
{
    InputActionRegistry reg;

    InputActionDesc jump_desc;
    jump_desc.name = "jump";
    jump_desc.value_type = EInputValueType::BOOL;

    ActionId jump_id = reg.registerAction(jump_desc);
    assert(jump_id != InvalidActionId);

    auto* found = reg.find(jump_id);
    assert(found != nullptr);
    assert(found->name == "jump");
    assert(found->value_type == EInputValueType::BOOL);
    assert(found->id == jump_id);

    // Registering same name returns existing id.
    jump_desc.consume_input = false;
    ActionId jump_id2 = reg.registerAction(jump_desc);
    assert(jump_id2 == jump_id);
    assert(reg.find(jump_id)->consume_input == false);

    // findByName
    assert(reg.findByName("jump") == jump_id);
    assert(reg.findByName("fire") == InvalidActionId);

    // Unregister.
    reg.unregisterAction(jump_id);
    assert(reg.find(jump_id) == nullptr);
    assert(reg.findByName("jump") == InvalidActionId);

    TEST_SECTION("InputActionRegistry register/find/unregister");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  9. InputContext + InputContextStack                                      //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_context_and_stack()
{
    InputContext gameplay("gameplay", false, false, 0);
    InputContext ui("ui", true, true, 10);

    // Enabled flag
    assert(gameplay.enabled());
    gameplay.setEnabled(false);
    assert(!gameplay.enabled());
    gameplay.setEnabled(true);

    // Priority
    assert(ui.priority() > gameplay.priority());

    // Stack: priority ordering
    InputContextStack stack;
    stack.push(&gameplay);
    stack.push(&ui);
    assert(stack.size() == 2);
    assert(stack.top() == &ui); // highest priority on top

    // Dedup: duplicate push ignored
    stack.push(&gameplay);
    assert(stack.size() == 2); // no change

    // contains
    assert(stack.contains(&gameplay));
    assert(stack.contains(&ui));

    InputContext other("other");
    assert(!stack.contains(&other));

    // pop specific
    assert(stack.pop(&gameplay));
    assert(stack.size() == 1);
    assert(!stack.contains(&gameplay));

    // pop top
    auto* top = stack.pop();
    assert(top == &ui);
    assert(stack.empty());

    TEST_SECTION("InputContext + InputContextStack");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  10. ActionMapper — basic key press → triggered                           //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_mapper_basic_key()
{
    ActionMapper mapper;
    auto& reg = mapper.actionRegistry();

    Actions::Jump = reg.registerAction({0, "jump", EInputValueType::BOOL});

    InputContext ctx("gameplay");
    ctx.actionMap().bindKey(Actions::Jump, EKey::KEY_SPACE, InputValue::makeBool(true));

    InputContextStack stack;
    stack.push(&ctx);

    // Frame 1: press Space
    {
        auto snap = makeBlankSnapshot();
        pressKey(snap, EKey::KEY_SPACE);
        mapper.update(snap, stack, snap.sample_dt);

        assert(mapper.triggered(Actions::Jump));
        assert(mapper.active(Actions::Jump));
        assert(mapper.state(Actions::Jump).started());
        assert(mapper.getValue(Actions::Jump).asBool());
    }

    // Frame 2: release Space
    {
        auto snap = makeBlankSnapshot();
        releaseKey(snap, EKey::KEY_SPACE);
        mapper.update(snap, stack, snap.sample_dt);

        assert(!mapper.triggered(Actions::Jump));
        assert(!mapper.active(Actions::Jump));
        assert(mapper.state(Actions::Jump).completed());
    }

    // Frame 3: nothing pressed — idle
    {
        auto snap = makeBlankSnapshot();
        mapper.update(snap, stack, snap.sample_dt);

        assert(!mapper.triggered(Actions::Jump));
        assert(!mapper.active(Actions::Jump));
        assert(!mapper.state(Actions::Jump).started());
        assert(!mapper.state(Actions::Jump).completed());
    }

    TEST_SECTION("ActionMapper basic key triggered + edge events");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  11. ActionMapper — WASD → Axis2D accumulation                            //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_mapper_wasd_accumulation()
{
    ActionMapper mapper;
    auto& reg = mapper.actionRegistry();

    Actions::Move = reg.registerAction({0, "move", EInputValueType::AXIS_2D});

    InputContext ctx("gameplay");
    ctx.actionMap()
        .bindKey(Actions::Move, EKey::KEY_W, InputValue::makeAxis2D(0.f, 1.f))
        .bindKey(Actions::Move, EKey::KEY_S, InputValue::makeAxis2D(0.f, -1.f))
        .bindKey(Actions::Move, EKey::KEY_A, InputValue::makeAxis2D(-1.f, 0.f))
        .bindKey(Actions::Move, EKey::KEY_D, InputValue::makeAxis2D(1.f, 0.f));

    InputContextStack stack;
    stack.push(&ctx);

    // Frame: press W only → (0, 1)
    {
        auto snap = makeBlankSnapshot();
        pressKey(snap, EKey::KEY_W);
        mapper.update(snap, stack, snap.sample_dt);

        auto v = mapper.getValue(Actions::Move).as2D();
        assert(near(v.x, 0.f));
        assert(near(v.y, 1.f));
    }

    // Frame: hold W + press D → (1, 1) diagonal
    {
        auto snap = makeBlankSnapshot();
        holdKey(snap, EKey::KEY_W);
        pressKey(snap, EKey::KEY_D);
        mapper.update(snap, stack, snap.sample_dt);

        auto v = mapper.getValue(Actions::Move).as2D();
        assert(near(v.x, 1.f));
        assert(near(v.y, 1.f));
        assert(mapper.active(Actions::Move));
    }

    // Frame: nothing → zero
    {
        auto snap = makeBlankSnapshot();
        mapper.update(snap, stack, snap.sample_dt);
        assert(!mapper.active(Actions::Move));
    }

    TEST_SECTION("ActionMapper WASD → Axis2D accumulation");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  12. ActionMapper — mouse button                                          //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_mapper_mouse_button()
{
    ActionMapper mapper;
    auto& reg = mapper.actionRegistry();

    Actions::Fire = reg.registerAction({0, "fire", EInputValueType::BOOL});

    InputContext ctx("gameplay");
    ctx.actionMap().bindMouseButton(Actions::Fire, EMouseButton::MOUSE_BUTTON_LEFT, InputValue::makeBool(true));

    InputContextStack stack;
    stack.push(&ctx);

    // Press left mouse
    {
        auto snap = makeBlankSnapshot();
        pressMouse(snap, EMouseButton::MOUSE_BUTTON_LEFT);
        mapper.update(snap, stack, snap.sample_dt);
        assert(mapper.triggered(Actions::Fire));
    }

    // Release
    {
        auto snap = makeBlankSnapshot();
        releaseMouse(snap, EMouseButton::MOUSE_BUTTON_LEFT);
        mapper.update(snap, stack, snap.sample_dt);
        assert(!mapper.triggered(Actions::Fire));
        assert(mapper.state(Actions::Fire).completed());
    }

    TEST_SECTION("ActionMapper mouse button");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  13. ActionMapper — mouse axis                                            //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_mapper_mouse_axis()
{
    ActionMapper mapper;
    auto& reg = mapper.actionRegistry();

    Actions::Look = reg.registerAction({0, "look", EInputValueType::AXIS_2D});

    InputContext ctx("gameplay");
    ctx.actionMap()
        .bindMouseAxis(Actions::Look, MouseAxisInput::EAxis::DELTA_X, InputValue::makeAxis2D(1.f, 0.f))
        .bindMouseAxis(Actions::Look, MouseAxisInput::EAxis::DELTA_Y, InputValue::makeAxis2D(0.f, 1.f));

    InputContextStack stack;
    stack.push(&ctx);

    auto snap = makeBlankSnapshot();
    snap.cursor_dx = 10.0;
    snap.cursor_dy = -5.0;
    mapper.update(snap, stack, snap.sample_dt);

    auto v = mapper.getValue(Actions::Look).as2D();
    assert(near(v.x, 10.f));
    assert(near(v.y, -5.f));

    TEST_SECTION("ActionMapper mouse axis");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  14. ActionMapper — UI capture blocks input                               //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_mapper_ui_capture()
{
    ActionMapper mapper;
    auto& reg = mapper.actionRegistry();

    Actions::Jump = reg.registerAction({0, "jump", EInputValueType::BOOL});
    Actions::Fire = reg.registerAction({0, "fire", EInputValueType::BOOL});

    InputContext ctx("gameplay");
    ctx.actionMap()
        .bindKey(Actions::Jump, EKey::KEY_SPACE, InputValue::makeBool(true))
        .bindMouseButton(Actions::Fire, EMouseButton::MOUSE_BUTTON_LEFT, InputValue::makeBool(true));

    InputContextStack stack;
    stack.push(&ctx);

    // Keyboard captured by UI → Jump should not fire
    {
        auto snap = makeBlankSnapshot();
        pressKey(snap, EKey::KEY_SPACE);
        snap.keyboard_captured_by_ui = true;
        mapper.update(snap, stack, snap.sample_dt, !snap.keyboard_captured_by_ui, !snap.mouse_captured_by_ui);
        assert(!mapper.triggered(Actions::Jump));
    }

    // Mouse captured by UI → Fire should not fire
    {
        auto snap = makeBlankSnapshot();
        pressMouse(snap, EMouseButton::MOUSE_BUTTON_LEFT);
        snap.mouse_captured_by_ui = true;
        mapper.update(snap, stack, snap.sample_dt, !snap.keyboard_captured_by_ui, !snap.mouse_captured_by_ui);
        assert(!mapper.triggered(Actions::Fire));
    }

    TEST_SECTION("ActionMapper UI capture blocks input");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  15. ActionMapper — context consume & priority                            //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_mapper_context_consume()
{
    ActionMapper mapper;
    auto& reg = mapper.actionRegistry();

    Actions::Jump = reg.registerAction({0, "jump", EInputValueType::BOOL});
    Actions::Interact = reg.registerAction({0, "interact", EInputValueType::BOOL});

    // High priority UI context consumes keyboard
    InputContext ui_ctx("ui", /*consumes_kb=*/true, /*consumes_mouse=*/false, /*prio=*/10);
    ui_ctx.actionMap().bindKey(Actions::Interact, EKey::KEY_SPACE, InputValue::makeBool(true));

    // Low priority gameplay context also binds Space
    InputContext gameplay_ctx("gameplay", false, false, 0);
    gameplay_ctx.actionMap().bindKey(Actions::Jump, EKey::KEY_SPACE, InputValue::makeBool(true));

    InputContextStack stack;
    stack.push(&gameplay_ctx);
    stack.push(&ui_ctx);

    auto snap = makeBlankSnapshot();
    pressKey(snap, EKey::KEY_SPACE);
    mapper.update(snap, stack, snap.sample_dt);

    // UI context fires Interact
    assert(mapper.triggered(Actions::Interact));
    // Gameplay context is consumed — Jump should NOT fire
    assert(!mapper.triggered(Actions::Jump));

    TEST_SECTION("ActionMapper context consume & priority");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  16. ActionMapper — disabled context is skipped                           //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_mapper_disabled_context()
{
    ActionMapper mapper;
    auto& reg = mapper.actionRegistry();

    Actions::Jump = reg.registerAction({0, "jump", EInputValueType::BOOL});

    InputContext ctx("gameplay");
    ctx.actionMap().bindKey(Actions::Jump, EKey::KEY_SPACE, InputValue::makeBool(true));

    InputContextStack stack;
    stack.push(&ctx);

    // Disabled context — Jump should not fire
    ctx.setEnabled(false);
    {
        auto snap = makeBlankSnapshot();
        pressKey(snap, EKey::KEY_SPACE);
        mapper.update(snap, stack, snap.sample_dt);
        assert(!mapper.triggered(Actions::Jump));
    }

    // Re-enable
    ctx.setEnabled(true);
    {
        auto snap = makeBlankSnapshot();
        pressKey(snap, EKey::KEY_SPACE);
        mapper.update(snap, stack, snap.sample_dt);
        assert(mapper.triggered(Actions::Jump));
    }

    TEST_SECTION("ActionMapper disabled context skipped");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  17. ActionMapper — injection API                                         //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_mapper_injection()
{
    ActionMapper mapper;
    auto& reg = mapper.actionRegistry();

    Actions::Jump = reg.registerAction({0, "jump", EInputValueType::BOOL});
    Actions::Move = reg.registerAction({0, "move", EInputValueType::AXIS_2D});

    InputContext ctx("gameplay"); // empty — no bindings
    InputContextStack stack;
    stack.push(&ctx);

    // injectTriggered — one-shot
    mapper.injectTriggered(Actions::Jump, InputValue::makeBool(true));
    {
        auto snap = makeBlankSnapshot();
        mapper.update(snap, stack, snap.sample_dt);
        assert(mapper.triggered(Actions::Jump));
        assert(mapper.getValue(Actions::Jump).asBool());
    }

    // Next frame — injection is cleared
    {
        auto snap = makeBlankSnapshot();
        mapper.update(snap, stack, snap.sample_dt);
        assert(!mapper.triggered(Actions::Jump));
    }

    // injectValue — persists
    mapper.injectValue(Actions::Move, InputValue::makeAxis2D(1.f, 0.f));
    {
        auto snap = makeBlankSnapshot();
        mapper.update(snap, stack, snap.sample_dt);
        auto v = mapper.getValue(Actions::Move).as2D();
        assert(near(v.x, 1.f));
        assert(near(v.y, 0.f));
        assert(mapper.active(Actions::Move));
    }

    // Still persists next frame
    {
        auto snap = makeBlankSnapshot();
        mapper.update(snap, stack, snap.sample_dt);
        assert(mapper.active(Actions::Move));
    }

    mapper.injectTriggered(Actions::Jump, InputValue::makeAxis1D(1.f));
    {
        auto snap = makeBlankSnapshot();
        mapper.update(snap, stack, snap.sample_dt);
        assert(mapper.triggered(Actions::Jump));
    }

    TEST_SECTION("ActionMapper injection API");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  18. ActionMapper — action-level modifier (Normalize2D)                   //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_mapper_action_modifier()
{
    ActionMapper mapper;
    auto& reg = mapper.actionRegistry();

    InputActionDesc move_desc;
    move_desc.name = "move";
    move_desc.value_type = EInputValueType::AXIS_2D;
    move_desc.action_modifiers = {{EModifierKind::NORMALIZE_2D}};
    Actions::Move = reg.registerAction(move_desc);

    InputContext ctx("gameplay");
    ctx.actionMap()
        .bindKey(Actions::Move, EKey::KEY_W, InputValue::makeAxis2D(0.f, 1.f))
        .bindKey(Actions::Move, EKey::KEY_D, InputValue::makeAxis2D(1.f, 0.f));

    InputContextStack stack;
    stack.push(&ctx);

    // Press W + D → diagonal, then Normalize2D brings to unit length
    auto snap = makeBlankSnapshot();
    pressKey(snap, EKey::KEY_W);
    pressKey(snap, EKey::KEY_D);
    mapper.update(snap, stack, snap.sample_dt);

    auto v = mapper.getValue(Actions::Move).as2D();
    float len = std::sqrt(v.x * v.x + v.y * v.y);
    assert(near(len, 1.0f));
    assert(v.x > 0.f && v.y > 0.f); // both positive

    TEST_SECTION("ActionMapper action-level Normalize2D modifier");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  19. ActionMapper — action-level trigger (Hold)                           //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_mapper_action_trigger()
{
    ActionMapper mapper;
    auto& reg = mapper.actionRegistry();

    InputActionDesc aim_desc;
    aim_desc.name = "aim";
    aim_desc.value_type = EInputValueType::BOOL;
    aim_desc.action_triggers = {{ETriggerKind::HOLD, ETriggerLogicType::EXPLICIT, 0.5f, 0.1f}};
    Actions::Aim = reg.registerAction(aim_desc);

    InputContext ctx("gameplay");
    ctx.actionMap().bindMouseButton(Actions::Aim, EMouseButton::MOUSE_BUTTON_RIGHT, InputValue::makeBool(true));

    InputContextStack stack;
    stack.push(&ctx);

    const float dt = 1.f / 60.f;

    // Frame 1: press right mouse — hold timer starts, Ongoing
    {
        auto snap = makeBlankSnapshot(dt);
        pressMouse(snap, EMouseButton::MOUSE_BUTTON_RIGHT);
        mapper.update(snap, stack, dt);
        assert(mapper.ongoing(Actions::Aim));
        assert(!mapper.triggered(Actions::Aim));
    }

    // Frames 2-5: hold — still Ongoing
    for (int i = 0; i < 4; ++i)
    {
        auto snap = makeBlankSnapshot(dt);
        holdMouse(snap, EMouseButton::MOUSE_BUTTON_RIGHT);
        mapper.update(snap, stack, dt);
    }
    assert(!mapper.triggered(Actions::Aim));

    // Keep holding until hold_time exceeded (0.1s ≈ 6 frames at 60fps)
    for (int i = 0; i < 5; ++i)
    {
        auto snap = makeBlankSnapshot(dt);
        holdMouse(snap, EMouseButton::MOUSE_BUTTON_RIGHT);
        mapper.update(snap, stack, dt);
    }
    // By now elapsed ≈ 10 * dt ≈ 0.167s > 0.1s → should be Triggered
    assert(mapper.triggered(Actions::Aim));

    TEST_SECTION("ActionMapper action-level Hold trigger");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  20. ActionMapper — binding-level trigger (Pressed edge)                  //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_mapper_binding_trigger()
{
    ActionMapper mapper;
    auto& reg = mapper.actionRegistry();

    Actions::Jump = reg.registerAction({0, "jump", EInputValueType::BOOL});

    InputContext ctx("gameplay");
    // Binding-level Pressed trigger: only fires on the rising edge
    std::vector<TriggerDesc> pressed_trigger = {{ETriggerKind::PRESSED, ETriggerLogicType::EXPLICIT, 0.5f}};
    ctx.actionMap().bindKey(Actions::Jump, EKey::KEY_SPACE, InputValue::makeBool(true), {}, pressed_trigger);

    InputContextStack stack;
    stack.push(&ctx);

    // Frame 1: press → fires (rising edge)
    {
        auto snap = makeBlankSnapshot();
        pressKey(snap, EKey::KEY_SPACE);
        mapper.update(snap, stack, snap.sample_dt);
        assert(mapper.triggered(Actions::Jump));
    }

    // Frame 2: hold → does NOT fire (no edge)
    {
        auto snap = makeBlankSnapshot();
        holdKey(snap, EKey::KEY_SPACE);
        mapper.update(snap, stack, snap.sample_dt);
        assert(!mapper.triggered(Actions::Jump));
    }

    // Frame 3: release → does NOT fire (Pressed is rising-edge only)
    {
        auto snap = makeBlankSnapshot();
        releaseKey(snap, EKey::KEY_SPACE);
        mapper.update(snap, stack, snap.sample_dt);
        assert(!mapper.triggered(Actions::Jump));
    }

    // Frame 4: press again → fires again
    {
        auto snap = makeBlankSnapshot();
        pressKey(snap, EKey::KEY_SPACE);
        mapper.update(snap, stack, snap.sample_dt);
        assert(mapper.triggered(Actions::Jump));
    }

    TEST_SECTION("ActionMapper binding-level Pressed trigger");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  21. ActionMapper — multi-frame held_seconds tracking                     //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_mapper_held_seconds()
{
    ActionMapper mapper;
    auto& reg = mapper.actionRegistry();

    Actions::Sprint = reg.registerAction({0, "sprint", EInputValueType::BOOL});

    InputContext ctx("gameplay");
    ctx.actionMap().bindKey(Actions::Sprint, EKey::KEY_LEFT_SHIFT, InputValue::makeBool(true));

    InputContextStack stack;
    stack.push(&ctx);

    const float dt = 1.f / 60.f;

    // Press and hold for 10 frames
    {
        auto snap = makeBlankSnapshot(dt);
        pressKey(snap, EKey::KEY_LEFT_SHIFT);
        mapper.update(snap, stack, dt);
    }
    for (int i = 0; i < 9; ++i)
    {
        auto snap = makeBlankSnapshot(dt);
        holdKey(snap, EKey::KEY_LEFT_SHIFT);
        mapper.update(snap, stack, dt);
    }

    float held = mapper.state(Actions::Sprint).held_seconds;
    // Expect ~10 * dt ≈ 0.167s
    assert(held > 0.1f && held < 0.25f);

    // Release — held_seconds should reset
    {
        auto snap = makeBlankSnapshot(dt);
        releaseKey(snap, EKey::KEY_LEFT_SHIFT);
        mapper.update(snap, stack, dt);
    }
    // After one more idle frame
    {
        auto snap = makeBlankSnapshot(dt);
        mapper.update(snap, stack, dt);
    }
    assert(near(mapper.state(Actions::Sprint).held_seconds, 0.f));

    TEST_SECTION("ActionMapper held_seconds tracking");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  22. IActionDispatcher                                                    //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_action_dispatcher()
{
    ActionMapper mapper;
    auto& reg = mapper.actionRegistry();
    Actions::Jump = reg.registerAction({0, "jump", EInputValueType::BOOL});
    Actions::Move = reg.registerAction({0, "move", EInputValueType::AXIS_2D});

    ActionMapperDispatcher dispatcher(mapper);

    InputContext ctx("empty");
    InputContextStack stack;
    stack.push(&ctx);

    // dispatchTriggered → injectTriggered
    dispatcher.dispatchTriggered(Actions::Jump, InputValue::makeBool(true));
    {
        auto snap = makeBlankSnapshot();
        mapper.update(snap, stack, snap.sample_dt);
        assert(mapper.triggered(Actions::Jump));
    }

    // dispatchValue → injectValue (2D preserved, not squashed to float)
    dispatcher.dispatchValue(Actions::Move, InputValue::makeAxis2D(0.5f, -0.3f));
    {
        auto snap = makeBlankSnapshot();
        mapper.update(snap, stack, snap.sample_dt);
        auto v = mapper.getValue(Actions::Move).as2D();
        assert(near(v.x, 0.5f));
        assert(near(v.y, -0.3f));
    }

    TEST_SECTION("IActionDispatcher dispatchTriggered + dispatchValue");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  23. ActionMapper — binding with modifiers (NegateY + Scale)              //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_mapper_binding_modifiers()
{
    ActionMapper mapper;
    auto& reg = mapper.actionRegistry();

    Actions::Look = reg.registerAction({0, "look", EInputValueType::AXIS_2D});

    InputContext ctx("gameplay");
    // bindMouseAxis with binding_modifiers: NegateY on vertical axis
    std::vector<ModifierSpec> y_mods = {{EModifierKind::NEGATE_Y}};
    ctx.actionMap()
        .bindMouseAxis(Actions::Look, MouseAxisInput::EAxis::DELTA_X, InputValue::makeAxis2D(1.f, 0.f))
        .bindMouseAxis(Actions::Look, MouseAxisInput::EAxis::DELTA_Y, InputValue::makeAxis2D(0.f, 1.f), 1.f, y_mods);

    InputContextStack stack;
    stack.push(&ctx);

    auto snap = makeBlankSnapshot();
    snap.cursor_dx = 10.0;
    snap.cursor_dy = 5.0;
    mapper.update(snap, stack, snap.sample_dt);

    auto v = mapper.getValue(Actions::Look).as2D();
    assert(near(v.x, 10.f));
    assert(near(v.y, -5.f)); // negated by NegateY modifier

    TEST_SECTION("ActionMapper binding modifiers (NegateY)");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  24. ActionMapper — scroll wheel axis                                     //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_mapper_scroll_axis()
{
    ActionMapper mapper;
    auto& reg = mapper.actionRegistry();

    // Use a 1D action for scroll
    ActionId Zoom = reg.registerAction({0, "zoom", EInputValueType::AXIS_1D});

    InputContext ctx("gameplay");
    ctx.actionMap().bindMouseAxis(Zoom, MouseAxisInput::EAxis::SCROLL_Y, InputValue::makeAxis1D(1.f));

    InputContextStack stack;
    stack.push(&ctx);

    auto snap = makeBlankSnapshot();
    snap.scroll_dy = 3.0;
    mapper.update(snap, stack, snap.sample_dt);

    assert(near(mapper.getValue(Zoom).as1D(), 3.f));
    assert(mapper.active(Zoom));

    TEST_SECTION("ActionMapper scroll wheel axis");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  25. ActionMapper — multiple contexts, same action, lower blocked         //
// ═══════════════════════════════════════════════════════════════════════════ //

static void
test_mapper_multi_context_mouse_consume()
{
    ActionMapper mapper;
    auto& reg = mapper.actionRegistry();

    ActionId UIClick = reg.registerAction({0, "ui_click", EInputValueType::BOOL});
    ActionId GameShoot = reg.registerAction({0, "game_shoot", EInputValueType::BOOL});

    InputContext ui_ctx("ui", false, true, 10); // consumes mouse
    ui_ctx.actionMap().bindMouseButton(UIClick, EMouseButton::MOUSE_BUTTON_LEFT, InputValue::makeBool(true));

    InputContext game_ctx("game", false, false, 0);
    game_ctx.actionMap().bindMouseButton(GameShoot, EMouseButton::MOUSE_BUTTON_LEFT, InputValue::makeBool(true));

    InputContextStack stack;
    stack.push(&game_ctx);
    stack.push(&ui_ctx);

    auto snap = makeBlankSnapshot();
    pressMouse(snap, EMouseButton::MOUSE_BUTTON_LEFT);
    mapper.update(snap, stack, snap.sample_dt);

    assert(mapper.triggered(UIClick));
    assert(!mapper.triggered(GameShoot)); // consumed by UI context

    TEST_SECTION("ActionMapper multi-context mouse consume");
}

static void
test_input_ownership_and_synthetic_evaluation()
{
    Input input;
    assert(&input.actionRegistry() == &input.mapper().actionRegistry());

    const ActionId action = input.actionRegistry().registerAction({0, "input.synthetic", EInputValueType::BOOL});
    InputContext context("synthetic");
    context.actionMap().bindKey(action, EKey::KEY_SPACE, InputValue::makeBool(true));
    input.contexts().push(&context);

    auto snapshot = makeBlankSnapshot();
    pressKey(snapshot, EKey::KEY_SPACE);
    input.evaluate(snapshot, snapshot.sample_dt);

    assert(input.mapper().triggered(action));
    assert(input.snapshot().isKeyJustPressed(EKey::KEY_SPACE));
    assert(!input.snapshot().isKeyHeld(EKey::UNKNOWN));

    Input empty;
    empty.evaluate(1.0f / 60.0f);

    TEST_SECTION("Input ownership + synthetic evaluation");
}

// ═══════════════════════════════════════════════════════════════════════════ //
//  Main                                                                     //
// ═══════════════════════════════════════════════════════════════════════════ //

int
main()
{
    std::printf("====== Input System Test ======\n");

    test_action_id();
    test_input_value();
    test_input_modifier();
    test_trigger_evaluator();
    test_trigger_group();
    test_binding_id_allocator();
    test_action_map_builder();
    test_action_registry();
    test_context_and_stack();
    test_mapper_basic_key();
    test_mapper_wasd_accumulation();
    test_mapper_mouse_button();
    test_mapper_mouse_axis();
    test_mapper_ui_capture();
    test_mapper_context_consume();
    test_mapper_disabled_context();
    test_mapper_injection();
    test_mapper_action_modifier();
    test_mapper_action_trigger();
    test_mapper_binding_trigger();
    test_mapper_held_seconds();
    test_action_dispatcher();
    test_mapper_binding_modifiers();
    test_mapper_scroll_axis();
    test_mapper_multi_context_mouse_consume();
    test_input_ownership_and_synthetic_evaluation();

    std::printf("==============================\n");
    std::printf("All %d tests passed.\n", tests_passed);
    return 0;
}
