#include <lux/engine/editor/material/MaterialInteraction.hpp>
#include <algorithm>
#include <exception>

namespace lux::editor::material
{
    MaterialInteraction::MaterialInteraction(
        sessions::TSessionAccess<MaterialSession> access,
        sessions::TSessionKey<MaterialSession> key
    ) noexcept
        : access_(access), key_(key)
    {}
    MaterialInteraction::~MaterialInteraction() noexcept
    {
        if (gesture_ && !cancel())
            std::terminate(); // Destruction inside an admitted callback violates the owner lifetime contract.
    }
    MaterialEditResult<void> MaterialInteraction::begin(std::string label)
    {
        if (gesture_)
            return lux::cxx::unexpected(sessions::ESessionError::BUSY);
        auto info = access_.describe(key_);
        if (!info)
            return lux::cxx::unexpected(info.error());
        if (info->admission != sessions::EEditAdmission::AVAILABLE)
            return lux::cxx::unexpected(sessions::ESessionError::BUSY);
        gesture_.emplace(MaterialEditBatch{info->current, std::move(label), {}});
        return {};
    }
    MaterialEditResult<void> MaterialInteraction::preview(std::vector<VMaterialEdit>& candidate)
    {
        if (!gesture_)
            return lux::cxx::unexpected(sessions::ESessionError::NOT_PREPARED);
        auto info = access_.describe(key_);
        if (!info)
            return lux::cxx::unexpected(info.error());
        if (info->current != gesture_->expected)
            return lux::cxx::unexpected(sessions::ESessionError::STALE_CONTENT);
        auto owner = access_.read(key_);
        if (!owner)
            return lux::cxx::unexpected(owner.error());
        auto read = owner->get().read();
        if (!read)
            return lux::cxx::unexpected(read.error());
        return read->withRead([&](const lux::material::MaterialSource&) -> MaterialEditResult<void> {
            gesture_->edits.swap(candidate);
            candidate.clear(); // Old preview payload and its code are released under the original gate.
            return {};
        });
    }
    MaterialEditResult<MaterialEditReceipt> MaterialInteraction::commit()
    {
        if (!gesture_)
            return lux::cxx::unexpected(sessions::ESessionError::NOT_PREPARED);
        auto info = access_.describe(key_);
        if (!info)
            return lux::cxx::unexpected(info.error());
        if (info->admission != sessions::EEditAdmission::AVAILABLE)
            return lux::cxx::unexpected(sessions::ESessionError::BUSY);
        if (info->current != gesture_->expected)
            return lux::cxx::unexpected(sessions::ESessionError::STALE_CONTENT);
        auto owner = access_.edit(key_);
        if (!owner)
            return lux::cxx::unexpected(owner.error());
        auto batch = std::move(*gesture_);
        gesture_.reset();
        return owner->get().apply(std::move(batch));
    }
    MaterialEditResult<void> MaterialInteraction::cancel()
    {
        if (!gesture_)
            return {};
        auto owner = access_.read(key_);
        if (!owner)
        {
            if (owner.error() != sessions::ESessionError::STALE_SESSION)
                return lux::cxx::unexpected(owner.error());
            // Only a confirmed stale identity can release input without borrowing a live Session gate.
            auto discarded = std::move(*gesture_);
            gesture_.reset();
            return {};
        }
        auto read = owner->get().read();
        if (!read)
            return lux::cxx::unexpected(read.error());
        return read->withRead([&](const lux::material::MaterialSource&) -> MaterialEditResult<void> {
            auto discarded = std::move(*gesture_);
            gesture_.reset(); // Input destruction follows below, while this read admission is still held.
            return {};
        });
    }
    MaterialEditResult<void> MaterialInteraction::select(std::vector<lux::material::NodeId> nodes)
    {
        auto info = access_.describe(key_);
        if (!info)
            return lux::cxx::unexpected(info.error());
        auto owner = access_.read(key_);
        if (!owner)
            return lux::cxx::unexpected(owner.error());
        auto read = owner->get().read();
        if (!read)
            return lux::cxx::unexpected(read.error());
        return read->withRead([&](const lux::material::MaterialSource& source) -> MaterialEditResult<void> {
            for (auto id : nodes)
                if (source.graph.node(id) == nullptr)
                    return lux::cxx::unexpected(sessions::ESessionError::INVALID_ARGUMENT);
            selection_ = std::move(nodes);
            selection_history_ = info->current.state.history;
            return {};
        });
    }
    MaterialEditResult<void> MaterialInteraction::synchronize()
    {
        auto info = access_.describe(key_);
        if (!info && info.error() != sessions::ESessionError::STALE_SESSION)
            return lux::cxx::unexpected(info.error());
        if (!info || (gesture_ && info->current != gesture_->expected))
        {
            auto cancelled = cancel();
            if (!cancelled)
                return cancelled;
        }
        if (!info || info->current.state.history != selection_history_)
        {
            selection_.clear();
            return {};
        }
        auto owner = access_.read(key_);
        if (!owner)
            return lux::cxx::unexpected(owner.error());
        auto read = owner->get().read();
        if (!read)
            return lux::cxx::unexpected(read.error());
        return read->withRead([&](const lux::material::MaterialSource& source) -> MaterialEditResult<void> {
            std::erase_if(selection_, [&](auto id) { return source.graph.node(id) == nullptr; });
            return {};
        });
    }
}
