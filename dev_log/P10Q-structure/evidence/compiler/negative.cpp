#include <lux/engine/editor/scene/SceneSaveSource.hpp>
template<class T> struct Policy { static constexpr auto bytes = sizeof(T); };
auto instantiated = Policy<lux::editor::scene::SceneSaveSource>::bytes;
