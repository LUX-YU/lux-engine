#include <lux/engine/editor/project/ProjectBuilder.hpp>
#include <array>
#include <cassert>

int main()
{
    std::array<std::uint8_t, 16> bytes{};
    bytes.back() = 1;
    auto value = lux::editor::ProjectBuilder(lux::asset::AssetId{bytes}, "Beginner").build();
    assert(value && value->name == "Beginner" && !value->initial_scene);
    assert(!lux::editor::ProjectBuilder({}, "Invalid").build());
}
