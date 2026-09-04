#include "ConsumerBehavior.hpp"

#include <lux/engine/function/script/artifact/ScriptArtifact.hpp>
#include <lux/engine/meta/Meta.hpp>
#include <lux/engine/simulation/scripting/cpp_static/CppStaticScriptBridge.hpp>

#include <array>
#include <cassert>
#include <optional>

namespace
{
    using namespace lux::simulation::script;

    lux::cxx::expected<ScriptAwaitableRegistration, EScriptAwaitableCreateError> createAwaitable(
        void*,
        ScriptInstanceId,
        std::optional<lux::rdesc::ScriptValueType>
    ) noexcept
    {
        return ScriptAwaitableRegistration{{1U, 1U}, {}};
    }

    void discardAwaitable(void*, ScriptInstanceId, ScriptAwaitableId) noexcept
    {
    }
}

int main()
{
    using namespace lux::simulation::script;
    lux::meta::ReflectionRegistry::initRegistry();
    auto& registry = lux::meta::ReflectionRegistry::instance();
    const auto* reflected = registry.findClass("installed_consumer::CoroutineBehavior");
    assert(reflected && reflected->methods.size() == 1U);
    const auto& reflected_method = reflected->methods.front();
    const auto coroutine = makeCppStaticCoroutineExport<
        &installed_consumer::CoroutineBehavior::run
    >(reflected_method, lux::script::ScriptSymbolId{1U});
    const std::array coroutines{coroutine};
    const std::array<const lux::meta::RefMethod*, 0U> methods{};
    const std::array<lux::script::ScriptSymbolId, 0U> symbols{};
    auto projected = projectCppStaticEntityScript(
        "installed.cpp-coroutine",
        "installed-cpp-coroutine-v1",
        *reflected,
        methods,
        symbols,
        {},
        nullptr,
        {},
        coroutines
    );
    assert(projected);
    auto artifact = lux::script::ScriptArtifact::create(projected->description(), {});
    assert(artifact);
    const std::array pools{CppStaticScriptPoolDescription{
        std::addressof(*projected),
        1U,
        1U,
        1024U,
        alignof(std::max_align_t),
        1U
    }};
    auto backend = CppStaticScriptBackend::create(pools);
    assert(backend);
    auto descriptor = backend->descriptor();
    ScriptBehavior behavior;
    ScriptBackendInstance instance;
    assert(descriptor.createInstance(
        descriptor.context,
        {{}, EntityScriptScope{lux::simulation::ecs::Entity{1U}}, &behavior, {1U, 1U}},
        *artifact,
        instance
    ) == EScriptBackendResult::SUCCESS);
    ScriptBackendPreparedMethod method;
    assert(descriptor.prepareMethod(
        descriptor.context,
        instance,
        artifact->description().exports.front(),
        method
    ) == EScriptBackendResult::SUCCESS);
    const auto call = method.resumable;
    lux_script_call_frame frame{};
    ScriptStepContext step{{1U, 1U}, nullptr, &createAwaitable, &discardAwaitable};
    ScriptBackendContinuation continuation;
    const auto suspended = call.invoke(call.context, frame, step, continuation);
    assert(suspended.state == EScriptStepState::SUSPENDED);
    assert(installed_consumer::observed == 1);
    const ScriptResumePacket packet{
        suspended.waiting_on,
        EScriptAwaitableState::READY,
        nullptr,
        {}
    };
    const auto completed = continuation.resume(continuation.state, step, packet);
    assert(completed.state == EScriptStepState::COMPLETED);
    assert(installed_consumer::observed == 2);
    continuation.destroy(continuation.state);
    descriptor.releaseMethod(descriptor.context, instance, method);
    descriptor.destroyInstance(descriptor.context, instance);
    assert(backend->stats().active_frames == 0U);
    return 0;
}
