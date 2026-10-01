#pragma once

#include <array>
#include <cstdint>
#include <limits>
#include <exception>
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
        [[nodiscard]] std::size_t size() const noexcept
        {
            return originals_.size();
        }
        [[nodiscard]] bool hasNode(std::uint64_t id) const
        {
            return assigned_[0].contains(id);
        }
        [[nodiscard]] bool hasPin(std::uint64_t id) const
        {
            return assigned_[1].contains(id);
        }
        [[nodiscard]] bool hasLink(std::uint64_t id) const
        {
            return assigned_[2].contains(id);
        }
        // Only after the owning backend context and all its gestures have been retired. Keep a
        // numeric high-water mark so even an accidentally retained old UI ID cannot name a new item.
        void retire() noexcept
        {
            first_ += originals_.size();
            decltype(assigned_){}.swap(assigned_);
            decltype(originals_){}.swap(originals_);
        }
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
            if (const auto found = ids.find(value); found != ids.end())
                return found->second;
            if (originals_.size() == std::numeric_limits<std::uintptr_t>::max() - first_)
                std::terminate(); // Exhaustion never recycles a former UI identity.
            const auto [entry, inserted] = ids.try_emplace(value, first_ + originals_.size());
            if (inserted)
            {
                originals_.push_back({kind, value});
            }
            return entry->second;
        }

        std::uint64_t lookup(EKind kind, std::uintptr_t value) const noexcept
        {
            if (value < first_ || value - first_ >= originals_.size())
            {
                return 0;
            }
            const auto& original = originals_[value - first_];
            return original.kind == kind ? original.value : 0;
        }

        std::array<std::unordered_map<std::uint64_t, std::uintptr_t>, 3> assigned_;
        std::vector<Source> originals_;
        std::uintptr_t first_{1};
    };
} // namespace lux::editor::widgets
