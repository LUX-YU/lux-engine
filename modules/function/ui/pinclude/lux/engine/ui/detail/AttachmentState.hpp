#pragma once
#include <vector>
#include <cstdint>
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
            Pane* pane{};
            std::vector<AttachmentNode> nodes;
            std::uint64_t revision{};
            bool mount{}, valid{true};
        };
    }
}
