#include <lux/engine/editor/flowforge/FlowInteraction.hpp>
#include <algorithm>
#include <exception>

namespace lux::editor::flowforge
{
    FlowInteraction::FlowInteraction(
        sessions::TSessionAccess<FlowSession> access,
        sessions::TSessionKey<FlowSession> key
    ) noexcept
        : access_(access), key_(key)
    {}
    FlowInteraction::~FlowInteraction() noexcept
    {
        if (gesture_ && !cancel())
            std::terminate(); // Destruction inside an admitted callback violates the owner lifetime contract.
    }
    FlowEditResult<void> FlowInteraction::begin(std::string label)
    {
        if (gesture_)
            return lux::cxx::unexpected(sessions::ESessionError::BUSY);
        auto info = access_.describe(key_);
        if (!info)
            return lux::cxx::unexpected(info.error());
        if (info->admission != sessions::EEditAdmission::AVAILABLE)
            return lux::cxx::unexpected(sessions::ESessionError::BUSY);
        gesture_.emplace(FlowEditBatch{info->current, std::move(label), {}});
        return {};
    }
    FlowEditResult<void> FlowInteraction::preview(std::vector<VFlowEdit>& candidate)
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
        return read->withRead([&]() -> FlowEditResult<void> {
            gesture_->edits.swap(candidate);
            candidate.clear(); // Old preview payload and its code are released under the original gate.
            return {};
        });
    }
    FlowEditResult<FlowEditReceipt> FlowInteraction::commit()
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
    FlowEditResult<void> FlowInteraction::cancel()
    {
        if (!gesture_)
            return {};
        auto owner = access_.read(key_);
        if (!owner)
        {
            gesture_.reset(); // A closed/reused Store slot cannot be edited by an input destructor.
            return {};
        }
        auto read = owner->get().read();
        if (!read)
            return lux::cxx::unexpected(read.error());
        return read->withRead([&]() -> FlowEditResult<void> {
            gesture_.reset();
            return {};
        });
    }
    FlowEditResult<void> FlowInteraction::select(std::vector<lux::flowforge::NodeId> nodes)
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
        return read->withRead([&](const lux::flowforge::FlowSource& source) -> FlowEditResult<void> {
            for (auto id : nodes)
                if (!(std::ranges::any_of(source.nodes, [&](const auto& node) { return node.id == id; })))
                    return lux::cxx::unexpected(sessions::ESessionError::INVALID_ARGUMENT);
            selection_ = std::move(nodes);
            selection_history_ = info->current.state.history;
            return {};
        });
    }
    FlowEditResult<void> FlowInteraction::synchronize()
    {
        auto info = access_.describe(key_);
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
        return read->withRead([&](const lux::flowforge::FlowSource& source) -> FlowEditResult<void> {
            std::erase_if(selection_, [&](auto id) {
                return !(std::ranges::any_of(source.nodes, [&](const auto& node) { return node.id == id; }));
            });
            return {};
        });
    }
}
