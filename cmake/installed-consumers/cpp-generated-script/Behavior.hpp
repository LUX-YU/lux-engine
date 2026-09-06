#pragma once
#include <lux/engine/meta/MetaAnnotations.hpp>
#include <lux/engine/simulation/scripting/ScriptBackend.hpp>
#include <lux/engine/simulation/scripting/ScriptLifecycle.hpp>
#include <lux/engine/simulation/scripting/cpp_static/ScriptDelayCoroutine.hpp>
#include <array>

namespace installed_generated
{
    inline unsigned begins{}, ends{}, destroys{};
    inline std::int32_t observed{};
    inline lux::simulation::script::EScriptEndPlayReason last_reason{};
    struct Observation final
    {
        lux::simulation::ecs::Entity self{lux::simulation::ecs::NullEntity};
        unsigned attaches{}, begins{}, calls{}, resumes{}, ends{}, destroys{};
    };
    inline std::array<Observation, 8U> observations;
    inline unsigned constructs{}, attach_errors{};
    inline bool fail_construction{};

    class LUX_TYPE_INFO(compile_time) Behavior final
    {
    public:
        Behavior()
        {
            if (fail_construction) throw 7;
            observation_ = constructs++;
        }
        ~Behavior() noexcept { ++destroys; ++observations[observation_].destroys; }
        LUX_METHOD(script_attach=true)
        void bindHost(lux::simulation::script::ScriptBehavior& host) noexcept
        {
            host_ = &host;
            observations[observation_].self = host.self();
            ++observations[observation_].attaches;
        }
        LUX_METHOD(script_export="behavior.begin", script_lifecycle=begin_play)
        void initialize() noexcept { checkHost(); value_ = 10; ++begins; ++observations[observation_].begins; }
        LUX_METHOD(script_export="behavior.update")
        void update(const std::int32_t&) noexcept
        {
            checkHost();
            observed = ++value_;
            ++observations[observation_].calls;
        }
        LUX_METHOD(script_export="behavior.task", script_coroutine=true)
        lux::simulation::script::ScriptCoroutine task(
            lux::simulation::script::ScriptCoroutineContext& context, const std::int32_t& input) noexcept
        {
            checkHost();
            value_ += input;
            observed = value_;
            co_await context.delay().nextStep();
            checkHost();
            ++observations[observation_].resumes;
            value_ += input;
            observed = value_;
        }
        LUX_METHOD(script_export="behavior.end", script_lifecycle=end_play)
        void retire(lux::simulation::script::EScriptEndPlayReason reason) noexcept
        {
            checkHost();
            last_reason = reason;
            observed = value_;
            ++ends;
            ++observations[observation_].ends;
        }
    private:
        void checkHost() noexcept
        {
            if (host_ == nullptr || !host_->isAttached() || host_->self() != observations[observation_].self)
                ++attach_errors;
        }
        lux::simulation::script::ScriptBehavior* host_{};
        unsigned observation_{};
        std::int32_t value_{};
    };
}
