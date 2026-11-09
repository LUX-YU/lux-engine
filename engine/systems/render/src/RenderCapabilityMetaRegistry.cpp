#include <lux/engine/systems/render/RenderCapabilityMetaRegistry.hpp>

#include <algorithm>

namespace lux::engine::render
{
    RenderCapabilityMetaRegistry::Result
    RenderCapabilityMetaRegistry::addCapability(cxx::type_info capability) noexcept
    {
        if (!capability.isValid())
        {
            return cxx::unexpected(lux::render::RenderError{lux::render::kCapabilityDeclaration});
        }
        const auto position = capabilities_.try_emplace(capability.hash(), RenderCapabilityMeta{capability}).first;
        if (position->second.capability.name() != capability.name())
        {
            return cxx::unexpected(lux::render::RenderError{lux::render::kCapabilityConflict});
        }
        return {};
    }

    RenderCapabilityMetaRegistry::Result RenderCapabilityMetaRegistry::addSceneReaction(SceneReaction metadata) noexcept
    {
        const bool duplicate = std::ranges::any_of(scene_reactions_, [&](const auto& entry) noexcept
        {
            return entry.first == metadata.first;
        });
        if (duplicate)
        {
            return cxx::unexpected(lux::render::RenderError{lux::render::kCapabilityConflict});
        }
        scene_reactions_.push_back(std::move(metadata));
        return {};
    }

    const RenderCapabilityMeta* RenderCapabilityMetaRegistry::find(cxx::type_info capability) const noexcept
    {
        const auto found = capabilities_.find(capability.hash());
        return found != capabilities_.end() && found->second.capability == capability ? &found->second : nullptr;
    }

    RenderCapabilityMetaRegistry::Result
    RenderCapabilityMetaRegistry::add(cxx::type_info capability, ReactionMeta metadata) noexcept
    {
        auto position = capabilities_.try_emplace(capability.hash(), RenderCapabilityMeta{capability}).first;
        auto& entry = position->second;
        const bool is_collision = entry.capability != capability;
        const bool is_duplicate = std::ranges::any_of(entry.reactions, [&](const auto& value) noexcept
        {
            return value.type == metadata.type;
        });
        if (is_collision || is_duplicate)
        {
            return cxx::unexpected(lux::render::RenderError{lux::render::kCapabilityConflict});
        }
        entry.reactions.push_back(std::move(metadata));
        return {};
    }

    RenderCapabilityMetaRegistry::Result
    RenderCapabilityMetaRegistry::add(cxx::type_info capability, CollectorMeta metadata) noexcept
    {
        auto position = capabilities_.try_emplace(capability.hash(), RenderCapabilityMeta{capability}).first;
        auto& entry = position->second;
        const bool is_collision = entry.capability != capability;
        const bool is_duplicate = std::ranges::any_of(entry.collectors, [&](const auto& value) noexcept
        {
            return value.type == metadata.type;
        });
        if (is_collision || is_duplicate)
        {
            return cxx::unexpected(lux::render::RenderError{lux::render::kCapabilityConflict});
        }
        entry.collectors.push_back(std::move(metadata));
        return {};
    }
} // namespace lux::engine::render
