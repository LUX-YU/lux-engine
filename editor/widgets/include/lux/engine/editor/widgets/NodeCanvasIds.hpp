#pragma once

#include <array>
#include <cstdint>
#include <imgui_node_editor.h>
#include <unordered_map>
#include <vector>

namespace lux::editor::widgets
{
    // Domain nodes, pins and links have independent identity spaces. Node-editor's
    // hit testing shares one space, so each Pane assigns stable, non-reused local IDs.
    class NodeCanvasIds final
    {
        enum class EKind : unsigned
        {
            NODE,
            PIN,
            LINK
        };

        struct Source final
        {
            EKind kind;
            std::uint64_t value;
        };

    public:
        ax::NodeEditor::NodeId node(std::uint64_t value)
        {
            return ax::NodeEditor::NodeId{assign(EKind::NODE, value)};
        }

        ax::NodeEditor::PinId pin(std::uint64_t value)
        {
            return ax::NodeEditor::PinId{assign(EKind::PIN, value)};
        }

        ax::NodeEditor::LinkId link(std::uint64_t value)
        {
            return ax::NodeEditor::LinkId{assign(EKind::LINK, value)};
        }

        std::uint64_t source(ax::NodeEditor::NodeId value) const noexcept
        {
            return lookup(EKind::NODE, value.Get());
        }

        std::uint64_t source(ax::NodeEditor::PinId value) const noexcept
        {
            return lookup(EKind::PIN, value.Get());
        }

        std::uint64_t source(ax::NodeEditor::LinkId value) const noexcept
        {
            return lookup(EKind::LINK, value.Get());
        }

    private:
        std::uintptr_t assign(EKind kind, std::uint64_t value)
        {
            if (value == 0)
            {
                return 0;
            }
            auto& ids = assigned_[static_cast<unsigned>(kind)];
            const auto [entry, inserted] = ids.try_emplace(value, originals_.size() + 1);
            if (inserted)
            {
                originals_.push_back({kind, value});
            }
            return entry->second;
        }

        std::uint64_t lookup(EKind kind, std::uintptr_t value) const noexcept
        {
            if (value == 0 || value > originals_.size())
            {
                return 0;
            }
            const auto& original = originals_[value - 1];
            return original.kind == kind ? original.value : 0;
        }

        std::array<std::unordered_map<std::uint64_t, std::uintptr_t>, 3> assigned_;
        std::vector<Source> originals_;
    };
} // namespace lux::editor::widgets
