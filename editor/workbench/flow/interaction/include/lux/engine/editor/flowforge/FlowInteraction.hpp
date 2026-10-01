#pragma once
#include <lux/engine/editor/flowforge/FlowSession.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <optional>
#include <span>

namespace lux::editor::flowforge
{
    // Owner-thread interaction only. The Store outlives this object. No live ReadView is retained.
    // Preview inputs are consumed only after admission. Finish/cancel before destroying inside a callback.
    class FlowInteraction final
    {
    public:
        FlowInteraction(sessions::TSessionAccess<FlowSession> access, sessions::TSessionKey<FlowSession> key) noexcept;
        ~FlowInteraction() noexcept;
        FlowInteraction(const FlowInteraction&) = delete;
        FlowInteraction& operator=(const FlowInteraction&) = delete;
        FlowInteraction(FlowInteraction&&) = delete;
        FlowInteraction& operator=(FlowInteraction&&) = delete;
        [[nodiscard]] FlowEditResult<void> begin(std::string label);
        [[nodiscard]] FlowEditResult<void> preview(std::vector<VFlowEdit>& candidate);
        [[nodiscard]] FlowEditResult<FlowEditReceipt> commit();
        [[nodiscard]] FlowEditResult<void> cancel();
        [[nodiscard]] FlowEditResult<void> synchronize();
        [[nodiscard]] sessions::TSessionKey<FlowSession> session() const noexcept
        {
            return key_;
        }
        [[nodiscard]] const FlowEditBatch* overlay() const noexcept
        {
            return gesture_ ? &*gesture_ : nullptr;
        }
        [[nodiscard]] FlowEditResult<void> select(std::vector<lux::flowforge::NodeId> nodes);
        [[nodiscard]] std::span<const lux::flowforge::NodeId> selection() const noexcept
        {
            return selection_;
        }

    private:
        sessions::TSessionAccess<FlowSession> access_;
        sessions::TSessionKey<FlowSession> key_;
        std::optional<FlowEditBatch> gesture_;
        std::vector<lux::flowforge::NodeId> selection_;
        // Validation provenance for selection_, not another author current or an admission token.
        sessions::ContentStamp selection_source_;
    };
}
