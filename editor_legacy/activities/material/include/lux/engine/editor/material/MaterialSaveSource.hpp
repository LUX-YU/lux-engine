#pragma once
#include <lux/engine/editor/sessions/SessionStore.hpp>
#include <lux/engine/editor/material/MaterialSession.hpp>
#include <lux/engine/editor/material/MaterialPersistenceAccess.hpp>
#include <lux/engine/editor/persistence/SaveSource.hpp>
namespace lux::editor::material
{
    class MaterialSaveSource final : public persistence::ISaveSource
    {
    public:
        MaterialSaveSource(
            sessions::TSessionAccess<MaterialSession> access,
            sessions::TSessionKey<MaterialSession> key,
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
        sessions::TSessionAccess<MaterialSession> access_;
        sessions::TSessionKey<MaterialSession> key_;
        std::optional<persistence::WriteTarget> target_;
        sessions::BindingRevision target_binding_;
    };
}
