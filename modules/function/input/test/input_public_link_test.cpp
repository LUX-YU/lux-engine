#include <lux/engine/input/Input.hpp>

#include <cassert>

int
main()
{
    lux::input::Input input;
    assert(&input.actionRegistry() == &input.mapper().actionRegistry());
    input.evaluate(lux::input::InputSnapshot{}, 1.0f / 60.0f);
    return 0;
}
