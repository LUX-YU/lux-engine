#include <lux/engine/simulation/HookPoint.hpp>

int main()
{
    lux::simulation::HookPoint<void()> hook;
    return static_cast<int>(hook.dispatch());
}
