#include <lux/engine/input/Input.hpp>

int
main()
{
    lux::input::Input input;
    input.evaluate(lux::input::InputSnapshot{}, 1.0f / 60.0f);
    return &input.actionRegistry() == &input.mapper().actionRegistry() ? 0 : 1;
}
