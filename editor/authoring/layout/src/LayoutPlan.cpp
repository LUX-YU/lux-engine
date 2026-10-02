#include <lux/engine/editor/workspace/LayoutPlan.hpp>
#include <lux/engine/editor/workspace/RecoveryManifest.hpp>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace lux::editor::workspace
{
    namespace
    {
        auto failed(EWorkspaceError code, std::string detail)
        {
            return lux::cxx::unexpected(WorkspaceFailure{code, std::move(detail)});
        }
        bool text(std::string_view value, WorkspaceLimits limits)
        {
            return !value.empty() && value.size() <= limits.text_bytes && value.find('\0') == value.npos;
        }
        WorkspaceResult<std::size_t> validateOpaque(
            std::span<const PreservedOpaqueState> values,
            const std::optional<LegacyOrigin>& origin,
            WorkspaceLimits limits
        )
        {
            if (values.size() > limits.entries)
                return failed(EWorkspaceError::CAPACITY, "opaque entry count");
            std::size_t total{};
            for (const auto& value : values)
            {
                if (!text(value.type, limits) || !value.schema)
                    return failed(EWorkspaceError::INVALID_DATA, "opaque identity");
                if (value.bytes.size() > limits.opaque_bytes - total)
                    return failed(EWorkspaceError::CAPACITY, "opaque bytes");
                total += value.bytes.size();
            }
            if (origin && (!text(origin->key, limits) || !text(origin->digest, limits)))
                return failed(EWorkspaceError::INVALID_DATA, "migration origin");
            return total;
        }
        using ViewKey = std::pair<std::string, std::string>;
        ViewKey key(const views::ViewRestoreKey& restore, const views::ViewTypeId& type)
        {
            return {std::string(restore.name()), std::string(type.name())};
        }
    }
    bool LayoutId::valid() const noexcept
    {
        return value.size() == 32 &&
               std::ranges::all_of(value, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
    }
    WorkspaceResult<ValidatedLayout> ValidatedLayout::validate(DockLayout value, WorkspaceLimits limits)
    {
        if (value.schema != 1)
            return failed(EWorkspaceError::UNSUPPORTED_VERSION, "layout schema");
        if (!value.id.valid() || !text(value.label, limits))
            return failed(EWorkspaceError::INVALID_DATA, "layout identity/label");
        const bool over_limit = value.slots.size() > limits.entries || value.dock.nodes.size() > limits.entries ||
                                value.dock.roots.size() > limits.entries;
        if (over_limit)
            return failed(EWorkspaceError::CAPACITY, "layout count");
        auto payload_bytes = validateOpaque(value.opaque, value.legacy_origin, limits);
        if (!payload_bytes)
            return lux::cxx::unexpected(payload_bytes.error());
        std::map<std::uint32_t, bool> slots;
        std::set<ViewKey> identities;
        for (const auto& slot : value.slots)
        {
            const bool bad_identity = !slot.id.value || !text(slot.type.name(), limits) ||
                                      !text(slot.restore_key.name(), limits) || !slot.state.schema;
            if (bad_identity || !slots.emplace(slot.id.value, false).second ||
                !identities.insert(key(slot.restore_key, slot.type)).second)
                return failed(EWorkspaceError::INVALID_DATA, "duplicate/invalid slot");
            if (slot.state.bytes.size() > limits.opaque_bytes - *payload_bytes)
                return failed(EWorkspaceError::CAPACITY, "view payload bytes");
            *payload_bytes += slot.state.bytes.size();
        }
        std::map<std::uint32_t, const DockNode*> nodes;
        for (const auto& node : value.dock.nodes)
        {
            if (!node.id || !nodes.emplace(node.id, &node).second)
                return failed(EWorkspaceError::INVALID_DATA, "duplicate/invalid dock node");
            const bool is_leaf = node.split == EDockSplit::LEAF;
            const bool invalid_split = node.split != EDockSplit::HORIZONTAL && node.split != EDockSplit::VERTICAL;
            const bool invalid_ratio = !std::isfinite(node.ratio) || node.ratio <= 0 || node.ratio >= 1;
            if (is_leaf ? (node.first || node.second)
                        : (invalid_split || invalid_ratio || !node.first || !node.second || !node.slots.empty()))
                return failed(EWorkspaceError::INVALID_DATA, "dock split");
            for (auto slot : node.slots)
            {
                auto found = slots.find(slot.value);
                if (found == slots.end() || found->second)
                    return failed(EWorkspaceError::INVALID_DATA, "dock slot reference");
                found->second = true;
            }
        }
        // A rooted tree can visit each identity exactly once. The iterative walk also bounds stack memory.
        std::set<std::uint32_t> visited;
        std::vector<std::pair<std::uint32_t, std::size_t>> pending;
        for (const auto& root : value.dock.roots)
        {
            const bool finite = std::isfinite(root.x) && std::isfinite(root.y) && std::isfinite(root.width) &&
                                std::isfinite(root.height);
            if (!finite || root.width <= 0 || root.height <= 0)
                return failed(EWorkspaceError::INVALID_DATA, "root geometry");
            pending.emplace_back(root.node, 1);
        }
        while (!pending.empty())
        {
            const auto [id, depth] = pending.back();
            pending.pop_back();
            if (depth > limits.depth)
                return failed(EWorkspaceError::CAPACITY, "dock depth");
            const auto found = nodes.find(id);
            if (found == nodes.end() || !visited.insert(id).second)
                return failed(EWorkspaceError::INVALID_DATA, "dock missing/cyclic/shared node");
            const auto& node = *found->second;
            if (node.split != EDockSplit::LEAF)
            {
                pending.emplace_back(node.first, depth + 1);
                pending.emplace_back(node.second, depth + 1);
            }
        }
        const bool unreferenced_slot = std::ranges::any_of(slots, [](const auto& slot) { return !slot.second; });
        if (visited.size() != nodes.size() || unreferenced_slot)
            return failed(EWorkspaceError::INVALID_DATA, "unreachable dock/slot");
        return ValidatedLayout(std::move(value));
    }
    WorkspaceResult<LayoutPlan> LayoutPlanner::resolve(
        const ValidatedLayout& input,
        std::span<const views::ViewInfo> existing,
        std::span<const ViewProviderInfo> providers,
        WorkspaceLimits limits
    )
    {
        if (existing.size() > limits.entries || providers.size() > limits.entries)
            return failed(EWorkspaceError::CAPACITY, "planner inputs");
        std::map<ViewKey, std::size_t> identities;
        std::set<std::tuple<std::uint64_t, std::uint32_t, std::uint64_t>> live_ids;
        for (std::size_t i = 0; i < existing.size(); ++i)
        {
            const auto& view = existing[i];
            const bool valid_identity =
                view.id.valid() && text(view.type.name(), limits) && text(view.restore_key.name(), limits);
            if (!valid_identity || !identities.emplace(key(view.restore_key, view.type), i).second ||
                !live_ids.emplace(view.id.domain, view.id.slot, view.id.generation).second)
                return failed(EWorkspaceError::INVALID_DATA, "ambiguous view snapshot");
        }
        std::map<std::string, ViewProviderInfo> registered;
        for (const auto& provider : providers)
        {
            const bool valid = text(provider.type.name(), limits) && provider.first_schema &&
                               provider.last_schema >= provider.first_schema;
            if (!valid || !registered.emplace(std::string(provider.type.name()), provider).second)
                return failed(EWorkspaceError::INVALID_DATA, "provider descriptor");
        }
        LayoutPlan plan{input.value()};
        std::vector<bool> used(existing.size());
        for (const auto& slot : input.value().slots)
        {
            PlannedView view{slot};
            if (const auto found = identities.find(key(slot.restore_key, slot.type)); found != identities.end())
            {
                view.existing = existing[found->second].id;
                used[found->second] = true;
            }
            if (const auto provider = registered.find(std::string(slot.type.name())); provider != registered.end())
            {
                const bool supported = slot.state.schema >= provider->second.first_schema &&
                                       slot.state.schema <= provider->second.last_schema;
                view.resolution = supported
                                      ? (view.existing ? ELayoutResolution::REUSE : ELayoutResolution::CREATE_UNBOUND)
                                      : ELayoutResolution::UNSUPPORTED_STATE;
            }
            plan.views.push_back(std::move(view));
        }
        for (std::size_t i = 0; i < existing.size(); ++i)
            if (!used[i])
                plan.retained.push_back(existing[i]);
        return plan;
    }
    WorkspaceResult<void> validatePreferences(const UserPreferences& value, WorkspaceLimits limits)
    {
        if (value.schema != 1)
            return failed(EWorkspaceError::UNSUPPORTED_VERSION, "preferences schema");
        if (value.selected_layout && !value.selected_layout->valid())
            return failed(EWorkspaceError::INVALID_DATA, "selected layout");
        auto opaque = validateOpaque(value.opaque, value.legacy_origin, limits);
        if (!opaque)
            return lux::cxx::unexpected(opaque.error());
        return {};
    }
    WorkspaceResult<void> validateRecovery(const RecoveryManifest& value, WorkspaceLimits limits)
    {
        if (value.schema != 2)
            return failed(EWorkspaceError::UNSUPPORTED_VERSION, "recovery schema");
        if (value.entries.size() > limits.entries)
            return failed(EWorkspaceError::CAPACITY, "recovery count");
        auto opaque = validateOpaque(value.opaque, value.legacy_origin, limits);
        if (!opaque)
            return lux::cxx::unexpected(opaque.error());
        std::set<ViewKey> unique;
        for (const auto& entry : value.entries)
        {
            const bool is_valid_identity = text(entry.restore_key.name(), limits) && text(entry.type.name(), limits);
            const bool is_invalid_primary = entry.primary && *entry.primary >= entry.contents.size();
            if (!is_valid_identity || is_invalid_primary || !unique.insert(key(entry.restore_key, entry.type)).second)
                return failed(EWorkspaceError::INVALID_DATA, "recovery identity/primary");
            if (entry.contents.size() > 64)
                return failed(EWorkspaceError::CAPACITY, "recovery content count");
            std::set<std::string_view> locations;
            for (const auto& content : entry.contents)
                if (!text(content.locator, limits) || !locations.insert(content.locator).second)
                    return failed(EWorkspaceError::INVALID_DATA, "recovery locator");
        }
        return {};
    }
}
