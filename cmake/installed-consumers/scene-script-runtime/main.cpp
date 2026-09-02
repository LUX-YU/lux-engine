#include <lux/engine/scene/ScriptRuntimeSystem.hpp>

int main()
{
    auto timer = lux::process::TimerQueue::create({2U});
    if (!timer)
        return 1;
    auto provider = lux::scene::ScriptRealDelayProvider::create(timer->client(), 2U);
    if (!provider || !(*provider)->endpoint())
        return 2;
    (*provider)->requestStop();
    if (!(*provider)->join())
        return 3;
    timer->requestStop();

    const auto registration = lux::scene::builtinScriptRuntimeSystemRegistration();
    return registration.description == &lux::scene::ScriptRuntimeSystem::Description ? 0 : 4;
}
