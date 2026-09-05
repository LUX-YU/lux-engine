#pragma once

#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/scripting/ScriptBackend.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptCoroutine.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>

namespace lux::simulation::test
{
struct LUX_TYPE_INFO(compile_time) BridgeRecord final
{
    std::int32_t value{};
};

inline ecs::Entity observed_self{ecs::NullEntity};
inline float observed_value{};
inline std::int32_t observed_record{};
inline std::int32_t observed_lifecycle_value{};
inline lux::simulation::script::EScriptEndPlayReason observed_end_reason{};
inline std::size_t constructed_objects{};
inline std::size_t destroyed_objects{};
inline std::int32_t coroutine_value{};
inline std::int32_t coroutine_event_value{};
inline std::uintptr_t aligned_local_before{};
inline std::uintptr_t aligned_local_after{};
inline std::int32_t aligned_local_value{};
inline std::optional<script::CppScriptEventSource<std::int32_t>> coroutine_event_source;

inline constexpr lux::script::ScriptEventSourceView BridgeEvent{
    "Gameplay",
    "damage",
    0x601U,
    0x602U,
    lux::script::EScriptEventRoute::SIMULATION_BROADCAST,
    {lux::semantic::typeId("lux.i32"), "lux.i32", LUX_SCRIPT_VK_INT32, sizeof(std::int32_t), alignof(std::int32_t)},
    0x603U,
    1U,
    0x604U,
    0x605U,
    1U};

class LUX_TYPE_INFO(compile_time) BridgeBehavior final
{
  public:
    BridgeBehavior() noexcept
    {
        ++constructed_objects;
    }

    ~BridgeBehavior() noexcept
    {
        ++destroyed_objects;
    }

    LUX_METHOD(script_export = "bridge.begin", script_lifecycle = begin_play)
    void admitToGameplay() noexcept
    {
        lifecycle_value_ = 10;
    }

    LUX_METHOD(script_export = "bridge.value")
    void onValue(float value) noexcept
    {
        observed_value = value;
        ++lifecycle_value_;
    }

    LUX_METHOD(script_export = "bridge.record")
    void onRecord(const BridgeRecord &value) noexcept
    {
        observed_record = value.value;
    }

    LUX_METHOD(script_export = "bridge.end", script_lifecycle = end_play)
    void leaveGameplay(lux::simulation::script::EScriptEndPlayReason reason) noexcept
    {
        observed_lifecycle_value = lifecycle_value_;
        observed_end_reason = reason;
    }

    LUX_METHOD()
    void throwing(float) {}

    LUX_METHOD(script_export = "bridge.task", script_coroutine = true)
    script::ScriptCoroutine task(script::ScriptCoroutineContext &context, const std::int32_t &borrowed_value) noexcept
    {
        alignas(64) volatile std::int32_t owned_local[16]{};
        owned_local[0] = borrowed_value;
        aligned_local_before = reinterpret_cast<std::uintptr_t>(&owned_local[0]);
        coroutine_value += borrowed_value;
        co_await context.delay().nextStep();
        aligned_local_after = reinterpret_cast<std::uintptr_t>(&owned_local[0]);
        aligned_local_value = owned_local[0];
        coroutine_value += borrowed_value + 5;
    }

    LUX_METHOD(script_export = "bridge.event", script_coroutine = true)
    script::ScriptCoroutine waitForEvent(script::ScriptCoroutineContext &context) noexcept
    {
        if (!coroutine_event_source)
            co_return;
        coroutine_event_value = co_await context.wait(*coroutine_event_source);
    }

    LUX_METHOD(script_export = "bridge.next", script_coroutine = true)
    script::ScriptCoroutine waitForNextStep(script::ScriptCoroutineContext &context) noexcept
    {
        co_await context.delay().nextStep();
        coroutine_value += 100;
    }

    LUX_METHOD(script_export = "bridge.twice", script_coroutine = true)
    script::ScriptCoroutine waitTwice(script::ScriptCoroutineContext &context) noexcept
    {
        co_await context.delay().nextStep();
        coroutine_value += 1'000;
        co_await context.delay().nextStep();
        coroutine_value += 10'000;
    }

    void unmarkedHelper() noexcept {}

  private:
    std::int32_t lifecycle_value_{};
};

inline std::int32_t LUX_FUNC(script_export = "bridge.free") bridgeFreeFunction(std::int32_t value) noexcept
{
    return value + 1;
}
} // namespace lux::simulation::test

namespace lux::semantic
{
template <> struct TypeTraits<lux::simulation::test::BridgeRecord> final
{
    inline static constexpr std::string_view CanonicalName{"lux.test.BridgeRecord"};
    inline static constexpr std::uint8_t AbiKind{LUX_SCRIPT_VK_STRUCT_REF};
    inline static constexpr std::uint32_t Size{sizeof(lux::simulation::test::BridgeRecord)};
    inline static constexpr std::uint32_t Alignment{alignof(lux::simulation::test::BridgeRecord)};
};
} // namespace lux::semantic
