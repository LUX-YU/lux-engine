#pragma once
#include <lux/engine/editor/material/MaterialSession.hpp>
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <optional>
#include <span>

namespace lux::editor::material
{
    // Owner-thread interaction only. The Store outlives this object. No live ReadView is retained.
    // Preview inputs are consumed only after admission. Finish/cancel before destroying inside a callback.
    class MaterialInteraction final
    {
    public:
        MaterialInteraction(
            sessions::TSessionAccess<MaterialSession> access,
            sessions::TSessionKey<MaterialSession> key
        ) noexcept;
        ~MaterialInteraction() noexcept;
        MaterialInteraction(const MaterialInteraction&) = delete;
        MaterialInteraction& operator=(const MaterialInteraction&) = delete;
        MaterialInteraction(MaterialInteraction&&) = delete;
        MaterialInteraction& operator=(MaterialInteraction&&) = delete;
        [[nodiscard]] MaterialEditResult<void> begin(std::string label);
        [[nodiscard]] MaterialEditResult<void> preview(std::vector<VMaterialEdit>& candidate);
        [[nodiscard]] MaterialEditResult<MaterialEditReceipt> commit();
        [[nodiscard]] MaterialEditResult<void> cancel();
        [[nodiscard]] MaterialEditResult<void> synchronize();
        [[nodiscard]] const MaterialEditBatch* overlay() const noexcept
        {
            return gesture_ ? &*gesture_ : nullptr;
        }
        [[nodiscard]] MaterialEditResult<void> select(std::vector<lux::material::NodeId> nodes);
        [[nodiscard]] std::span<const lux::material::NodeId> selection() const noexcept
        {
            return selection_;
        }

    private:
        sessions::TSessionAccess<MaterialSession> access_;
        sessions::TSessionKey<MaterialSession> key_;
        std::optional<MaterialEditBatch> gesture_;
        std::vector<lux::material::NodeId> selection_;
        editing::HistoryId selection_history_;
    };
}
