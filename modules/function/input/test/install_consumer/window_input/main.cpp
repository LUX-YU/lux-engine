#include <lux/engine/input/Input.hpp>
#include <lux/engine/window/LuxWindow.hpp>

void
samplePlatformInput(lux::input::Input& input, lux::window::LuxWindow& window)
{
    input.sample(window);
    input.evaluate(1.0f / 60.0f);
}

int
main()
{
    return 0;
}
