#pragma once

#include <cstddef>
#include <lux/engine/ui/Element.hpp>
#include <vector>

namespace lux::editor
{
    // Editor-owned typed draft. Changes invalidate cold qualification; serialization never touches a live Scene.
    class SceneConfigurationForm : public ui::Element
    {
    public:
        object::TSignal<> edited{*this};
        [[nodiscard]] virtual bool serialize(std::vector<std::byte>& output) const noexcept = 0;
    };
} // namespace lux::editor
