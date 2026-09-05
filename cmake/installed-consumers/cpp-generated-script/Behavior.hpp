#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp>

namespace installed_generated
{
    inline unsigned begins{}, ends{}, destroys{};
    inline std::int32_t observed{};
    inline lux::simulation::script::EScriptEndPlayReason last_reason{};

    class LUX_TYPE_INFO(compile_time) Behavior final
    {
    public:
        ~Behavior() noexcept { ++destroys; }
        LUX_METHOD(script_export="behavior.begin", script_lifecycle=begin_play)
        void initialize() noexcept { value_ = 10; ++begins; }
        LUX_METHOD(script_export="behavior.update")
        void update(const std::int32_t&) noexcept { observed = ++value_; }
        LUX_METHOD(script_export="behavior.task", script_coroutine=true)
        lux::simulation::script::ScriptCoroutine task(
            lux::simulation::script::ScriptCoroutineContext& context, const std::int32_t& input) noexcept
        {
            value_ += input;
            observed = value_;
            co_await context.delay().nextStep();
            value_ += input;
            observed = value_;
        }
        LUX_METHOD(script_export="behavior.end", script_lifecycle=end_play)
        void retire(lux::simulation::script::EScriptEndPlayReason reason) noexcept
        {
            last_reason = reason;
            observed = value_;
            ++ends;
        }
    private:
        std::int32_t value_{};
    };
}
