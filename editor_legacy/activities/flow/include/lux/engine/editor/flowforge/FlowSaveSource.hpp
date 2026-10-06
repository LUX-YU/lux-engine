#pragma once
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/flowforge/FlowSession.hpp>
#include <lux/engine/editor/flowforge/FlowPersistenceAccess.hpp>
#include <lux/engine/editor/persistence/SaveSource.hpp>
namespace lux::editor::flowforge
{
    class FlowSaveSource final : public persistence::ISaveSource
    {
    public:
        FlowSaveSource(
            sessions::TSessionAccess<FlowSession> access,
            sessions::TSessionKey<FlowSession> key,
            std::optional<persistence::WriteTarget> target,
            sessions::BindingRevision binding
        ) noexcept
            : access_(access), key_(key), target_(std::move(target)), target_binding_(binding)
        {}
        [[nodiscard]] persistence::PersistenceResult<persistence::SaveSourceInfo> describe() const override;
        [[nodiscard]] persistence::PersistenceResult<persistence::FrozenSave> captureForSave(
            const persistence::SaveSourceInfo&,
            const persistence::SaveRequest&,
            std::size_t max_bytes
        ) override;
        [[nodiscard]] persistence::EAdoption accept(persistence::SaveReceipt&&) noexcept override;

    private:
        sessions::TSessionAccess<FlowSession> access_;
        sessions::TSessionKey<FlowSession> key_;
        std::optional<persistence::WriteTarget> target_;
        sessions::BindingRevision target_binding_;
    };
}
