#pragma once
#include <vector>
#include <cstdint>
#include <lux/engine/ui/Attachment.hpp>
namespace lux::ui
{
    class Root;
    class Pane;
    class Element;
    namespace detail
    {
        struct AttachmentNode final
        {
            Pane* pane{};
            Element* element{};
        };
        // Root and candidate only observe this record. The preparation is its sole owner.
        struct AttachmentState final
        {
            Root* root{};
            std::vector<Pane*> roots;
            std::vector<AttachmentNode> nodes;
            std::vector<WindowVisibility> visibility;
            std::vector<Pane*> visibility_changed;
            std::uint64_t revision{}, window_revision{};
            bool mount{}, valid{true};
        };
    }
}
