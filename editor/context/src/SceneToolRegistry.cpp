#include <lux/engine/editor/SceneToolRegistry.hpp>
namespace lux::editor
{
    SceneToolRegistry::SceneToolRegistry(std::vector<Entry> entries) noexcept : entries_(std::move(entries)) {}
} // namespace lux::editor
